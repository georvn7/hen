#!/usr/bin/env python3
"""
Generate debugger DPO pairs from distilled step datasets.

Input:
- a dataset leaf directory containing step_*.json files, or
- a root directory containing many such leaves.

Outputs written per dataset leaf:
- train_dbg_dpo.jsonl
- step_<start>_<current>_prefer.json
- step_<start>_<current>_reject.json
- step_<start>_<current>_pair.txt

The JSONL format follows the explicit conversational preference shape supported
by TRL/Hugging Face workflows:
{
  "prompt":   [{"role": "...", "content": "..."}, ...],
  "chosen":   [{"role": "assistant", "content": "..."}],
  "rejected": [{"role": "assistant", "content": "..."}],
  "meta":     {...}
}
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple
from urllib import error, request


STEP_RE = re.compile(r"^step_(\d+)_(\d+)\.json$")
OPT_RE = re.compile(r"^optimized_fix_(\d+)_(\d+)\.json$")

INFO_ACTIONS = {
    "function_info",
    "data_info",
    "file_info",
    "functions_summary",
    "call_graph",
    "log_info",
    "search_source",
    "step_info",
}

VALID_ACTIONS = {
    "log_info",
    "function_info",
    "data_info",
    "file_info",
    "functions_summary",
    "call_graph",
    "search_source",
    "fix_function",
    "refactor_data",
    "new_data",
    "debug_function",
    "run_test",
}

STRONG_REASONS = {
    "invalid_json",
    "missing_action_type",
    "missing_action_subject",
    "missing_breakpoints",
    "missing_invocation",
    "missing_line_number",
    "missing_motivation",
    "empty_action_type",
    "unknown_action_type",
    "breakpoints_without_debug_function",
    "invalid_fix_function_subject",
    "duplicate_in_prompt_history",
    "duplicate_info_request",
}


@dataclass
class ActionKey:
    action_type: str = ""
    action_subject: str = ""

    @property
    def normalized_type(self) -> str:
        return self.action_type.strip()

    @property
    def normalized_subject(self) -> str:
        return self.action_subject.strip()

    def is_same_step(self, other: "ActionKey") -> bool:
        return (
            self.normalized_type == other.normalized_type
            and self.normalized_subject == other.normalized_subject
        )


@dataclass(frozen=True)
class StepSignature:
    action_type: str = ""
    action_subject: str = ""
    invocation: int = 1
    line_number: int = 0

    @property
    def normalized_type(self) -> str:
        return self.action_type.strip()

    @property
    def normalized_subject(self) -> str:
        return self.action_subject.strip()


@dataclass
class TrackStepMeta:
    start_step: int
    fix_step: int
    current_step: int
    original_step: int
    inserted: bool
    compression_ratio: float
    final_fix_subject: str = ""
    original_track_present: bool = False
    mapped_original_signature: Optional[StepSignature] = None
    original_info_signatures: Tuple[StepSignature, ...] = ()
    original_fix_subjects: Tuple[str, ...] = ()


@dataclass
class Candidate:
    raw_content: str
    action: ActionKey
    parsed: Optional[dict]
    reasons: List[str] = field(default_factory=list)
    score: int = 0
    signature: Optional[StepSignature] = None
    matches_original_step_exact: bool = False
    matches_original_fix_subject: bool = False


@dataclass
class SelectionTrace:
    sample_outcomes: List[str] = field(default_factory=list)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "dataset_root",
        help="Dataset leaf directory or a root containing multiple dataset leaves.",
    )
    parser.add_argument(
        "--api-base",
        required=True,
        help="OpenAI-compatible API base, e.g. http://10.0.0.34:8002/v1",
    )
    parser.add_argument(
        "--endpoint-path",
        default="/chat/completions",
        help="Endpoint path appended to --api-base. Default: /chat/completions",
    )
    parser.add_argument(
        "--model",
        required=True,
        help="Model name to sample rejects from.",
    )
    parser.add_argument(
        "--api-key",
        default="",
        help="Optional bearer token. Empty is fine for local vLLM.",
    )
    parser.add_argument(
        "--samples-per-step",
        type=int,
        default=3,
        help="How many reject candidates to sample per step.",
    )
    parser.add_argument(
        "--temperature",
        type=float,
        default=0.4,
        help="Sampling temperature for reject generation.",
    )
    parser.add_argument(
        "--max-completion-tokens",
        type=int,
        default=1400,
        help="Max completion tokens for reject generation.",
    )
    parser.add_argument(
        "--timeout-sec",
        type=int,
        default=120,
        help="HTTP timeout per request in seconds.",
    )
    parser.add_argument(
        "--request-retries",
        type=int,
        default=3,
        help="Retry count for transient generation failures.",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Overwrite existing prefer/reject/pair files and train_dbg_dpo.jsonl.",
    )
    parser.add_argument(
        "--max-steps",
        type=int,
        default=0,
        help="Optional limit for the number of step_*.json files processed per leaf.",
    )
    parser.add_argument(
        "--drop-invalid-rejects",
        action="store_true",
        help="Skip malformed/unparseable rejects instead of keeping them.",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Print per-step decisions while generating pairs.",
    )
    return parser.parse_args()


def load_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def save_text(path: Path, text: str) -> None:
    path.write_text(text, encoding="utf-8")


def sanitize_message(message: dict) -> dict:
    sanitized = {
        "role": str(message.get("role", "")),
        "content": str(message.get("content", "")),
    }
    return sanitized


def sanitize_messages(messages: Sequence[dict]) -> List[dict]:
    return [sanitize_message(msg) for msg in messages]


def parse_action_from_text(text: str) -> Tuple[Optional[dict], ActionKey]:
    stripped = text.strip()
    if not stripped:
        return None, ActionKey()

    try:
        parsed = json.loads(stripped)
    except json.JSONDecodeError:
        return None, ActionKey()

    if not isinstance(parsed, dict):
        return None, ActionKey()

    return parsed, ActionKey(
        action_type=str(parsed.get("action_type", "")),
        action_subject=str(parsed.get("action_subject", "")),
    )


def normalize_int(value, default: int) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def parse_signature_from_step(step: dict) -> Optional[StepSignature]:
    if not isinstance(step, dict):
        return None
    action_type = str(step.get("action_type", "")).strip()
    action_subject = str(step.get("action_subject", "")).strip()
    if not action_type:
        return None
    invocation = normalize_int(step.get("invocation", 1), 1)
    line_number = normalize_int(step.get("line_number", 0), 0)
    if invocation <= 0:
        invocation = 1
    if line_number < 0:
        line_number = 0
    return StepSignature(
        action_type=action_type,
        action_subject=action_subject,
        invocation=invocation,
        line_number=line_number,
    )


def parse_candidate_signature(parsed: Optional[dict]) -> Optional[StepSignature]:
    if not isinstance(parsed, dict):
        return None
    required = ("action_type", "action_subject", "invocation", "line_number")
    if any(field not in parsed for field in required):
        return None
    return parse_signature_from_step(parsed)


def load_original_track_meta(path: Path) -> Tuple[Dict[int, StepSignature], Tuple[StepSignature, ...], Tuple[str, ...], bool]:
    payload = load_json(path)
    raw_steps = payload.get("steps", [])
    if not isinstance(raw_steps, list) or not raw_steps:
        return {}, (), (), False

    by_original_step: Dict[int, StepSignature] = {}
    original_info_signatures: List[StepSignature] = []
    original_fix_subjects: List[str] = []

    for step in raw_steps:
        if not isinstance(step, dict):
            continue
        signature = parse_signature_from_step(step)
        if signature is None:
            continue

        original_step = normalize_int(step.get("original_step", -1), -1)
        if original_step > 0:
            by_original_step[original_step] = signature

        if signature.normalized_type in INFO_ACTIONS:
            original_info_signatures.append(signature)

        if signature.normalized_type == "fix_function" and signature.normalized_subject:
            original_fix_subjects.append(signature.normalized_subject)

    return (
        by_original_step,
        tuple(original_info_signatures),
        tuple(sorted(set(original_fix_subjects))),
        True,
    )


def discover_leaf_dirs(root: Path) -> List[Path]:
    if any(STEP_RE.match(p.name) for p in root.iterdir() if p.is_file()):
        return [root]

    leaves: List[Path] = []
    for current, _, files in os.walk(root):
        if any(STEP_RE.match(name) for name in files):
            leaves.append(Path(current))
    leaves.sort()
    return leaves


def build_track_index(leaf_dir: Path) -> Dict[str, TrackStepMeta]:
    step_meta: Dict[str, TrackStepMeta] = {}

    for path in sorted(leaf_dir.iterdir()):
        match = OPT_RE.match(path.name)
        if not match:
            continue

        start_step = int(match.group(1))
        fix_step = int(match.group(2))
        original_path = leaf_dir / f"original_fix_{start_step}_{fix_step}.json"
        (
            original_by_step,
            original_info_signatures,
            original_fix_subjects,
            original_track_present,
        ) = ({}, (), (), False)
        if original_path.exists():
            (
                original_by_step,
                original_info_signatures,
                original_fix_subjects,
                original_track_present,
            ) = load_original_track_meta(original_path)

        payload = load_json(path)
        raw_steps = payload.get("steps", [])
        if not isinstance(raw_steps, list) or not raw_steps:
            continue

        optimized_steps = raw_steps[:]
        if (
            optimized_steps
            and isinstance(optimized_steps[0], dict)
            and str(optimized_steps[0].get("action_type", "")).strip() == "run_test"
        ):
            optimized_steps = optimized_steps[1:]

        if not optimized_steps:
            continue

        compression_ratio = float(max(fix_step - start_step, 1)) / float(len(optimized_steps))
        final_fix_subject = ""
        last_step = optimized_steps[-1]
        if isinstance(last_step, dict) and str(last_step.get("action_type", "")).strip() == "fix_function":
            final_fix_subject = str(last_step.get("action_subject", "")).strip()

        for index, step in enumerate(optimized_steps, start=1):
            if not isinstance(step, dict):
                continue

            current_step = start_step + index
            original_step = int(step.get("original_step", -1))
            inserted = original_step <= 0

            step_name = f"step_{start_step}_{current_step}"
            step_meta[step_name] = TrackStepMeta(
                start_step=start_step,
                fix_step=fix_step,
                current_step=current_step,
                original_step=original_step,
                inserted=inserted,
                compression_ratio=compression_ratio,
                final_fix_subject=final_fix_subject,
                original_track_present=original_track_present,
                mapped_original_signature=original_by_step.get(original_step),
                original_info_signatures=original_info_signatures,
                original_fix_subjects=original_fix_subjects,
            )

    return step_meta


def collect_prompt_history_actions(messages: Sequence[dict]) -> List[ActionKey]:
    actions: List[ActionKey] = []
    for message in messages:
        if str(message.get("role", "")) != "assistant":
            continue
        parsed, action = parse_action_from_text(str(message.get("content", "")))
        if parsed is not None:
            actions.append(action)
    return actions


def is_duplicate_in_history(candidate: ActionKey, history: Sequence[ActionKey]) -> bool:
    return any(candidate.is_same_step(prev) for prev in history)


def has_bad_schema(parsed: dict) -> List[str]:
    reasons: List[str] = []
    for field_name in ("action_type", "action_subject", "breakpoints", "invocation", "line_number", "motivation"):
        if field_name not in parsed:
            reasons.append(f"missing_{field_name}")

    action_type = str(parsed.get("action_type", ""))
    if not action_type.strip():
        reasons.append("empty_action_type")
    elif action_type.strip() not in VALID_ACTIONS:
        reasons.append("unknown_action_type")

    if action_type.strip() != "debug_function" and parsed.get("breakpoints"):
        reasons.append("breakpoints_without_debug_function")

    action_subject = str(parsed.get("action_subject", "")).strip()
    if action_type.strip() == "fix_function" and (not action_subject or action_subject == "none"):
        reasons.append("invalid_fix_function_subject")

    return reasons


def score_candidate(
    candidate_text: str,
    chosen_action: ActionKey,
    prompt_history: Sequence[ActionKey],
    track_meta: Optional[TrackStepMeta],
    keep_invalid_rejects: bool,
) -> Tuple[Optional[Candidate], str]:
    parsed, action = parse_action_from_text(candidate_text)
    candidate = Candidate(
        raw_content=candidate_text,
        action=action,
        parsed=parsed,
        signature=parse_candidate_signature(parsed),
    )

    if parsed is None:
        if not keep_invalid_rejects:
            return None, "invalid_json_dropped"
        candidate.reasons.append("invalid_json")
        candidate.score += 4
        return candidate, "invalid_json"

    if action.is_same_step(chosen_action):
        return None, "same_as_preferred"

    if (
        track_meta is not None
        and track_meta.current_step < track_meta.fix_step
        and action.normalized_type == "fix_function"
        and action.normalized_subject
        and action.normalized_subject == track_meta.final_fix_subject
    ):
        return None, "grounded_shortcut_to_final_fix"

    if track_meta is not None and candidate.signature is not None:
        if candidate.signature == track_meta.mapped_original_signature:
            candidate.matches_original_step_exact = True
        if (
            candidate.signature.normalized_type in INFO_ACTIONS
            and candidate.signature in track_meta.original_info_signatures
        ):
            return None, "valid_original_info_alternative"
        if (
            candidate.signature.normalized_type == "fix_function"
            and candidate.signature.normalized_subject in track_meta.original_fix_subjects
        ):
            candidate.matches_original_fix_subject = True

    schema_reasons = has_bad_schema(parsed)
    candidate.reasons.extend(schema_reasons)
    if schema_reasons:
        candidate.score += 4

    if action.normalized_type != chosen_action.normalized_type:
        candidate.reasons.append("action_type_mismatch")
        candidate.score += 2

    if action.normalized_subject != chosen_action.normalized_subject:
        candidate.reasons.append("action_subject_mismatch")
        candidate.score += 1

    if is_duplicate_in_history(action, prompt_history):
        candidate.reasons.append("duplicate_in_prompt_history")
        candidate.score += 2

    if action.normalized_type in INFO_ACTIONS and is_duplicate_in_history(action, prompt_history):
        candidate.reasons.append("duplicate_info_request")
        candidate.score += 1

    if track_meta is not None:
        if track_meta.inserted:
            candidate.reasons.append("inserted_optimized_context")
            candidate.score += 1
        if track_meta.compression_ratio >= 1.5:
            candidate.reasons.append("high_optimization_pressure")
            candidate.score += 1

    if not candidate.reasons:
        return None, "no_scoring_reasons"

    if not any(reason in STRONG_REASONS for reason in candidate.reasons):
        return None, "only_weak_reasons"

    return candidate, ",".join(candidate.reasons)


def post_chat_completions(
    api_base: str,
    endpoint_path: str,
    api_key: str,
    model: str,
    messages: Sequence[dict],
    temperature: float,
    max_completion_tokens: int,
    timeout_sec: int,
) -> str:
    url = api_base.rstrip("/") + "/" + endpoint_path.strip("/")
    payload = {
        "model": model,
        "messages": list(messages),
        "temperature": temperature,
        "max_tokens": max_completion_tokens,
    }

    body = json.dumps(payload).encode("utf-8")
    headers = {"Content-Type": "application/json"}
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"

    req = request.Request(url, data=body, headers=headers, method="POST")
    try:
        with request.urlopen(req, timeout=timeout_sec) as response:
            raw = response.read().decode("utf-8")
    except error.HTTPError as exc:
        details = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP {exc.code} from {url}: {details}") from exc
    except error.URLError as exc:
        raise RuntimeError(f"Unable to reach {url}: {exc}") from exc

    payload = json.loads(raw)
    choices = payload.get("choices", [])
    if not choices:
        raise RuntimeError("Missing choices in chat completion response")

    message = choices[0].get("message", {})
    if not isinstance(message, dict):
        raise RuntimeError("Missing message object in chat completion response")

    content = message.get("content", "")
    if isinstance(content, list):
        parts: List[str] = []
        for item in content:
            if isinstance(item, dict) and item.get("type") == "text":
                parts.append(str(item.get("text", "")))
            elif isinstance(item, str):
                parts.append(item)
        content = "".join(parts)

    if not isinstance(content, str):
        raise RuntimeError("Unsupported message.content shape in chat completion response")

    return content


def choose_best_reject(
    prompt_messages: Sequence[dict],
    chosen_action: ActionKey,
    prompt_history: Sequence[ActionKey],
    track_meta: Optional[TrackStepMeta],
    args: argparse.Namespace,
) -> Tuple[Optional[Candidate], SelectionTrace]:
    best: Optional[Candidate] = None
    trace = SelectionTrace()
    sanitized_prompt = sanitize_messages(prompt_messages)

    for _ in range(args.samples_per_step):
        last_error: Optional[Exception] = None
        raw = ""
        for _attempt in range(args.request_retries):
            try:
                raw = post_chat_completions(
                    api_base=args.api_base,
                    endpoint_path=args.endpoint_path,
                    api_key=args.api_key,
                    model=args.model,
                    messages=sanitized_prompt,
                    temperature=args.temperature,
                    max_completion_tokens=args.max_completion_tokens,
                    timeout_sec=args.timeout_sec,
                )
                last_error = None
                break
            except RuntimeError as exc:
                last_error = exc
                time.sleep(0.5)

        if last_error is not None:
            raise last_error

        candidate, outcome = score_candidate(
            candidate_text=raw,
            chosen_action=chosen_action,
            prompt_history=prompt_history,
            track_meta=track_meta,
            keep_invalid_rejects=not args.drop_invalid_rejects,
        )
        trace.sample_outcomes.append(outcome)

        if candidate is None:
            continue

        if best is None or candidate.score > best.score:
            best = candidate

    return best, trace


def render_messages(messages: Sequence[dict]) -> str:
    lines: List[str] = []
    for message in messages:
        role = str(message.get("role", ""))
        content = str(message.get("content", ""))
        lines.append(f">> {role}\n\n\n{content}\n")
    return "\n".join(lines).rstrip() + "\n"


def render_pair_text(prompt_messages: Sequence[dict], chosen_message: dict, rejected_message: dict, meta: dict) -> str:
    rendered = render_messages(prompt_messages)
    rendered += "\n>> preferred_assistant\n\n\n"
    rendered += str(chosen_message.get("content", "")) + "\n"
    rendered += "\n>> rejected_assistant\n\n\n"
    rendered += str(rejected_message.get("content", "")) + "\n"
    rendered += "\n>> meta\n\n\n"
    rendered += json.dumps(meta, indent=2, ensure_ascii=True) + "\n"
    return rendered


def process_step_file(
    step_path: Path,
    leaf_dir: Path,
    step_meta_index: Dict[str, TrackStepMeta],
    train_writer,
    args: argparse.Namespace,
) -> bool:
    sample = load_json(step_path)
    messages = sample.get("messages", [])
    if not isinstance(messages, list) or len(messages) < 2:
        return False

    prompt_messages = sanitize_messages(messages[:-1])
    chosen_raw = messages[-1]
    chosen_message = sanitize_message(chosen_raw)
    chosen_parsed, chosen_action = parse_action_from_text(chosen_message["content"])
    if chosen_parsed is None:
        return False

    prompt_history = collect_prompt_history_actions(prompt_messages)
    step_name = step_path.stem
    track_meta = step_meta_index.get(step_name)

    reject, trace = choose_best_reject(
        prompt_messages=prompt_messages,
        chosen_action=chosen_action,
        prompt_history=prompt_history,
        track_meta=track_meta,
        args=args,
    )
    if reject is None:
        if args.verbose:
            print(f"[skip] {step_name}: outcomes={';'.join(trace.sample_outcomes)}")
        return False

    rejected_message = {
        "role": "assistant",
        "content": reject.raw_content.strip(),
    }

    meta = {
        "dataset_leaf": str(leaf_dir),
        "sample": step_name,
        "model": args.model,
        "preferred_action_type": chosen_action.normalized_type,
        "preferred_action_subject": chosen_action.normalized_subject,
        "rejected_action_type": reject.action.normalized_type,
        "rejected_action_subject": reject.action.normalized_subject,
        "reject_reasons": reject.reasons,
        "reject_score": reject.score,
        "original_track_present": track_meta.original_track_present if track_meta is not None else False,
        "matches_original_step_exact": reject.matches_original_step_exact,
        "matches_original_fix_subject": reject.matches_original_fix_subject,
    }
    if track_meta is not None:
        meta.update(
            {
                "start_step": track_meta.start_step,
                "current_step": track_meta.current_step,
                "fix_step": track_meta.fix_step,
                "original_step": track_meta.original_step,
                "inserted_optimized_step": track_meta.inserted,
                "compression_ratio": track_meta.compression_ratio,
            }
        )

    prefer_file = leaf_dir / f"{step_name}_prefer.json"
    reject_file = leaf_dir / f"{step_name}_reject.json"
    pair_file = leaf_dir / f"{step_name}_pair.txt"

    if not args.overwrite and (prefer_file.exists() or reject_file.exists() or pair_file.exists()):
        raise RuntimeError(
            f"Refusing to overwrite existing pair files for {step_name}. Use --overwrite."
        )

    prefer_payload = {"messages": prompt_messages + [chosen_message]}
    reject_payload = {"messages": prompt_messages + [rejected_message]}

    save_text(prefer_file, json.dumps(prefer_payload, ensure_ascii=True))
    save_text(reject_file, json.dumps(reject_payload, ensure_ascii=True))
    save_text(pair_file, render_pair_text(prompt_messages, chosen_message, rejected_message, meta))

    dpo_record = {
        "prompt": prompt_messages,
        "chosen": [chosen_message],
        "rejected": [rejected_message],
        "meta": meta,
    }
    train_writer.write(json.dumps(dpo_record, ensure_ascii=True) + "\n")

    if args.verbose:
        print(
            f"[DPO] {step_name}: prefer={chosen_action.normalized_type}/{chosen_action.normalized_subject} "
            f"reject={reject.action.normalized_type}/{reject.action.normalized_subject} "
            f"reasons={','.join(reject.reasons)}"
        )

    return True


def process_leaf_dir(leaf_dir: Path, args: argparse.Namespace) -> Tuple[int, int]:
    step_meta_index = build_track_index(leaf_dir)
    step_files = sorted(
        path for path in leaf_dir.iterdir() if path.is_file() and STEP_RE.match(path.name)
    )
    if args.max_steps > 0:
        step_files = step_files[: args.max_steps]

    output_jsonl = leaf_dir / "train_dbg_dpo.jsonl"
    if output_jsonl.exists() and not args.overwrite:
        raise RuntimeError(f"Refusing to overwrite existing {output_jsonl}. Use --overwrite.")

    kept = 0
    total = 0
    with output_jsonl.open("w", encoding="utf-8") as train_writer:
        for step_path in step_files:
            total += 1
            if process_step_file(step_path, leaf_dir, step_meta_index, train_writer, args):
                kept += 1

    return kept, total


def main() -> int:
    args = parse_args()
    root = Path(args.dataset_root).expanduser().resolve()
    if not root.exists():
        print(f"Dataset path does not exist: {root}", file=sys.stderr)
        return 2

    leaf_dirs = discover_leaf_dirs(root)
    if not leaf_dirs:
        print(f"No dataset leaves with step_*.json found under: {root}", file=sys.stderr)
        return 2

    grand_total = 0
    grand_kept = 0
    start_time = time.time()

    for leaf_dir in leaf_dirs:
        kept, total = process_leaf_dir(leaf_dir, args)
        grand_kept += kept
        grand_total += total
        print(f"[leaf] {leaf_dir} kept {kept}/{total} DPO pairs")

    elapsed = time.time() - start_time
    print(f"[done] kept {grand_kept}/{grand_total} DPO pairs in {elapsed:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
