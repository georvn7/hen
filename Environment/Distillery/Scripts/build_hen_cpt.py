#!/usr/bin/env python3
"""
Build continued-pretraining JSONL corpora from hen JSON logs.

The exporter intentionally reads JSON logs only. It uses response_*.json files
as the primary text source. Request messages can optionally be included after
frequency-based boilerplate filtering, so dynamic grounded context is preserved
without repeating static prompt contracts thousands of times.

Outputs:
- hen-cpt-debug.jsonl: grouped debugger-step documents from logs/debug
- hen-cpt-nodes.jsonl: optional node/codegen documents from logs/nodes
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from hashlib import sha1
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple


RESPONSE_RE = re.compile(r"^response_(\d+)_([A-Za-z0-9_]+)\.json$")
STEP_RE = re.compile(r"^step_(\d+)$")

DEFAULT_DEBUG_PROMPTS = (
    "SystemAnalysis",
    "SystemDebugAnalysis",
    "SpecialFramesAnalysis",
    "TraceAnalysis",
    "LLDBAnalysis",
    "RunAnalysis",
    "NextStep",
    "ReviewFixStep",
    "RewardHacking",
    "SummarizeTrajectory",
)

DEFAULT_NODE_PROMPTS = (
    "Programming",
    "ImplementSource",
    "ReviewSource",
    "FixCompilation",
    "ReviewCompilation",
    "ReviewCompilationLib",
    "ReviewCompilationSelf",
    "ReviewCompilationLuck",
    "ReviewCompilationPanic",
    "ReviewCompilationOptions",
    "DefineFunction",
    "ListFunctions",
    "ReviewListFunctions",
    "CompareFunctions",
    "ReviewCompareFunctions",
    "ReviewFunction",
    "ReviewFunctionSelf",
    "DefineData",
    "ReviewData",
    "ReviewDataSelf",
    "ImplementTest",
    "ReviewTestSelf",
    "ReviewTestSource",
    "DefineTest",
)

DEBUG_SECTION_ORDER = (
    "SystemAnalysis",
    "SystemDebugAnalysis",
    "SpecialFramesAnalysis",
    "TraceAnalysis",
    "LLDBAnalysis",
    "RunAnalysis",
    "NextStep",
    "ReviewFixStep",
    "RewardHacking",
    "SummarizeTrajectory",
)

SECRET_PATTERNS = (
    re.compile(r"sk-[A-Za-z0-9_-]{20,}"),
    re.compile(r"gsk_[A-Za-z0-9_-]{20,}"),
    re.compile(r"(?i)(authorization\s*:\s*bearer\s+)[A-Za-z0-9._-]+"),
)

STATIC_HEADING_PATTERNS = (
    re.compile(r"^\s*PROJECT DESCRIPTION\b", re.I),
    re.compile(r"^\s*DEBUGGING WORKFLOW\b", re.I),
    re.compile(r"^\s*POSSIBLE NEXT ACTIONS\b", re.I),
    re.compile(r"^\s*PROJECT SOURCE CODE REQUIREMENTS\b", re.I),
    re.compile(r"^\s*OUTPUT REQUIREMENTS\b", re.I),
    re.compile(r"^\s*GENERATION CHECKLIST\b", re.I),
    re.compile(r"^\s*JSON SCHEMA\b", re.I),
    re.compile(r"^\s*Provide in your response only one top-level JSON", re.I),
    re.compile(r"^\s*Hey, you are a Large Language Model", re.I),
)


@dataclass
class ResponseRecord:
    run: str
    area: str
    prompt: str
    request_id: int
    content: str
    path: Path
    request_context: str = ""
    test: str = ""
    step: int = -1
    node: str = ""
    model: str = ""
    requested_llm: str = ""
    time: str = ""


@dataclass
class ExportStats:
    debug_docs: int = 0
    node_docs: int = 0
    responses_seen: int = 0
    responses_kept: int = 0
    responses_skipped_prompt: int = 0
    responses_skipped_empty: int = 0
    responses_skipped_parse: int = 0
    request_context_messages_seen: int = 0
    request_context_messages_kept: int = 0
    request_context_messages_dropped_stable: int = 0
    request_context_messages_dropped_heading: int = 0
    request_context_messages_truncated: int = 0
    docs_truncated: int = 0
    output_debug: str = ""
    output_nodes: str = ""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "log_roots",
        nargs="+",
        help="One or more hen logs directories, e.g. SimpleC/full_*/logs.",
    )
    parser.add_argument(
        "--output-dir",
        default=".",
        help="Directory for generated CPT JSONL files.",
    )
    parser.add_argument(
        "--debug-output",
        default="hen-cpt-debug.jsonl",
        help="Filename for debugger CPT JSONL.",
    )
    parser.add_argument(
        "--nodes-output",
        default="hen-cpt-nodes.jsonl",
        help="Filename for optional node/codegen CPT JSONL.",
    )
    parser.add_argument(
        "--include-nodes",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Emit logs/nodes corpus. Enabled by default.",
    )
    parser.add_argument(
        "--debug-prompts",
        default=",".join(DEFAULT_DEBUG_PROMPTS),
        help="Comma-separated response prompt names to keep from logs/debug.",
    )
    parser.add_argument(
        "--node-prompts",
        default=",".join(DEFAULT_NODE_PROMPTS),
        help="Comma-separated response prompt names to keep from logs/nodes.",
    )
    parser.add_argument(
        "--include-reasoning",
        action="store_true",
        help="Append message.reasoning when present. Disabled by default.",
    )
    parser.add_argument(
        "--include-dynamic-request-context",
        action=argparse.BooleanOptionalAction,
        default=False,
        help=(
            "Include dynamic request messages after dropping stable prompt "
            "boilerplate. Disabled by default for response-only export."
        ),
    )
    parser.add_argument(
        "--stable-message-min-count",
        type=int,
        default=20,
        help=(
            "Request message fingerprints seen at least this many times are "
            "treated as stable boilerplate. Used with --include-dynamic-request-context."
        ),
    )
    parser.add_argument(
        "--max-request-context-chars",
        type=int,
        default=120000,
        help=(
            "Middle-truncate the dynamic request context attached to each "
            "response to this many chars. Set 0 to disable."
        ),
    )
    parser.add_argument(
        "--max-section-chars",
        type=int,
        default=60000,
        help="Middle-truncate individual response sections to this many chars. Set 0 to disable.",
    )
    parser.add_argument(
        "--max-doc-chars",
        type=int,
        default=180000,
        help="Middle-truncate full CPT documents to this many chars. Set 0 to disable.",
    )
    parser.add_argument(
        "--min-content-chars",
        type=int,
        default=20,
        help="Skip response contents shorter than this many non-space chars.",
    )
    parser.add_argument(
        "--redact-paths",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Replace /Users/<name>/ with /Users/<user>/ in emitted text.",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Overwrite existing output files.",
    )
    return parser.parse_args()


def comma_set(value: str) -> set[str]:
    return {item.strip() for item in value.split(",") if item.strip()}


def load_json(path: Path) -> Optional[dict]:
    try:
        with path.open("r", encoding="utf-8") as handle:
            payload = json.load(handle)
            return payload if isinstance(payload, dict) else None
    except (OSError, json.JSONDecodeError, UnicodeDecodeError):
        return None


def response_message(payload: dict) -> dict:
    message = payload.get("message")
    if isinstance(message, dict):
        return message

    choices = payload.get("choices")
    if isinstance(choices, list) and choices:
        first = choices[0]
        if isinstance(first, dict) and isinstance(first.get("message"), dict):
            return first["message"]

    return {}


def request_messages(payload: dict) -> List[dict]:
    messages = payload.get("messages")
    if not isinstance(messages, list):
        return []
    return [message for message in messages if isinstance(message, dict)]


def normalize_content(value) -> str:
    if isinstance(value, str):
        return value
    if isinstance(value, list):
        pieces: List[str] = []
        for item in value:
            if isinstance(item, str):
                pieces.append(item)
            elif isinstance(item, dict):
                text = item.get("text") or item.get("content")
                if isinstance(text, str):
                    pieces.append(text)
        return "\n".join(pieces)
    return ""


def normalize_for_fingerprint(text: str) -> str:
    return re.sub(r"\s+", " ", text).strip()


def request_message_fingerprint(text: str) -> str:
    return sha1(normalize_for_fingerprint(text).encode("utf-8")).hexdigest()


def has_static_heading(text: str) -> bool:
    sample = text.lstrip()
    return any(pattern.search(sample) for pattern in STATIC_HEADING_PATTERNS)


def redact_text(text: str, redact_paths: bool) -> str:
    redacted = text
    for pattern in SECRET_PATTERNS:
        if pattern.pattern.startswith("(?i)(authorization"):
            redacted = pattern.sub(r"\1<redacted>", redacted)
        else:
            redacted = pattern.sub("<redacted-secret>", redacted)
    if redact_paths:
        redacted = re.sub(r"/Users/[^/\s]+/", "/Users/<user>/", redacted)
    return redacted


def truncate_middle(text: str, limit: int) -> Tuple[str, bool]:
    if limit <= 0 or len(text) <= limit:
        return text, False
    marker = "\n\n[... truncated for CPT export ...]\n\n"
    side = max(0, (limit - len(marker)) // 2)
    return text[:side] + marker + text[-side:], True


def parse_response_filename(path: Path) -> Optional[Tuple[int, str]]:
    match = RESPONSE_RE.match(path.name)
    if not match:
        return None
    return int(match.group(1)), match.group(2)


def paired_request_path(response_path: Path, request_id: int, prompt: str) -> Path:
    return response_path.with_name(f"request_{request_id}_{prompt}.json")


def load_paired_request(response_path: Path, request_id: int, prompt: str) -> Optional[dict]:
    return load_json(paired_request_path(response_path, request_id, prompt))


def request_metadata(payload: Optional[dict], prompt: str) -> dict:
    if not payload:
        return {}
    return {
        "model": payload.get("llm", "") or "",
        "requested_llm": payload.get("requested_llm", "") or "",
        "time": payload.get("time", "") or "",
        "prompt": payload.get("prompt", "") or prompt,
        "node_dir": payload.get("node_dir", "") or "",
    }


def response_files_for_allowed_prompts(
    logs_root: Path,
    area: str,
    prompt_allow: set[str],
) -> Iterable[Path]:
    files = debug_response_files(logs_root) if area == "debug" else node_response_files(logs_root)
    for path in files:
        parsed = parse_response_filename(path)
        if parsed and parsed[1] in prompt_allow:
            yield path


def collect_request_fingerprint_counts(
    logs_roots: Sequence[Path],
    debug_prompt_allow: set[str],
    node_prompt_allow: set[str],
    include_nodes: bool,
) -> Dict[str, int]:
    counts: Dict[str, int] = {}
    areas = [("debug", debug_prompt_allow)]
    if include_nodes:
        areas.append(("nodes", node_prompt_allow))

    for logs_root in logs_roots:
        for area, prompt_allow in areas:
            for response_path in response_files_for_allowed_prompts(logs_root, area, prompt_allow):
                parsed = parse_response_filename(response_path)
                if not parsed:
                    continue
                request_id, prompt = parsed
                payload = load_paired_request(response_path, request_id, prompt)
                if not payload:
                    continue
                for message in request_messages(payload):
                    content = normalize_content(message.get("content")).strip()
                    if not content:
                        continue
                    fingerprint = request_message_fingerprint(content)
                    counts[fingerprint] = counts.get(fingerprint, 0) + 1
    return counts


def extract_dynamic_request_context(
    payload: Optional[dict],
    stable_counts: Dict[str, int],
    args: argparse.Namespace,
    stats: ExportStats,
) -> str:
    if not args.include_dynamic_request_context or not payload:
        return ""

    blocks: List[str] = []
    for message in request_messages(payload):
        role = str(message.get("role") or "unknown")
        content = normalize_content(message.get("content")).strip()
        if not content:
            continue

        stats.request_context_messages_seen += 1
        fingerprint = request_message_fingerprint(content)
        if args.stable_message_min_count > 0 and stable_counts.get(fingerprint, 0) >= args.stable_message_min_count:
            stats.request_context_messages_dropped_stable += 1
            continue
        if has_static_heading(content):
            stats.request_context_messages_dropped_heading += 1
            continue

        content = redact_text(content, args.redact_paths)
        blocks.append(f">> {role}\n{content}")
        stats.request_context_messages_kept += 1

    if not blocks:
        return ""

    context = "\n\n".join(blocks)
    context, truncated = truncate_middle(context, args.max_request_context_chars)
    if truncated:
        stats.request_context_messages_truncated += 1
    return context


def run_name_for_logs(logs_root: Path) -> str:
    if logs_root.name == "logs":
        return logs_root.parent.name
    return logs_root.name


def discover_log_roots(paths: Sequence[str]) -> List[Path]:
    roots: List[Path] = []
    for raw in paths:
        path = Path(raw).expanduser().resolve()
        if (path / "debug").is_dir() or (path / "nodes").is_dir():
            roots.append(path)
        elif path.name != "logs" and (path / "logs").is_dir():
            roots.append((path / "logs").resolve())
        else:
            raise SystemExit(f"Not a hen logs root: {path}")
    return roots


def debug_response_files(logs_root: Path) -> Iterable[Path]:
    debug_root = logs_root / "debug"
    if not debug_root.is_dir():
        return []
    return debug_root.glob("*/step_*/response_*.json")


def node_response_files(logs_root: Path) -> Iterable[Path]:
    nodes_root = logs_root / "nodes"
    if not nodes_root.is_dir():
        return []
    return nodes_root.glob("*/response_*.json")


def parse_debug_path(logs_root: Path, response_path: Path) -> Tuple[str, int]:
    rel = response_path.relative_to(logs_root / "debug")
    parts = rel.parts
    test = parts[0] if parts else ""
    step = -1
    if len(parts) >= 2:
        match = STEP_RE.match(parts[1])
        if match:
            step = int(match.group(1))
    return test, step


def parse_node_path(logs_root: Path, response_path: Path) -> str:
    rel = response_path.relative_to(logs_root / "nodes")
    return rel.parts[0] if rel.parts else ""


def build_record(
    logs_root: Path,
    response_path: Path,
    area: str,
    args: argparse.Namespace,
    stable_counts: Dict[str, int],
    stats: ExportStats,
) -> Tuple[Optional[ResponseRecord], str]:
    parsed_name = parse_response_filename(response_path)
    if not parsed_name:
        return None, "parse"
    request_id, prompt = parsed_name

    payload = load_json(response_path)
    if not payload:
        return None, "parse"

    message = response_message(payload)
    content = normalize_content(message.get("content"))
    if args.include_reasoning:
        reasoning = normalize_content(message.get("reasoning"))
        if reasoning:
            content = content + "\n\nREASONING:\n" + reasoning

    content = redact_text(content.strip(), args.redact_paths)
    if len(content.strip()) < args.min_content_chars:
        return None, "empty"

    request_payload = load_paired_request(response_path, request_id, prompt)
    meta = request_metadata(request_payload, prompt)
    record = ResponseRecord(
        run=run_name_for_logs(logs_root),
        area=area,
        prompt=meta.get("prompt") or prompt,
        request_id=request_id,
        content=content,
        path=response_path,
        request_context=extract_dynamic_request_context(request_payload, stable_counts, args, stats),
        model=meta.get("model", ""),
        requested_llm=meta.get("requested_llm", ""),
        time=meta.get("time", ""),
    )
    if area == "debug":
        record.test, record.step = parse_debug_path(logs_root, response_path)
    elif area == "nodes":
        record.node = parse_node_path(logs_root, response_path)
    return record, ""


def collect_debug_records(
    logs_roots: Sequence[Path],
    prompt_allow: set[str],
    args: argparse.Namespace,
    stats: ExportStats,
    stable_counts: Dict[str, int],
) -> Dict[Tuple[str, str, int], List[ResponseRecord]]:
    grouped: Dict[Tuple[str, str, int], List[ResponseRecord]] = {}
    for logs_root in logs_roots:
        for path in debug_response_files(logs_root):
            stats.responses_seen += 1
            parsed = parse_response_filename(path)
            if not parsed:
                stats.responses_skipped_parse += 1
                continue
            if parsed[1] not in prompt_allow:
                stats.responses_skipped_prompt += 1
                continue
            record, reason = build_record(
                logs_root,
                path,
                "debug",
                args,
                stable_counts,
                stats,
            )
            if not record:
                if reason == "empty":
                    stats.responses_skipped_empty += 1
                else:
                    stats.responses_skipped_parse += 1
                continue
            stats.responses_kept += 1
            grouped.setdefault((record.run, record.test, record.step), []).append(record)
    return grouped


def collect_node_records(
    logs_roots: Sequence[Path],
    prompt_allow: set[str],
    args: argparse.Namespace,
    stats: ExportStats,
    stable_counts: Dict[str, int],
) -> List[ResponseRecord]:
    records: List[ResponseRecord] = []
    for logs_root in logs_roots:
        for path in node_response_files(logs_root):
            stats.responses_seen += 1
            parsed = parse_response_filename(path)
            if not parsed:
                stats.responses_skipped_parse += 1
                continue
            if parsed[1] not in prompt_allow:
                stats.responses_skipped_prompt += 1
                continue
            record, reason = build_record(
                logs_root,
                path,
                "nodes",
                args,
                stable_counts,
                stats,
            )
            if not record:
                if reason == "empty":
                    stats.responses_skipped_empty += 1
                else:
                    stats.responses_skipped_parse += 1
                continue
            stats.responses_kept += 1
            records.append(record)
    return records


def section_title(prompt: str, occurrence: int, total: int) -> str:
    title = re.sub(r"(?<!^)([A-Z])", r"_\1", prompt).upper()
    if total > 1:
        return f"{title}_{occurrence}"
    return title


def build_debug_doc(
    key: Tuple[str, str, int],
    records: List[ResponseRecord],
    args: argparse.Namespace,
    stats: ExportStats,
) -> dict:
    run, test, step = key
    records = sorted(records, key=lambda r: (DEBUG_SECTION_ORDER.index(r.prompt) if r.prompt in DEBUG_SECTION_ORDER else 999, r.request_id))
    by_prompt: Dict[str, List[ResponseRecord]] = {}
    for record in records:
        by_prompt.setdefault(record.prompt, []).append(record)

    lines = [
        "<hen_debug_cpt>",
        f"run: {run}",
        f"test: {test}",
        f"step: {step}",
        (
            "source: logs/debug response JSON content with filtered dynamic request context"
            if args.include_dynamic_request_context
            else "source: logs/debug response JSON content only"
        ),
        "",
    ]

    prompts: List[str] = []
    request_ids: List[int] = []
    truncated = False
    for prompt in DEBUG_SECTION_ORDER:
        prompt_records = by_prompt.get(prompt, [])
        total = len(prompt_records)
        for index, record in enumerate(prompt_records, 1):
            title = section_title(prompt, index, total)
            content, was_truncated = truncate_middle(record.content, args.max_section_chars)
            truncated = truncated or was_truncated
            if record.request_context:
                lines.extend(
                    [
                        f"{title}_REQUEST_CONTEXT:",
                        record.request_context,
                        "",
                        f"{title}_ASSISTANT_RESPONSE:",
                        content,
                        "",
                    ]
                )
            else:
                lines.extend(
                    [
                        f"{title}:",
                        content,
                        "",
                    ]
                )
            prompts.append(prompt)
            request_ids.append(record.request_id)

    text = "\n".join(lines).rstrip() + "\n</hen_debug_cpt>\n"
    text, doc_truncated = truncate_middle(text, args.max_doc_chars)
    if truncated or doc_truncated:
        stats.docs_truncated += 1

    return {
        "text": text,
        "meta": {
            "kind": "hen_cpt_debug",
            "run": run,
            "test": test,
            "step": step,
            "prompts": prompts,
            "request_ids": request_ids,
            "dynamic_request_context": args.include_dynamic_request_context,
            "truncated": truncated or doc_truncated,
        },
    }


def build_node_doc(record: ResponseRecord, args: argparse.Namespace, stats: ExportStats) -> dict:
    content, section_truncated = truncate_middle(record.content, args.max_section_chars)
    lines = [
        "<hen_node_cpt>",
        f"run: {record.run}",
        f"node: {record.node}",
        f"prompt: {record.prompt}",
        f"request_id: {record.request_id}",
        (
            "source: logs/nodes response JSON content with filtered dynamic request context"
            if args.include_dynamic_request_context
            else "source: logs/nodes response JSON content only"
        ),
        "",
    ]
    if record.request_context:
        lines.extend(["REQUEST_CONTEXT:", record.request_context, "", "ASSISTANT_RESPONSE:", content])
    else:
        lines.extend(["RESPONSE:", content])
    lines.extend(["</hen_node_cpt>", ""])
    text = "\n".join(lines)
    text, doc_truncated = truncate_middle(text, args.max_doc_chars)
    if section_truncated or doc_truncated:
        stats.docs_truncated += 1
    return {
        "text": text,
        "meta": {
            "kind": "hen_cpt_nodes",
            "run": record.run,
            "node": record.node,
            "prompt": record.prompt,
            "request_id": record.request_id,
            "dynamic_request_context": args.include_dynamic_request_context,
            "truncated": section_truncated or doc_truncated,
        },
    }


def write_jsonl(path: Path, rows: Iterable[dict], overwrite: bool) -> int:
    if path.exists() and not overwrite:
        raise SystemExit(f"Output exists, use --overwrite: {path}")
    count = 0
    with path.open("w", encoding="utf-8") as handle:
        for row in rows:
            handle.write(json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n")
            count += 1
    return count


def export_cpt(args: argparse.Namespace) -> ExportStats:
    logs_roots = discover_log_roots(args.log_roots)
    output_dir = Path(args.output_dir).expanduser().resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    stats = ExportStats()
    debug_path = output_dir / args.debug_output
    nodes_path = output_dir / args.nodes_output
    stats.output_debug = str(debug_path)
    stats.output_nodes = str(nodes_path)

    debug_prompts = comma_set(args.debug_prompts)
    node_prompts = comma_set(args.node_prompts)
    stable_counts: Dict[str, int] = {}
    if args.include_dynamic_request_context:
        stable_counts = collect_request_fingerprint_counts(
            logs_roots,
            debug_prompts,
            node_prompts,
            args.include_nodes,
        )

    debug_records = collect_debug_records(logs_roots, debug_prompts, args, stats, stable_counts)
    debug_rows = [
        build_debug_doc(key, records, args, stats)
        for key, records in sorted(debug_records.items())
        if records
    ]
    stats.debug_docs = write_jsonl(debug_path, debug_rows, args.overwrite)

    if args.include_nodes:
        node_records = collect_node_records(logs_roots, node_prompts, args, stats, stable_counts)
        node_rows = [
            build_node_doc(record, args, stats)
            for record in sorted(node_records, key=lambda r: (r.run, r.node, r.request_id))
        ]
        stats.node_docs = write_jsonl(nodes_path, node_rows, args.overwrite)
    else:
        stats.output_nodes = ""

    return stats


def main() -> int:
    args = parse_args()
    stats = export_cpt(args)
    print(json.dumps(stats.__dict__, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
