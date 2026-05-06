#!/usr/bin/env python3

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace


SCRIPT_PATH = Path(__file__).with_name("build_hen_cpt.py")
MODULE_NAME = "build_hen_cpt_under_test"


def load_module():
    spec = importlib.util.spec_from_file_location(MODULE_NAME, SCRIPT_PATH)
    module = importlib.util.module_from_spec(spec)
    sys.modules[MODULE_NAME] = module
    spec.loader.exec_module(module)
    return module


def write_json(path: Path, payload):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload), encoding="utf-8")


def request_payload(prompt: str, messages=None):
    return {
        "llm": "test-model",
        "requested_llm": "test-requested",
        "prompt": prompt,
        "time": "2026-05-04_12-00-00",
        "messages": messages or [{"role": "user", "content": "PROMPT BOILERPLATE MUST NOT LEAK"}],
    }


def response_payload(content: str):
    return {
        "finish_reason": "stop",
        "message": {
            "role": "assistant",
            "content": content,
        },
    }


class BuildHenCptTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mod = load_module()

    def args(self, root: Path, output: Path, **overrides):
        base = {
            "log_roots": [str(root)],
            "output_dir": str(output),
            "debug_output": "hen-cpt-debug.jsonl",
            "nodes_output": "hen-cpt-nodes.jsonl",
            "include_nodes": True,
            "debug_prompts": "SystemAnalysis,NextStep,SummarizeTrajectory",
            "node_prompts": "Programming,ReviewSource",
            "include_reasoning": False,
            "include_dynamic_request_context": False,
            "stable_message_min_count": 20,
            "max_request_context_chars": 120000,
            "max_section_chars": 60000,
            "max_doc_chars": 180000,
            "min_content_chars": 1,
            "redact_paths": True,
            "overwrite": True,
        }
        base.update(overrides)
        return SimpleNamespace(**base)

    def test_exports_debug_from_response_json_without_request_prompt(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td) / "run_a" / "logs"
            out = Path(td) / "out"

            write_json(
                root / "debug" / "S0" / "step_1" / "request_10_SystemAnalysis.json",
                request_payload("SystemAnalysis"),
            )
            write_json(
                root / "debug" / "S0" / "step_1" / "response_10_SystemAnalysis.json",
                response_payload("analysis says /Users/georvn/projects/hen and sk-abcdefghijklmnopqrstuvwxyz"),
            )
            write_json(
                root / "debug" / "S0" / "step_1" / "request_11_NextStep.json",
                request_payload("NextStep"),
            )
            write_json(
                root / "debug" / "S0" / "step_1" / "response_11_NextStep.json",
                response_payload('{"action_type":"function_info","action_subject":"main"}'),
            )

            stats = self.mod.export_cpt(self.args(root, out, include_nodes=False))

            self.assertEqual(stats.debug_docs, 1)
            payload = json.loads((out / "hen-cpt-debug.jsonl").read_text())
            text = payload["text"]
            self.assertIn("SYSTEM_ANALYSIS:", text)
            self.assertIn("NEXT_STEP:", text)
            self.assertIn("/Users/<user>/projects/hen", text)
            self.assertIn("<redacted-secret>", text)
            self.assertNotIn("PROMPT BOILERPLATE MUST NOT LEAK", text)
            self.assertEqual(payload["meta"]["run"], "run_a")
            self.assertEqual(payload["meta"]["test"], "S0")
            self.assertEqual(payload["meta"]["step"], 1)

    def test_dynamic_request_context_drops_stable_and_static_messages(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td) / "run_ctx" / "logs"
            out = Path(td) / "out"

            repeated_static = "REPEATED STATIC CONTRACT"
            write_json(
                root / "debug" / "S0" / "step_1" / "request_10_SystemAnalysis.json",
                request_payload(
                    "SystemAnalysis",
                    [
                        {"role": "system", "content": repeated_static},
                        {"role": "user", "content": "PROJECT DESCRIPTION: static project block"},
                        {"role": "user", "content": "LAST RUN INFO: failing assertion at line 42"},
                    ],
                ),
            )
            write_json(
                root / "debug" / "S0" / "step_1" / "response_10_SystemAnalysis.json",
                response_payload("analysis based on line 42"),
            )
            write_json(
                root / "debug" / "S0" / "step_1" / "request_11_NextStep.json",
                request_payload(
                    "NextStep",
                    [
                        {"role": "system", "content": repeated_static},
                        {"role": "user", "content": "CURRENT STEP: request a corrected action"},
                    ],
                ),
            )
            write_json(
                root / "debug" / "S0" / "step_1" / "response_11_NextStep.json",
                response_payload('{"action_type":"debug_function","action_subject":"parse_expr"}'),
            )

            stats = self.mod.export_cpt(
                self.args(
                    root,
                    out,
                    include_nodes=False,
                    include_dynamic_request_context=True,
                    stable_message_min_count=2,
                )
            )

            self.assertEqual(stats.debug_docs, 1)
            payload = json.loads((out / "hen-cpt-debug.jsonl").read_text())
            text = payload["text"]
            self.assertIn("SYSTEM_ANALYSIS_REQUEST_CONTEXT:", text)
            self.assertIn("LAST RUN INFO: failing assertion at line 42", text)
            self.assertIn("CURRENT STEP: request a corrected action", text)
            self.assertNotIn(repeated_static, text)
            self.assertNotIn("PROJECT DESCRIPTION", text)
            self.assertEqual(stats.request_context_messages_dropped_stable, 2)
            self.assertEqual(stats.request_context_messages_dropped_heading, 1)

    def test_exports_nodes_to_separate_file_when_enabled(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td) / "run_b" / "logs"
            out = Path(td) / "out"

            write_json(
                root / "nodes" / "gen_expr" / "request_20_Programming.json",
                request_payload("Programming"),
            )
            write_json(
                root / "nodes" / "gen_expr" / "response_20_Programming.json",
                response_payload("```cpp\nint gen_expr() { return 1; }\n```"),
            )
            write_json(
                root / "nodes" / "gen_expr" / "request_21_UnwantedPrompt.json",
                request_payload("UnwantedPrompt"),
            )
            write_json(
                root / "nodes" / "gen_expr" / "response_21_UnwantedPrompt.json",
                response_payload("do not include me"),
            )

            stats = self.mod.export_cpt(self.args(root, out))

            self.assertEqual(stats.node_docs, 1)
            payload = json.loads((out / "hen-cpt-nodes.jsonl").read_text())
            text = payload["text"]
            self.assertIn("<hen_node_cpt>", text)
            self.assertIn("node: gen_expr", text)
            self.assertIn("int gen_expr()", text)
            self.assertNotIn("do not include me", text)


if __name__ == "__main__":
    unittest.main()
