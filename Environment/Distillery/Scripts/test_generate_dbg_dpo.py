#!/usr/bin/env python3

import importlib.util
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


SCRIPT_PATH = Path(__file__).with_name("generate_dbg_dpo.py")
MODULE_NAME = "generate_dbg_dpo_under_test"


def load_module():
    spec = importlib.util.spec_from_file_location(MODULE_NAME, SCRIPT_PATH)
    module = importlib.util.module_from_spec(spec)
    sys.modules[MODULE_NAME] = module
    spec.loader.exec_module(module)
    return module


def write_json(path: Path, payload):
    path.write_text(json.dumps(payload), encoding="utf-8")


class GenerateDbgDpoTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mod = load_module()

    def make_leaf_with_tracks(self, root: Path) -> Path:
        leaf = root / "leaf"
        leaf.mkdir()

        write_json(
            leaf / "optimized_fix_10_20.json",
            {
                "steps": [
                    {
                        "step": 11,
                        "original_step": 31,
                        "action_type": "search_source",
                        "action_subject": "bar",
                        "invocation": 1,
                        "line_number": 0,
                    },
                    {
                        "step": 20,
                        "original_step": 40,
                        "action_type": "fix_function",
                        "action_subject": "bar",
                        "invocation": 1,
                        "line_number": 0,
                    },
                ]
            },
        )

        write_json(
            leaf / "original_fix_10_20.json",
            {
                "steps": [
                    {
                        "original_step": 31,
                        "action_type": "function_info",
                        "action_subject": "foo",
                        "invocation": 2,
                        "line_number": 7,
                    },
                    {
                        "original_step": 40,
                        "action_type": "fix_function",
                        "action_subject": "baz",
                        "invocation": 1,
                        "line_number": 0,
                    },
                ]
            },
        )

        return leaf

    def test_build_track_index_loads_original_track_metadata(self):
        with tempfile.TemporaryDirectory() as td:
            leaf = self.make_leaf_with_tracks(Path(td))
            step_meta_index = self.mod.build_track_index(leaf)

            info_meta = step_meta_index["step_10_11"]
            fix_meta = step_meta_index["step_10_12"]

            self.assertTrue(info_meta.original_track_present)
            self.assertEqual(
                info_meta.original_info_signatures,
                (
                    self.mod.StepSignature(
                        action_type="function_info",
                        action_subject="foo",
                        invocation=2,
                        line_number=7,
                    ),
                ),
            )
            self.assertEqual(info_meta.original_fix_subjects, ("baz",))
            self.assertEqual(
                fix_meta.mapped_original_signature,
                self.mod.StepSignature(
                    action_type="fix_function",
                    action_subject="baz",
                    invocation=1,
                    line_number=0,
                ),
            )

    def test_score_candidate_drops_exact_original_info_alternative(self):
        with tempfile.TemporaryDirectory() as td:
            leaf = self.make_leaf_with_tracks(Path(td))
            step_meta_index = self.mod.build_track_index(leaf)
            track_meta = step_meta_index["step_10_11"]

            candidate, outcome = self.mod.score_candidate(
                candidate_text=json.dumps(
                    {
                        "action_type": "function_info",
                        "action_subject": "foo",
                        "invocation": 2,
                        "line_number": 7,
                        "motivation": "Use the original info step.",
                        "breakpoints": [],
                    }
                ),
                chosen_action=self.mod.ActionKey("search_source", "bar"),
                prompt_history=[],
                track_meta=track_meta,
                keep_invalid_rejects=False,
            )

            self.assertIsNone(candidate)
            self.assertEqual(outcome, "valid_original_info_alternative")

    def test_score_candidate_tags_original_fix_without_shielding(self):
        with tempfile.TemporaryDirectory() as td:
            leaf = self.make_leaf_with_tracks(Path(td))
            step_meta_index = self.mod.build_track_index(leaf)
            track_meta = step_meta_index["step_10_12"]

            candidate, outcome = self.mod.score_candidate(
                candidate_text=json.dumps(
                    {
                        "action_type": "fix_function",
                        "action_subject": "baz",
                        "invocation": 1,
                        "line_number": 0,
                        "motivation": "Retry the original fix target.",
                        "breakpoints": [],
                    }
                ),
                chosen_action=self.mod.ActionKey("search_source", "bar"),
                prompt_history=[self.mod.ActionKey("fix_function", "baz")],
                track_meta=track_meta,
                keep_invalid_rejects=True,
            )

            self.assertIsNotNone(candidate)
            self.assertIn("duplicate_in_prompt_history", candidate.reasons)
            self.assertTrue(candidate.matches_original_step_exact)
            self.assertTrue(candidate.matches_original_fix_subject)
            self.assertEqual(outcome, ",".join(candidate.reasons))

    def test_process_step_file_persists_original_track_metadata(self):
        with tempfile.TemporaryDirectory() as td:
            leaf = self.make_leaf_with_tracks(Path(td))
            step_path = leaf / "step_10_11.json"
            write_json(
                step_path,
                {
                    "messages": [
                        {"role": "system", "content": "Debugger"},
                        {"role": "user", "content": "Pick the next step."},
                        {
                            "role": "assistant",
                            "content": json.dumps(
                                {
                                    "action_type": "search_source",
                                    "action_subject": "bar",
                                    "invocation": 1,
                                    "line_number": 0,
                                    "motivation": "Chosen optimized step.",
                                    "breakpoints": [],
                                }
                            ),
                        },
                    ]
                },
            )

            step_meta_index = self.mod.build_track_index(leaf)
            reject = self.mod.Candidate(
                raw_content=json.dumps(
                    {
                        "action_type": "fix_function",
                        "action_subject": "baz",
                        "invocation": 1,
                        "line_number": 0,
                        "motivation": "Rejected original fix target.",
                        "breakpoints": [],
                    }
                ),
                action=self.mod.ActionKey("fix_function", "baz"),
                parsed={"action_type": "fix_function", "action_subject": "baz"},
                reasons=["duplicate_in_prompt_history"],
                score=3,
                signature=self.mod.StepSignature(
                    action_type="fix_function",
                    action_subject="baz",
                    invocation=1,
                    line_number=0,
                ),
                matches_original_step_exact=True,
                matches_original_fix_subject=True,
            )
            trace = self.mod.SelectionTrace(sample_outcomes=["synthetic"])
            args = SimpleNamespace(model="test-model", overwrite=True, verbose=False)
            writer = io.StringIO()

            with mock.patch.object(self.mod, "choose_best_reject", return_value=(reject, trace)):
                kept = self.mod.process_step_file(
                    step_path=step_path,
                    leaf_dir=leaf,
                    step_meta_index=step_meta_index,
                    train_writer=writer,
                    args=args,
                )

            self.assertTrue(kept)
            record = json.loads(writer.getvalue())
            self.assertTrue(record["meta"]["original_track_present"])
            self.assertTrue(record["meta"]["matches_original_step_exact"])
            self.assertTrue(record["meta"]["matches_original_fix_subject"])
            self.assertEqual(record["meta"]["sample"], "step_10_11")


if __name__ == "__main__":
    unittest.main()
