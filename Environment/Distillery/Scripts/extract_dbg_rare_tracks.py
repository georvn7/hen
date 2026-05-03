#!/usr/bin/env python3
"""
Extract full distilled debugger fix tracks that contain rare actions.

This is a no-inference post-processing tool. It scans existing distilled
dataset leaves, selects original debugger fix tracks whose sequence contains at
least one rare action, and writes a separate JSONL file per leaf.

Default output per leaf:
- train_dbg_rare_tracks_sft.jsonl
- rare_tracks_manifest.json
- rare_tracks.txt
- rare_track_<start>_<fix>.txt overview
- rare_track_<start>_<fix>.jsonl
- rare_sample_<source_sample>.txt, one human-readable file per emitted row

By default this extractor emits only step_* action samples. system_* run-analysis
rows can be included with --include-system, but are excluded by default because
they do not correspond to action diversification targets.

Selection is based on original_fix_*.json by default because the goal is action
diversification from actions the debugger actually took. Rows are emitted from
the distilled sequence that has step_* samples, usually optimized_fix_*.json.
If an original rare action was optimized away, this post-process cannot
synthesize its missing chat row; the track is skipped unless another rare action
remains in the emitted samples.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Set, Tuple


STEP_RE = re.compile(r"^step_(\d+)_(\d+)\.json$")
OPT_RE = re.compile(r"^optimized_fix_(\d+)_(\d+)\.json$")
ORIG_RE = re.compile(r"^original_fix_(\d+)_(\d+)\.json$")
SYSTEM_RE = re.compile(r"^system_(\d+)_(\d+)\.json$")

DEFAULT_RARE_ACTIONS = (
    "file_info",
    "search_source",
    "functions_summary",
    "call_graph",
    "data_info",
    "log_info",
    "debug_function",
)


@dataclass
class TrackRef:
    start_step: int
    fix_step: int
    sequence_path: Path
    sequence_kind: str
    steps: List[dict]
    selection_path: Optional[Path] = None
    selection_kind: str = ""
    selection_steps: List[dict] = field(default_factory=list)
    rare_actions: Set[str] = field(default_factory=set)
    emitted_rare_actions: Set[str] = field(default_factory=set)

    @property
    def key(self) -> str:
        return f"{self.start_step}_{self.fix_step}"


@dataclass
class SampleRef:
    path: Path
    sample_kind: str
    start_step: int
    fix_step: int
    current_step: int


@dataclass
class LeafStats:
    leaf: Path
    tracks_seen: int = 0
    tracks_selected: int = 0
    tracks_skipped_no_rare: int = 0
    tracks_skipped_oversize: int = 0
    tracks_skipped_missing_rows: int = 0
    tracks_skipped_invalid_rows: int = 0
    tracks_skipped_rare_optimized_away: int = 0
    rows_written: int = 0
    rows_skipped_oversize: int = 0
    rows_missing: int = 0
    rows_invalid_action: int = 0
    action_counts: Dict[str, int] = field(default_factory=dict)
    selected_tracks: List[dict] = field(default_factory=list)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "dataset_root",
        help="Dataset leaf directory or root containing many distilled leaves.",
    )
    parser.add_argument(
        "--output-name",
        default="train_dbg_rare_tracks_sft.jsonl",
        help="Per-leaf JSONL output filename.",
    )
    parser.add_argument(
        "--manifest-name",
        default="rare_tracks_manifest.json",
        help="Per-leaf manifest filename.",
    )
    parser.add_argument(
        "--summary-name",
        default="rare_tracks.txt",
        help="Per-leaf human-readable summary filename.",
    )
    parser.add_argument(
        "--track-text-prefix",
        default="rare_track_",
        help="Prefix for per-selected-track inspection text files.",
    )
    parser.add_argument(
        "--sample-text-prefix",
        default="rare_sample_",
        help="Prefix for per-emitted-sample inspection text files.",
    )
    parser.add_argument(
        "--write-track-jsonl",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Write one rare_track_<start>_<fix>.jsonl file per selected track.",
    )
    parser.add_argument(
        "--rare-actions",
        default=",".join(DEFAULT_RARE_ACTIONS),
        help="Comma-separated action types that mark a track as selected.",
    )
    parser.add_argument(
        "--track-source",
        choices=("original", "optimized"),
        default="original",
        help=(
            "Which distilled sequence files select tracks and sample rows. "
            "Default is original because rare actions optimized away are still "
            "valid action-diversification examples."
        ),
    )
    parser.add_argument(
        "--max-prompt-chars",
        type=int,
        default=131072,
        help=(
            "Approximate 32K-context guard. Sum of non-final-assistant message "
            "content must be <= this value. Set 0 to disable."
        ),
    )
    parser.add_argument(
        "--include-system",
        action=argparse.BooleanOptionalAction,
        default=False,
        help="Include system_<run>_<fix>.json run-analysis rows when present. Disabled by default.",
    )
    parser.add_argument(
        "--allow-partial-tracks",
        action="store_true",
        help="Write fitting rows even if another row from the same selected track is missing or oversized.",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Overwrite existing rare-track outputs.",
    )
    parser.add_argument(
        "--combined-output",
        default="",
        help="Optional path to also write a combined JSONL across all processed leaves.",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Print selected and skipped tracks.",
    )
    return parser.parse_args()


def load_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def dump_jsonl_line(payload: dict) -> str:
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":"))


def parse_action_from_text(text: str) -> Optional[dict]:
    text = text.strip()
    if not text:
        return None
    try:
        parsed = json.loads(text)
        return parsed if isinstance(parsed, dict) else None
    except json.JSONDecodeError:
        pass

    fence = re.search(r"```(?:json)?\s*(\{.*?\})\s*```", text, re.S)
    if fence:
        try:
            parsed = json.loads(fence.group(1))
            return parsed if isinstance(parsed, dict) else None
        except json.JSONDecodeError:
            pass

    start = text.find("{")
    end = text.rfind("}")
    if start >= 0 and end > start:
        try:
            parsed = json.loads(text[start : end + 1])
            return parsed if isinstance(parsed, dict) else None
        except json.JSONDecodeError:
            return None
    return None


def final_assistant_action(sample: dict) -> str:
    messages = sample.get("messages")
    if not isinstance(messages, list):
        return ""

    for message in reversed(messages):
        if not isinstance(message, dict) or message.get("role") != "assistant":
            continue
        parsed = parse_action_from_text(str(message.get("content", "")))
        if isinstance(parsed, dict):
            return str(parsed.get("action_type", "")).strip()
        return ""
    return ""


def prompt_chars(sample: dict) -> int:
    messages = sample.get("messages")
    if not isinstance(messages, list):
        return 0

    last_assistant = -1
    for idx, message in enumerate(messages):
        if isinstance(message, dict) and message.get("role") == "assistant":
            last_assistant = idx

    total = 0
    for idx, message in enumerate(messages):
        if idx == last_assistant:
            continue
        if isinstance(message, dict):
            total += len(str(message.get("content", "")))
    return total


def render_chat_sample(sample: dict) -> str:
    messages = sample.get("messages")
    if not isinstance(messages, list):
        return json.dumps(sample, ensure_ascii=False, indent=2)

    parts: List[str] = []
    for message in messages:
        if not isinstance(message, dict):
            continue
        role = str(message.get("role", "unknown"))
        content = str(message.get("content", ""))
        parts.append(f">> {role}\n\n{content}\n")
    return "\n".join(parts).rstrip() + "\n"


def render_or_copy_sample_text(ref: SampleRef, sample: dict) -> str:
    source_text_path = ref.path.with_suffix(".txt")
    if source_text_path.exists():
        return source_text_path.read_text(encoding="utf-8")
    return render_chat_sample(sample)


def render_sequence_steps(track: TrackRef) -> str:
    lines: List[str] = []
    highlighted_rare_actions = track.rare_actions | track.emitted_rare_actions
    for index, step in enumerate(track.steps, start=1):
        action = str(step.get("action_type", "")).strip()
        subject = str(step.get("action_subject", "")).strip()
        original = step.get("original_step", "")
        invocation = step.get("invocation", "")
        line_number = step.get("line_number", "")
        rare_marker = " *RARE*" if action in highlighted_rare_actions else ""
        lines.append(
            f"{index:02d}. {action} {subject} "
            f"(original_step={original}, invocation={invocation}, line={line_number}){rare_marker}"
        )
    return "\n".join(lines)


def render_track_text(
    leaf_dir: Path,
    track: TrackRef,
    sample_files: Sequence[dict],
    errors: Sequence[str],
) -> str:
    lines: List[str] = []
    lines.append(f"RARE DEBUG FIX TRACK: {track.start_step} -> {track.fix_step}")
    lines.append(f"Leaf: {leaf_dir}")
    lines.append(f"Sequence: {track.sequence_path.name} ({track.sequence_kind})")
    lines.append(f"Rare actions: {', '.join(sorted(track.rare_actions))}")
    lines.append("")
    lines.append("EMITTED SEQUENCE:")
    lines.append(render_sequence_steps(track))
    if errors:
        lines.append("")
        lines.append("WARNINGS:")
        lines.extend(f"- {err}" for err in errors)
    lines.append("")

    lines.append("SAMPLES:")
    if sample_files:
        for sample_file in sample_files:
            lines.append(
                f"- {sample_file['source_file']} | kind={sample_file['kind']} | "
                f"target={sample_file['target']} | prompt_chars={sample_file['prompt_chars']} | "
                f"txt={sample_file['text_file']}"
            )
    else:
        lines.append("- <none>")

    return "\n".join(lines).rstrip() + "\n"


def render_leaf_summary(stats: LeafStats, rare_actions: Set[str], output_name: str) -> str:
    lines: List[str] = []
    lines.append(f"RARE DEBUG TRACKS SUMMARY")
    lines.append(f"Leaf: {stats.leaf}")
    lines.append(f"Output JSONL: {output_name if stats.rows_written else '<none>'}")
    lines.append(f"Rare action selector: {', '.join(sorted(rare_actions))}")
    lines.append("")
    lines.append(f"Tracks seen: {stats.tracks_seen}")
    lines.append(f"Tracks selected: {stats.tracks_selected}")
    lines.append(f"Tracks skipped without rare actions: {stats.tracks_skipped_no_rare}")
    lines.append(f"Tracks skipped for oversize rows: {stats.tracks_skipped_oversize}")
    lines.append(f"Tracks skipped for missing rows: {stats.tracks_skipped_missing_rows}")
    lines.append(f"Tracks skipped for invalid action rows: {stats.tracks_skipped_invalid_rows}")
    lines.append(f"Tracks skipped because rare actions were optimized away: {stats.tracks_skipped_rare_optimized_away}")
    lines.append(f"Rows written: {stats.rows_written}")
    lines.append(f"Rows skipped with invalid action target: {stats.rows_invalid_action}")
    lines.append("")
    lines.append("Action counts:")
    if stats.action_counts:
        for action, count in sorted(stats.action_counts.items(), key=lambda item: (-item[1], item[0])):
            lines.append(f"- {action or 'debug_analysis'}: {count}")
    else:
        lines.append("- <none>")
    lines.append("")
    lines.append("Selected tracks:")
    if stats.selected_tracks:
        for track in stats.selected_tracks:
            lines.append(
                f"- {track['start_step']} -> {track['fix_step']} "
                f"rare={','.join(track['rare_actions'])} rows={track['rows_written']} "
                f"txt={track.get('text_file', '')} samples={len(track.get('sample_files', []))}"
            )
    else:
        lines.append("- <none>")
    return "\n".join(lines) + "\n"


def discover_leaf_dirs(root: Path) -> List[Path]:
    if not root.is_dir():
        return []

    if any(STEP_RE.match(path.name) for path in root.iterdir() if path.is_file()):
        return [root]

    leaves: List[Path] = []
    for current, _, files in os.walk(root):
        if any(STEP_RE.match(name) for name in files):
            leaves.append(Path(current))
    leaves.sort()
    return leaves


def load_sequence(path: Path, start_step: int, fix_step: int, sequence_kind: str) -> Optional[TrackRef]:
    payload = load_json(path)
    raw_steps = payload.get("steps")
    if not isinstance(raw_steps, list) or not raw_steps:
        return None

    steps = [step for step in raw_steps if isinstance(step, dict)]
    if not steps:
        return None

    return TrackRef(
        start_step=start_step,
        fix_step=fix_step,
        sequence_path=path,
        sequence_kind=sequence_kind,
        steps=steps,
    )


def iter_optimized_track_refs(leaf_dir: Path) -> Iterable[TrackRef]:
    for path in sorted(leaf_dir.iterdir()):
        match = OPT_RE.match(path.name)
        if not match:
            continue
        start_step = int(match.group(1))
        fix_step = int(match.group(2))
        track = load_sequence(path, start_step, fix_step, "optimized")
        if track is not None:
            track.selection_path = path
            track.selection_kind = "optimized"
            track.selection_steps = track.steps[:]
            yield track


def iter_original_selected_track_refs(leaf_dir: Path) -> Iterable[TrackRef]:
    for path in sorted(leaf_dir.iterdir()):
        match = ORIG_RE.match(path.name)
        if not match:
            continue

        start_step = int(match.group(1))
        fix_step = int(match.group(2))
        original = load_sequence(path, start_step, fix_step, "original")
        if original is None:
            continue

        optimized_path = leaf_dir / f"optimized_fix_{start_step}_{fix_step}.json"
        emitted = None
        if optimized_path.exists():
            emitted = load_sequence(optimized_path, start_step, fix_step, "optimized")
        if emitted is None:
            emitted = original

        emitted.selection_path = path
        emitted.selection_kind = "original"
        emitted.selection_steps = original.steps[:]
        yield emitted


def iter_track_refs(leaf_dir: Path, track_source: str) -> Iterable[TrackRef]:
    if track_source == "optimized":
        yield from iter_optimized_track_refs(leaf_dir)
    else:
        yield from iter_original_selected_track_refs(leaf_dir)


def emitted_steps(track: TrackRef) -> List[dict]:
    steps = track.steps[:]
    if steps and str(steps[0].get("action_type", "")).strip() == "run_test":
        steps = steps[1:]
    return steps


def track_rare_actions(track: TrackRef, rare_actions: Set[str]) -> Set[str]:
    found: Set[str] = set()
    steps = track.selection_steps if track.selection_steps else track.steps
    selector = TrackRef(
        start_step=track.start_step,
        fix_step=track.fix_step,
        sequence_path=track.selection_path or track.sequence_path,
        sequence_kind=track.selection_kind or track.sequence_kind,
        steps=steps,
    )
    for step in emitted_steps(selector):
        action = str(step.get("action_type", "")).strip()
        if action in rare_actions:
            found.add(action)
    return found


def emitted_rare_actions(track: TrackRef, rare_actions: Set[str]) -> Set[str]:
    found: Set[str] = set()
    for step in emitted_steps(track):
        action = str(step.get("action_type", "")).strip()
        if action in rare_actions:
            found.add(action)
    return found


def sample_refs_for_track(leaf_dir: Path, track: TrackRef, include_system: bool) -> List[SampleRef]:
    refs: List[SampleRef] = []

    if include_system:
        refs.append(
            SampleRef(
                path=leaf_dir / f"system_{track.start_step}_{track.fix_step}.json",
                sample_kind="system",
                start_step=track.start_step,
                fix_step=track.fix_step,
                current_step=track.start_step,
            )
        )

    for index, _ in enumerate(emitted_steps(track), start=1):
        current_step = track.start_step + index
        refs.append(
            SampleRef(
                path=leaf_dir / f"step_{track.start_step}_{current_step}.json",
                sample_kind="step",
                start_step=track.start_step,
                fix_step=track.fix_step,
                current_step=current_step,
            )
        )

    return refs


def load_fitting_samples(
    refs: Sequence[SampleRef],
    max_prompt_chars: int,
    allow_partial_tracks: bool,
) -> Tuple[List[Tuple[SampleRef, dict, int]], List[str]]:
    loaded: List[Tuple[SampleRef, dict, int]] = []
    errors: List[str] = []

    for ref in refs:
        if not ref.path.exists():
            if ref.sample_kind == "system":
                continue
            errors.append(f"missing:{ref.path.name}")
            continue

        try:
            sample = load_json(ref.path)
        except Exception as exc:  # pragma: no cover - defensive path
            errors.append(f"malformed:{ref.path.name}:{exc}")
            continue

        size = prompt_chars(sample)
        if max_prompt_chars > 0 and size > max_prompt_chars:
            errors.append(f"oversize:{ref.path.name}:{size}")
            if allow_partial_tracks:
                continue
            continue

        if ref.sample_kind == "step" and not final_assistant_action(sample):
            errors.append(f"invalid_action:{ref.path.name}")
            if allow_partial_tracks:
                continue
            continue

        loaded.append((ref, sample, size))

    if errors and not allow_partial_tracks:
        return [], errors
    return loaded, errors


def process_leaf_dir(leaf_dir: Path, args: argparse.Namespace, rare_actions: Set[str]) -> LeafStats:
    stats = LeafStats(leaf=leaf_dir)
    output_path = leaf_dir / args.output_name
    manifest_path = leaf_dir / args.manifest_name
    summary_path = leaf_dir / args.summary_name

    if output_path.exists() and not args.overwrite:
        raise RuntimeError(f"Refusing to overwrite {output_path}. Use --overwrite.")
    if manifest_path.exists() and not args.overwrite:
        raise RuntimeError(f"Refusing to overwrite {manifest_path}. Use --overwrite.")
    if summary_path.exists() and not args.overwrite:
        raise RuntimeError(f"Refusing to overwrite {summary_path}. Use --overwrite.")

    if args.overwrite:
        output_path.unlink(missing_ok=True)
        manifest_path.unlink(missing_ok=True)
        summary_path.unlink(missing_ok=True)
        for stale in leaf_dir.glob(f"{args.track_text_prefix}*.txt"):
            stale.unlink()
        for stale in leaf_dir.glob(f"{args.track_text_prefix}*.jsonl"):
            stale.unlink()
        for stale in leaf_dir.glob(f"{args.sample_text_prefix}*.txt"):
            stale.unlink()

    with output_path.open("w", encoding="utf-8", buffering=1) as writer:
        for track in iter_track_refs(leaf_dir, args.track_source):
            stats.tracks_seen += 1
            rare = track_rare_actions(track, rare_actions)
            if not rare:
                stats.tracks_skipped_no_rare += 1
                continue

            track.rare_actions = rare
            track.emitted_rare_actions = emitted_rare_actions(track, rare_actions)
            if not track.emitted_rare_actions:
                stats.tracks_skipped_rare_optimized_away += 1
                continue

            refs = sample_refs_for_track(leaf_dir, track, args.include_system)
            samples, errors = load_fitting_samples(
                refs,
                args.max_prompt_chars,
                args.allow_partial_tracks,
            )
            invalid_count = sum(1 for err in errors if err.startswith("invalid_action:"))
            stats.rows_invalid_action += invalid_count

            if errors and not args.allow_partial_tracks:
                if any(err.startswith("oversize:") for err in errors):
                    stats.tracks_skipped_oversize += 1
                if any(err.startswith("missing:") for err in errors):
                    stats.tracks_skipped_missing_rows += 1
                if invalid_count:
                    stats.tracks_skipped_invalid_rows += 1
                if args.verbose:
                    print(f"[skip] {leaf_dir} track {track.key}: {'; '.join(errors)}")
                continue

            if not samples:
                stats.tracks_skipped_missing_rows += 1
                continue

            stats.tracks_selected += 1
            track_rows = 0
            track_actions: Dict[str, int] = {}
            max_seen_prompt_chars = 0
            track_text_name = f"{args.track_text_prefix}{track.start_step}_{track.fix_step}.txt"
            track_text_path = leaf_dir / track_text_name
            track_jsonl_name = f"{args.track_text_prefix}{track.start_step}_{track.fix_step}.jsonl"
            track_jsonl_path = leaf_dir / track_jsonl_name
            sample_files: List[dict] = []

            track_jsonl_writer = None
            if args.write_track_jsonl:
                track_jsonl_writer = track_jsonl_path.open("w", encoding="utf-8", buffering=1)
            for ref, sample, size in samples:
                action = final_assistant_action(sample) or "debug_analysis"
                sample_text_name = f"{args.sample_text_prefix}{ref.path.stem}.txt"
                sample_text_path = leaf_dir / sample_text_name
                if sample_text_path.exists() and not args.overwrite:
                    raise RuntimeError(f"Refusing to overwrite {sample_text_path}. Use --overwrite.")
                sample_text_path.write_text(
                    render_or_copy_sample_text(ref, sample),
                    encoding="utf-8",
                )

                stats.action_counts[action] = stats.action_counts.get(action, 0) + 1
                track_actions[action] = track_actions.get(action, 0) + 1
                sample_line = dump_jsonl_line(sample) + "\n"
                writer.write(sample_line)
                if track_jsonl_writer is not None:
                    track_jsonl_writer.write(sample_line)
                stats.rows_written += 1
                track_rows += 1
                max_seen_prompt_chars = max(max_seen_prompt_chars, size)
                sample_files.append(
                    {
                        "source_file": ref.path.name,
                        "text_file": sample_text_name,
                        "kind": ref.sample_kind,
                        "target": action,
                        "prompt_chars": size,
                    }
                )
            if track_jsonl_writer is not None:
                track_jsonl_writer.close()

            track_text_path.write_text(
                render_track_text(leaf_dir, track, sample_files, errors),
                encoding="utf-8",
            )

            stats.rows_missing += sum(1 for err in errors if err.startswith("missing:"))
            stats.rows_skipped_oversize += sum(1 for err in errors if err.startswith("oversize:"))
            stats.selected_tracks.append(
                {
                    "start_step": track.start_step,
                    "fix_step": track.fix_step,
                    "sequence_kind": track.sequence_kind,
                    "sequence_file": track.sequence_path.name,
                    "rare_actions": sorted(rare),
                    "emitted_rare_actions": sorted(track.emitted_rare_actions),
                    "selection_kind": track.selection_kind or track.sequence_kind,
                    "selection_file": (track.selection_path or track.sequence_path).name,
                    "rows_written": track_rows,
                    "action_counts": dict(sorted(track_actions.items())),
                    "max_prompt_chars": max_seen_prompt_chars,
                    "text_file": track_text_name,
                    "jsonl_file": track_jsonl_name if args.write_track_jsonl else "",
                    "sample_files": sample_files,
                    "partial": bool(errors),
                    "warnings": errors,
                }
            )

            if args.verbose:
                print(
                    f"[select] {leaf_dir} track {track.key} "
                    f"rare={','.join(sorted(rare))} rows={track_rows}"
                )

    if stats.rows_written == 0:
        output_path.unlink(missing_ok=True)

    summary_path.write_text(
        render_leaf_summary(stats, rare_actions, output_path.name),
        encoding="utf-8",
    )

    manifest = {
        "leaf": str(leaf_dir),
        "output": output_path.name if stats.rows_written else "",
        "summary": summary_path.name,
        "rare_actions": sorted(rare_actions),
        "track_source": args.track_source,
        "max_prompt_chars": args.max_prompt_chars,
        "include_system": args.include_system,
        "allow_partial_tracks": args.allow_partial_tracks,
        "tracks_seen": stats.tracks_seen,
        "tracks_selected": stats.tracks_selected,
        "tracks_skipped_no_rare": stats.tracks_skipped_no_rare,
        "tracks_skipped_oversize": stats.tracks_skipped_oversize,
        "tracks_skipped_missing_rows": stats.tracks_skipped_missing_rows,
        "tracks_skipped_invalid_rows": stats.tracks_skipped_invalid_rows,
        "tracks_skipped_rare_optimized_away": stats.tracks_skipped_rare_optimized_away,
        "rows_written": stats.rows_written,
        "rows_missing": stats.rows_missing,
        "rows_skipped_oversize": stats.rows_skipped_oversize,
        "rows_invalid_action": stats.rows_invalid_action,
        "action_counts": dict(sorted(stats.action_counts.items())),
        "selected_tracks": stats.selected_tracks,
    }
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )

    return stats


def append_leaf_output(combined_handle, leaf_dir: Path, output_name: str) -> int:
    output_path = leaf_dir / output_name
    if not output_path.exists():
        return 0
    count = 0
    with output_path.open("r", encoding="utf-8") as handle:
        for line in handle:
            if line.strip():
                combined_handle.write(line)
                count += 1
    return count


def main() -> int:
    args = parse_args()
    root = Path(args.dataset_root).expanduser().resolve()
    if not root.exists():
        print(f"Dataset path does not exist: {root}", file=sys.stderr)
        return 2

    rare_actions = {item.strip() for item in args.rare_actions.split(",") if item.strip()}
    if not rare_actions:
        print("No rare actions configured.", file=sys.stderr)
        return 2

    leaf_dirs = discover_leaf_dirs(root)
    if not leaf_dirs:
        print(f"No distilled dataset leaves with step_*.json found under: {root}", file=sys.stderr)
        return 2

    combined_handle = None
    combined_rows = 0
    if args.combined_output:
        combined_path = Path(args.combined_output).expanduser().resolve()
        if combined_path.exists() and not args.overwrite:
            print(f"Refusing to overwrite {combined_path}. Use --overwrite.", file=sys.stderr)
            return 2
        combined_path.parent.mkdir(parents=True, exist_ok=True)
        combined_handle = combined_path.open("w", encoding="utf-8", buffering=1)

    total_tracks_seen = 0
    total_tracks_selected = 0
    total_rows = 0
    total_actions: Dict[str, int] = {}

    try:
        for leaf_dir in leaf_dirs:
            stats = process_leaf_dir(leaf_dir, args, rare_actions)
            total_tracks_seen += stats.tracks_seen
            total_tracks_selected += stats.tracks_selected
            total_rows += stats.rows_written
            for action, count in stats.action_counts.items():
                total_actions[action] = total_actions.get(action, 0) + count

            if combined_handle is not None:
                combined_rows += append_leaf_output(combined_handle, leaf_dir, args.output_name)

            print(
                f"[leaf] {leaf_dir} selected_tracks={stats.tracks_selected}/{stats.tracks_seen} "
                f"rows={stats.rows_written}"
            )
    finally:
        if combined_handle is not None:
            combined_handle.close()

    print(
        f"[done] selected_tracks={total_tracks_selected}/{total_tracks_seen} "
        f"rows={total_rows}"
    )
    if combined_handle is not None:
        print(f"[combined] rows={combined_rows} output={Path(args.combined_output).expanduser().resolve()}")
    if total_actions:
        print("[actions]")
        for action, count in sorted(total_actions.items(), key=lambda item: (-item[1], item[0])):
            print(f"  {action}: {count}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
