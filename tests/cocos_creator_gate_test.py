"""Tests for the Creator CLI gate *harness*; these are never Editor tests.

Only a real, installed Cocos Creator 3.8.8 binary running inside a GUI
session can satisfy the separate manual Editor/Web acceptance workflow.
"""
import importlib.util
import json
import os
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch


FILE = Path(__file__).resolve().parents[1] / "scripts/ci/verify_cocos_creator_web.py"
SPEC = importlib.util.spec_from_file_location("verify_cocos_creator_web", FILE)
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)


class RealEditorGateHarnessTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mygames-editor-gate-")
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.project = root / "apps" / "cocos"
        (self.project / "assets" / "scenes").mkdir(parents=True)
        (self.project / "assets" / "scenes" / "Lobby.scene").write_text("[]")
        (self.project / "package.json").write_text(
            json.dumps({"creator": {"version": "3.8.8"}}))
        self.creator = root / "CocosCreator.exe"
        self.creator.write_bytes(b"fake-executable-for-harness-contract-only")
        self.report = root / "evidence.json"
        self.index = self.project / "build" / "web-desktop" / "index.html"

    def invoke(self, exit_code=36, produce=True, fresh=True):
        def intercepted(command, **kwargs):
            if command[0] == "git":
                return SimpleNamespace(returncode=1, stdout="", stderr="")
            if produce:
                self.index.parent.mkdir(parents=True, exist_ok=True)
                self.index.write_text("<html>dummy harness artifact</html>")
                if not fresh:
                    previous = time.time() - 3600
                    os.utime(self.index, (previous, previous))
            return SimpleNamespace(returncode=exit_code, stdout="", stderr="")
        with patch.object(GATE.subprocess, "run", side_effect=intercepted):
            return GATE.run_gate(self.creator, self.project, self.report, 40,
                                 platform_name="win32")

    def test_documented_success_with_fresh_output_records_qualified_build_only(self):
        self.assertEqual(self.invoke(), self.report)
        evidence = json.loads(self.report.read_text())
        self.assertEqual(evidence["documented_success_exit_code"], 36)
        self.assertTrue(evidence["editor_runtime_invoked"])
        self.assertTrue(evidence["produced_in_this_run"])
        self.assertFalse(evidence["editor_binary_version_verified"])
        self.assertFalse(evidence["editor_scene_visual_acceptance"])
        self.assertFalse(evidence["browser_three_client_visual_acceptance"])
        self.assertEqual(evidence["project_requested_creator_version"], "3.8.8")

    def test_non_success_exit_including_zero_never_claims_build(self):
        for code in (0, 32, 34, 1):
            with self.subTest(exit_code=code):
                if self.report.exists():
                    self.report.unlink()
                with self.assertRaisesRegex(RuntimeError, "exit 36"):
                    self.invoke(exit_code=code)
                self.assertFalse(self.report.exists())

    def test_success_exit_without_new_index_is_not_accepted(self):
        with self.assertRaisesRegex(RuntimeError, "index.html"):
            self.invoke(produce=False)
        self.assertFalse(self.report.exists())

    def test_stale_index_from_prior_run_is_not_accepted(self):
        with self.assertRaisesRegex(RuntimeError, "stale"):
            self.invoke(fresh=False)
        self.assertFalse(self.report.exists())

    def test_linux_and_wrong_project_version_fail_closed(self):
        with self.assertRaisesRegex(RuntimeError, "Windows/macOS GUI"):
            GATE.run_gate(self.creator, self.project, self.report, 40,
                          platform_name="linux")
        (self.project / "package.json").write_text(
            json.dumps({"creator": {"version": "3.8.7"}}))
        with self.assertRaisesRegex(RuntimeError, "3.8.8"):
            GATE.run_gate(self.creator, self.project, self.report, 40,
                          platform_name="win32")


if __name__ == "__main__":
    unittest.main()
