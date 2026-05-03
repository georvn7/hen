#!/usr/bin/env python3

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace


SCRIPT_PATH = Path(__file__).with_name("extract_dbg_rare_tracks.py")
MODULE_NAME = "extract_dbg_rare_tracks_under_test"


def load_module():
    spec = importlib.util.spec_from_file_location(MODULE_NAME, SCRIPT_PATH)
    module = importlib.util.module_from_spec(spec)
    sys.modules[MODULE_NAME] = module
    spec.loader.exec_module(module)
    return module


def write_json(path: Path, payload):
    path.write_text(json.dumps(payload), encoding="utf-8")


def action_sample(action_type: str, subject: str = "x", prompt: str = "prompt"):
    return {
        "messages": [
            {"role": "system", "content": "debugger"},
            {"role": "user", "content": prompt},
            {
                "role": "assistant",
                "content": json.dumps(
                    {
                        "action_type": action_type,
                        "action_subject": subject,
                        "breakpoints": [],
                        "invocation": 1,
                        "line_number": 0,
                        "motivation": "m",
                    }
                ),
            },
        ]
    }


def analysis_sample(prompt: str = "run analysis"):
    return {
        "messages": [
            {"role": "system", "content": "debugger"},
            {"role": "user", "content": prompt},
            {
                "role": "assistant",
                "content": json.dumps(
                    {
                        "debug_notes": "notes",
                        "log_summary": "summary",
                    }
                ),
            },
        ]
    }


class ExtractDbgRareTracksTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mod = load_module()

    def args(self, **overrides):
        base = {
            "output_name": "train_dbg_rare_tracks_sft.jsonl",
            "manifest_name": "rare_tracks_manifest.json",
            "summary_name": "rare_tracks.txt",
            "track_text_prefix": "rare_track_",
            "sample_text_prefix": "rare_sample_",
            "write_track_jsonl": True,
            "max_prompt_chars": 131072,
            "include_system": False,
            "track_source": "original",
            "allow_partial_tracks": False,
            "overwrite": True,
            "verbose": False,
        }
        base.update(overrides)
        return SimpleNamespace(**base)

    def read_jsonl_actions(self, path: Path):
        actions = []
        for line in path.read_text(encoding="utf-8").splitlines():
            payload = json.loads(line)
            actions.append(self.mod.final_assistant_action(payload))
        return actions

    def test_selects_complete_emitted_track_with_rare_action(self):
        with tempfile.TemporaryDirectory() as td:
            leaf = Path(td)
            write_json(
                leaf / "original_fix_10_20.json",
                {
                    "steps": [
                        {"action_type": "run_test", "action_subject": "none", "original_step": 10},
                        {"action_type": "function_info", "action_subject": "parse", "original_step": 11},
                        {"action_type": "file_info", "action_subject": "s0.c", "original_step": 12},
                        {"action_type": "fix_function", "action_subject": "parse", "original_step": 20},
                    ]
                },
            )
            write_json(
                leaf / "optimized_fix_10_20.json",
                {
                    "steps": [
                        {"action_type": "run_test", "action_subject": "none", "original_step": 10},
                        {"action_type": "function_info", "action_subject": "parse", "original_step": 11},
                        {"action_type": "file_info", "action_subject": "s0.c", "original_step": 12},
                        {"action_type": "fix_function", "action_subject": "parse", "original_step": 20},
                    ]
                },
            )
            write_json(leaf / "system_10_20.json", analysis_sample())
            write_json(leaf / "step_10_11.json", action_sample("function_info", "parse"))
            write_json(leaf / "step_10_12.json", action_sample("file_info", "s0.c"))
            write_json(leaf / "step_10_13.json", action_sample("fix_function", "parse"))

            stats = self.mod.process_leaf_dir(
                leaf,
                self.args(),
                {"file_info"},
            )

            self.assertEqual(stats.tracks_seen, 1)
            self.assertEqual(stats.tracks_selected, 1)
            self.assertEqual(stats.rows_written, 3)
            self.assertEqual(
                self.read_jsonl_actions(leaf / "train_dbg_rare_tracks_sft.jsonl"),
                ["function_info", "file_info", "fix_function"],
            )

            manifest = json.loads((leaf / "rare_tracks_manifest.json").read_text())
            self.assertEqual(manifest["selected_tracks"][0]["rare_actions"], ["file_info"])
            self.assertEqual(manifest["selected_tracks"][0]["text_file"], "rare_track_10_20.txt")
            self.assertEqual(manifest["selected_tracks"][0]["jsonl_file"], "rare_track_10_20.jsonl")
            self.assertEqual(
                [item["text_file"] for item in manifest["selected_tracks"][0]["sample_files"]],
                [
                    "rare_sample_step_10_11.txt",
                    "rare_sample_step_10_12.txt",
                    "rare_sample_step_10_13.txt",
                ],
            )

            summary = (leaf / "rare_tracks.txt").read_text(encoding="utf-8")
            self.assertIn("Tracks selected: 1", summary)
            self.assertIn("rare_track_10_20.txt", summary)

            track_txt = (leaf / "rare_track_10_20.txt").read_text(encoding="utf-8")
            self.assertIn("RARE DEBUG FIX TRACK: 10 -> 20", track_txt)
            self.assertIn("file_info s0.c", track_txt)
            self.assertIn("*RARE*", track_txt)
            self.assertIn("SAMPLES:", track_txt)
            self.assertIn("rare_sample_step_10_12.txt", track_txt)
            self.assertNotIn(">> assistant", track_txt)

            sample_txt = (leaf / "rare_sample_step_10_12.txt").read_text(encoding="utf-8")
            self.assertIn(">> assistant", sample_txt)
            self.assertIn('"action_type": "file_info"', sample_txt)

            self.assertEqual(
                self.read_jsonl_actions(leaf / "rare_track_10_20.jsonl"),
                ["function_info", "file_info", "fix_function"],
            )

    def test_original_selector_skips_when_optimized_drops_all_rare_actions(self):
        with tempfile.TemporaryDirectory() as td:
            leaf = Path(td)
            write_json(
                leaf / "original_fix_1_9.json",
                {
                    "steps": [
                        {"action_type": "run_test", "action_subject": "none", "original_step": 1},
                        {"action_type": "file_info", "action_subject": "s0.c", "original_step": 2},
                        {"action_type": "fix_function", "action_subject": "parse", "original_step": 9},
                    ]
                },
            )
            write_json(
                leaf / "optimized_fix_1_9.json",
                {
                    "steps": [
                        {"action_type": "run_test", "action_subject": "none", "original_step": 1},
                        {"action_type": "function_info", "action_subject": "parse", "original_step": 3},
                        {"action_type": "fix_function", "action_subject": "parse", "original_step": 9},
                    ]
                },
            )
            write_json(leaf / "step_1_2.json", action_sample("function_info", "parse"))
            write_json(leaf / "step_1_3.json", action_sample("fix_function", "parse"))

            stats = self.mod.process_leaf_dir(
                leaf,
                self.args(),
                {"file_info"},
            )

            self.assertEqual(stats.tracks_seen, 1)
            self.assertEqual(stats.tracks_selected, 0)
            self.assertEqual(stats.tracks_skipped_rare_optimized_away, 1)
            self.assertFalse((leaf / "train_dbg_rare_tracks_sft.jsonl").exists())
            self.assertTrue((leaf / "rare_tracks.txt").exists())

    def test_selects_original_sequence_without_optimized_track(self):
        with tempfile.TemporaryDirectory() as td:
            leaf = Path(td)
            write_json(
                leaf / "original_fix_1_4.json",
                {
                    "steps": [
                        {"action_type": "run_test", "action_subject": "none", "original_step": 1},
                        {"action_type": "search_source", "action_subject": "parse", "original_step": 2},
                        {"action_type": "fix_function", "action_subject": "parse", "original_step": 4},
                    ]
                },
            )
            write_json(leaf / "step_1_2.json", action_sample("search_source", "parse"))
            write_json(leaf / "step_1_3.json", action_sample("fix_function", "parse"))

            stats = self.mod.process_leaf_dir(
                leaf,
                self.args(include_system=False),
                {"search_source"},
            )

            self.assertEqual(stats.tracks_selected, 1)
            self.assertEqual(
                self.read_jsonl_actions(leaf / "train_dbg_rare_tracks_sft.jsonl"),
                ["search_source", "fix_function"],
            )

    def test_strict_context_cap_skips_whole_track(self):
        with tempfile.TemporaryDirectory() as td:
            leaf = Path(td)
            write_json(
                leaf / "original_fix_1_3.json",
                {
                    "steps": [
                        {"action_type": "run_test", "action_subject": "none", "original_step": 1},
                        {"action_type": "file_info", "action_subject": "big.txt", "original_step": 2},
                        {"action_type": "fix_function", "action_subject": "parse", "original_step": 3},
                    ]
                },
            )
            write_json(leaf / "step_1_2.json", action_sample("file_info", "big.txt", prompt="x" * 20))
            write_json(leaf / "step_1_3.json", action_sample("fix_function", "parse"))

            stats = self.mod.process_leaf_dir(
                leaf,
                self.args(max_prompt_chars=10),
                {"file_info"},
            )

            self.assertEqual(stats.tracks_selected, 0)
            self.assertEqual(stats.tracks_skipped_oversize, 1)
            self.assertFalse((leaf / "train_dbg_rare_tracks_sft.jsonl").exists())

    def test_strict_mode_skips_track_with_invalid_step_action(self):
        with tempfile.TemporaryDirectory() as td:
            leaf = Path(td)
            write_json(
                leaf / "original_fix_1_3.json",
                {
                    "steps": [
                        {"action_type": "run_test", "action_subject": "none", "original_step": 1},
                        {"action_type": "search_source", "action_subject": "error text", "original_step": 2},
                        {"action_type": "fix_function", "action_subject": "parse", "original_step": 3},
                    ]
                },
            )
            write_json(leaf / "step_1_2.json", action_sample("", ""))
            write_json(leaf / "step_1_3.json", action_sample("fix_function", "parse"))

            stats = self.mod.process_leaf_dir(
                leaf,
                self.args(),
                {"search_source"},
            )

            self.assertEqual(stats.tracks_selected, 0)
            self.assertEqual(stats.tracks_skipped_invalid_rows, 1)
            self.assertEqual(stats.rows_invalid_action, 1)
            self.assertFalse((leaf / "train_dbg_rare_tracks_sft.jsonl").exists())

            manifest = json.loads((leaf / "rare_tracks_manifest.json").read_text())
            self.assertEqual(manifest["tracks_skipped_invalid_rows"], 1)
            self.assertEqual(manifest["rows_invalid_action"], 1)

    def test_partial_mode_keeps_fitting_rows(self):
        with tempfile.TemporaryDirectory() as td:
            leaf = Path(td)
            write_json(
                leaf / "original_fix_1_3.json",
                {
                    "steps": [
                        {"action_type": "run_test", "action_subject": "none", "original_step": 1},
                        {"action_type": "file_info", "action_subject": "big.txt", "original_step": 2},
                        {"action_type": "fix_function", "action_subject": "parse", "original_step": 3},
                    ]
                },
            )
            write_json(leaf / "step_1_2.json", action_sample("file_info", "big.txt", prompt="x" * 20))
            write_json(leaf / "step_1_3.json", action_sample("fix_function", "parse"))

            stats = self.mod.process_leaf_dir(
                leaf,
                self.args(max_prompt_chars=16, allow_partial_tracks=True),
                {"file_info"},
            )

            self.assertEqual(stats.tracks_selected, 1)
            self.assertEqual(stats.rows_written, 1)
            self.assertEqual(
                self.read_jsonl_actions(leaf / "train_dbg_rare_tracks_sft.jsonl"),
                ["fix_function"],
            )


if __name__ == "__main__":
    unittest.main()
