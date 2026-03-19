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
                allow_weak_rejects=False,
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
                allow_weak_rejects=False,
            )

            self.assertIsNotNone(candidate)
            self.assertIn("duplicate_in_prompt_history", candidate.reasons)
            self.assertTrue(candidate.matches_original_step_exact)
            self.assertTrue(candidate.matches_original_fix_subject)
            self.assertEqual(outcome, ",".join(candidate.reasons))

    def test_score_candidate_accepts_grounded_shortcut_as_strong_negative(self):
        with tempfile.TemporaryDirectory() as td:
            leaf = self.make_leaf_with_tracks(Path(td))
            step_meta_index = self.mod.build_track_index(leaf)
            track_meta = step_meta_index["step_10_11"]

            candidate, outcome = self.mod.score_candidate(
                candidate_text=json.dumps(
                    {
                        "action_type": "fix_function",
                        "action_subject": "bar",
                        "invocation": 1,
                        "line_number": 0,
                        "motivation": "Jump to the final fix too early.",
                        "breakpoints": [],
                    }
                ),
                chosen_action=self.mod.ActionKey("search_source", "bar"),
                prompt_history=[],
                track_meta=track_meta,
                keep_invalid_rejects=True,
                allow_weak_rejects=False,
            )

            self.assertIsNotNone(candidate)
            self.assertIn("grounded_shortcut_to_final_fix", candidate.reasons)
            self.assertEqual(outcome, ",".join(candidate.reasons))
            self.assertEqual(self.mod.classify_reject_kind(candidate), "hard_negative")

    def test_score_candidate_can_accept_weak_rejects(self):
        candidate, outcome = self.mod.score_candidate(
            candidate_text=json.dumps(
                {
                    "action_type": "file_info",
                    "action_subject": "codegen",
                    "invocation": 1,
                    "line_number": 0,
                    "motivation": "Broader but weaker probe.",
                    "breakpoints": [],
                }
            ),
            chosen_action=self.mod.ActionKey("function_info", "gen_binary_op"),
            prompt_history=[],
            track_meta=None,
            keep_invalid_rejects=True,
            allow_weak_rejects=True,
        )

        self.assertIsNotNone(candidate)
        self.assertEqual(outcome, "accepted_weak_reject")
        self.assertEqual(candidate.action.normalized_type, "file_info")
        self.assertEqual(self.mod.classify_reject_kind(candidate), "efficiency_negative")

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
                reject_kind="hard_negative",
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
            trace_payload = json.loads((leaf / "step_10_11_trace.json").read_text(encoding="utf-8"))
            self.assertTrue(trace_payload["kept"])
            self.assertEqual(trace_payload["preferred_action"]["action_type"], "search_source")
            self.assertEqual(trace_payload["selected_reject"]["action"]["action_type"], "fix_function")
            self.assertEqual(trace_payload["selected_reject"]["reject_kind"], "hard_negative")
            self.assertEqual(record["meta"]["reject_kind"], "hard_negative")

    def test_choose_best_reject_skips_request_failures(self):
        args = SimpleNamespace(
            api_base="http://127.0.0.1:8000/v1",
            endpoint_path="/chat/completions",
            api_key="",
            model="test-model",
            temperature=0.4,
            max_completion_tokens=256,
            timeout_sec=30,
            samples_per_step=2,
            request_retries=1,
            drop_invalid_rejects=False,
            allow_weak_rejects=False,
        )

        prompt_messages = [
            {"role": "system", "content": "Debugger"},
            {"role": "user", "content": "Pick the next step."},
        ]

        with mock.patch.object(
            self.mod,
            "post_chat_completions",
            side_effect=RuntimeError("Timed out waiting for the model"),
        ):
            reject, trace = self.mod.choose_best_reject(
                prompt_messages=prompt_messages,
                chosen_action=self.mod.ActionKey("function_info", "foo"),
                prompt_history=[],
                track_meta=None,
                args=args,
            )

        self.assertIsNone(reject)
        self.assertEqual(
            trace.sample_outcomes,
            ["request_failed:RuntimeError", "request_failed:RuntimeError"],
        )
        self.assertEqual(len(trace.sample_details), 2)
        self.assertEqual(trace.sample_details[0]["outcome"], "request_failed:RuntimeError")
        self.assertIn("Timed out waiting for the model", trace.sample_details[0]["request_error"])

    def test_choose_best_reject_prefers_structured_non_run_test_over_run_test(self):
        args = SimpleNamespace(
            api_base="http://127.0.0.1:8000/v1",
            endpoint_path="/chat/completions",
            api_key="",
            model="test-model",
            temperature=0.4,
            max_completion_tokens=256,
            timeout_sec=30,
            samples_per_step=2,
            request_retries=1,
            drop_invalid_rejects=False,
            allow_weak_rejects=True,
        )

        prompt_messages = [
            {"role": "system", "content": "Debugger"},
            {"role": "user", "content": "Pick the next step."},
        ]

        with mock.patch.object(
            self.mod,
            "post_chat_completions",
            side_effect=[
                json.dumps(
                    {
                        "action_type": "search_source",
                        "action_subject": "codegen.hpp",
                        "invocation": 1,
                        "line_number": 0,
                        "motivation": "Broader structured probe.",
                        "breakpoints": [],
                    }
                ),
                json.dumps(
                    {
                        "action_type": "run_test",
                        "action_subject": "none",
                        "invocation": 1,
                        "line_number": 0,
                        "motivation": "Retry immediately.",
                        "breakpoints": [],
                    }
                ),
            ],
        ):
            reject, trace = self.mod.choose_best_reject(
                prompt_messages=prompt_messages,
                chosen_action=self.mod.ActionKey("function_info", "gen_binary_op"),
                prompt_history=[self.mod.ActionKey("run_test", "none")],
                track_meta=None,
                args=args,
            )

        self.assertIsNotNone(reject)
        self.assertEqual(reject.action.normalized_type, "search_source")
        self.assertEqual(reject.action.normalized_subject, "codegen.hpp")
        self.assertEqual(reject.reject_kind, "efficiency_negative")
        self.assertEqual(
            trace.sample_outcomes,
            [
                "accepted_weak_reject",
                "action_type_mismatch,action_subject_mismatch,duplicate_in_prompt_history",
            ],
        )

    def test_choose_best_reject_prefers_run_test_over_invalid_json_fallback(self):
        args = SimpleNamespace(
            api_base="http://127.0.0.1:8000/v1",
            endpoint_path="/chat/completions",
            api_key="",
            model="test-model",
            temperature=0.4,
            max_completion_tokens=256,
            timeout_sec=30,
            samples_per_step=2,
            request_retries=1,
            drop_invalid_rejects=False,
            allow_weak_rejects=False,
        )

        prompt_messages = [
            {"role": "system", "content": "Debugger"},
            {"role": "user", "content": "Pick the next step."},
        ]

        with mock.patch.object(
            self.mod,
            "post_chat_completions",
            side_effect=[
                "not-json",
                json.dumps(
                    {
                        "action_type": "run_test",
                        "action_subject": "none",
                        "invocation": 1,
                        "line_number": 0,
                        "motivation": "Retry immediately.",
                        "breakpoints": [],
                    }
                ),
            ],
        ):
            reject, _trace = self.mod.choose_best_reject(
                prompt_messages=prompt_messages,
                chosen_action=self.mod.ActionKey("function_info", "gen_binary_op"),
                prompt_history=[self.mod.ActionKey("run_test", "none")],
                track_meta=None,
                args=args,
            )

        self.assertIsNotNone(reject)
        self.assertEqual(reject.action.normalized_type, "run_test")
        self.assertEqual(reject.reject_kind, "hard_negative")

    def test_process_step_file_writes_trace_for_skipped_step(self):
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
            trace = self.mod.SelectionTrace(
                sample_outcomes=["only_weak_reasons"],
                sample_details=[
                    {
                        "sample_index": 1,
                        "outcome": "only_weak_reasons",
                        "candidate_action": {
                            "action_type": "file_info",
                            "action_subject": "foo.cpp",
                            "invocation": 1,
                            "line_number": 0,
                        },
                    }
                ],
            )
            args = SimpleNamespace(model="test-model", overwrite=True, verbose=False)
            writer = io.StringIO()

            with mock.patch.object(self.mod, "choose_best_reject", return_value=(None, trace)):
                kept = self.mod.process_step_file(
                    step_path=step_path,
                    leaf_dir=leaf,
                    step_meta_index=step_meta_index,
                    train_writer=writer,
                    args=args,
                )

            self.assertFalse(kept)
            self.assertEqual(writer.getvalue(), "")
            trace_payload = json.loads((leaf / "step_10_11_trace.json").read_text(encoding="utf-8"))
            self.assertFalse(trace_payload["kept"])
            self.assertEqual(trace_payload["sample_outcomes"], ["only_weak_reasons"])
            self.assertEqual(trace_payload["samples"][0]["candidate_action"]["action_type"], "file_info")

    def test_process_leaf_dir_overwrite_cleans_stale_outputs(self):
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

            stale_prefer = leaf / "step_10_11_prefer.json"
            stale_reject = leaf / "step_10_11_reject.json"
            stale_pair = leaf / "step_10_11_pair.txt"
            stale_trace = leaf / "step_10_11_trace.json"
            stale_train = leaf / "train_dbg_dpo.jsonl"
            for path in (stale_prefer, stale_reject, stale_pair, stale_trace, stale_train):
                path.write_text("stale", encoding="utf-8")

            args = SimpleNamespace(max_steps=0, overwrite=True)

            with mock.patch.object(self.mod, "process_step_file", return_value=False):
                kept, total = self.mod.process_leaf_dir(leaf, args)

            self.assertEqual((kept, total), (0, 1))
            self.assertFalse(stale_prefer.exists())
            self.assertFalse(stale_reject.exists())
            self.assertFalse(stale_pair.exists())
            self.assertFalse(stale_trace.exists())
            self.assertTrue(stale_train.exists())
            self.assertEqual(stale_train.read_text(encoding="utf-8"), "")


if __name__ == "__main__":
    unittest.main()
