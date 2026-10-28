"""Tests for the Creator CLI gate *harness*; these are never Editor tests.

Only a real, installed Cocos Creator 3.8.8 binary running inside a GUI
session can satisfy the separate manual Editor/Web acceptance workflow.
"""
import hashlib
import importlib.util
import json
import os
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch


SCENE_UUID = "74bddbba-b612-4236-b8ed-d10f871e0d49"
FAKE_HEAD = "a" * 40
FAKE_CREATOR_BYTES = b"fake-executable-for-harness-contract-only"
FAKE_CREATOR_HASH = hashlib.sha256(FAKE_CREATOR_BYTES).hexdigest()
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
        (self.project / "assets" / "scenes" / "Lobby.scene").write_text(
            json.dumps([{"__type__": "cc.SceneAsset"},
                        {"__type__": "cc.Scene", "_id": SCENE_UUID}]))
        (self.project / "assets" / "scenes" / "Lobby.scene.meta").write_text(
            json.dumps({"importer": "scene", "uuid": SCENE_UUID}))
        (self.project / "package.json").write_text(
            json.dumps({"creator": {"version": "3.8.8"}}))
        self.creator = root / "CocosCreator.exe"
        self.creator.write_bytes(FAKE_CREATOR_BYTES)
        self.report = root / "evidence.json"
        self.output = None
        self.index = None
        self.application = None

    def invoke(self, exit_code=36, produce=True, fresh=True,
               produce_bootstrap=True, expected_head=FAKE_HEAD,
               expected_hash=FAKE_CREATOR_HASH):
        def intercepted(command, **kwargs):
            if command[0] == "git":
                return SimpleNamespace(returncode=0, stdout=FAKE_HEAD + "\n",
                                       stderr="")
            self.assertEqual(command[1:3], ["--project", str(self.project)])
            self.assertEqual(command[3], "--build")
            self.assertIn("startScene=" + SCENE_UUID, command[4])
            parameters = dict(part.split("=", 1)
                              for part in command[4].split(";"))
            self.assertEqual(parameters["buildPath"], "project://build")
            output_name = parameters["outputName"]
            self.assertRegex(output_name,
                             r"^" + GATE.BUILD_OUTPUT_PREFIX + r"-[0-9a-f]{16}$")
            self.output = self.project / "build" / output_name
            self.index = self.output / "index.html"
            self.application = self.output / "src" / "application.js"
            if produce:
                self.index.parent.mkdir(parents=True, exist_ok=True)
                self.index.write_text("<html>dummy harness artifact</html>")
            if produce_bootstrap:
                self.application.parent.mkdir(parents=True, exist_ok=True)
                self.application.write_text("// dummy Creator application.js")
            if not fresh:
                previous = time.time() - 3600
                for path in (self.index, self.application):
                    if path.exists():
                        os.utime(path, (previous, previous))
            return SimpleNamespace(returncode=exit_code, stdout="", stderr="")
        with patch.object(GATE.subprocess, "run", side_effect=intercepted):
            return GATE.run_gate(
                self.creator, self.project, self.report, 40,
                platform_name="win32",
                expected_head=expected_head,
                expected_creator_sha256=expected_hash)

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
        self.assertEqual(evidence["tracked_start_scene_uuid"], SCENE_UUID)
        self.assertTrue(evidence["explicit_start_scene"])
        self.assertRegex(evidence["output_name"],
                         r"^" + GATE.BUILD_OUTPUT_PREFIX + r"-[0-9a-f]{16}$")
        self.assertTrue(evidence["output_isolated_per_run"])
        self.assertEqual(evidence["source_head_sha"], FAKE_HEAD)
        self.assertTrue(evidence["expected_head_sha_matched"])
        self.assertTrue(evidence["creator_sha256_pin_verified"])
        self.assertEqual(len(evidence["index_sha256"]), 64)
        self.assertEqual(len(evidence["bootstrap_sha256"]), 64)
        self.assertEqual(len(evidence["creator_executable_sha256"]), 64)

    def test_prior_artifact_cannot_satisfy_a_new_isolated_run(self):
        old = self.project / "build" / (
            GATE.BUILD_OUTPUT_PREFIX + "-" + "0" * 16) / "index.html"
        old.parent.mkdir(parents=True)
        old.write_text("old build")
        self.assertEqual(self.invoke(), self.report)
        self.assertNotEqual(self.index, old)
        self.assertEqual(old.read_text(), "old build")

    def test_expected_head_mismatch_rejects_before_editor_execution(self):
        with self.assertRaisesRegex(RuntimeError, "Source HEAD differs"):
            self.invoke(expected_head="b" * 40)
        self.assertFalse(self.report.exists())
        self.assertIsNone(self.output)

    def test_wrong_creator_hash_rejects_before_editor_execution(self):
        with self.assertRaisesRegex(RuntimeError, "SHA256 does not match pin"):
            self.invoke(expected_hash="0" * 64)
        self.assertFalse(self.report.exists())
        self.assertIsNone(self.output)

    def test_unpinned_local_run_records_its_limitations(self):
        self.assertEqual(self.invoke(expected_head=None, expected_hash=None),
                         self.report)
        evidence = json.loads(self.report.read_text())
        self.assertFalse(evidence["creator_sha256_pin_verified"])
        self.assertFalse(evidence["expected_head_sha_matched"])

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

    def test_missing_application_js_is_not_accepted(self):
        with self.assertRaisesRegex(RuntimeError, "application.js"):
            self.invoke(produce_bootstrap=False)
        self.assertFalse(self.report.exists())

    def test_lobby_scene_uuid_mismatch_is_not_accepted(self):
        scene_path = self.project / "assets" / "scenes" / "Lobby.scene"
        scene = json.loads(scene_path.read_text())
        scene[1]["_id"] = "00000000-0000-0000-0000-000000000000"
        scene_path.write_text(json.dumps(scene))
        with self.assertRaisesRegex(RuntimeError, "metadata disagree"):
            self.invoke()
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


    def test_m5i_workflow_tracks_app_changes_but_never_auto_admits_gui(self):
        # This tests only the GitHub Actions admission CONTRACT. It cannot
        # substitute for a trusted Windows GUI runner or an Edge render.
        workflow_path = FILE.parents[2] / ".github/workflows/ddz-m5i-cocos-editor-web.yml"
        workflow = workflow_path.read_text(encoding="utf-8")
        pr_trigger = workflow.split("permissions:", 1)[0]
        self.assertIn("types: [opened, synchronize, reopened, labeled]", pr_trigger)
        for tracked in (
            "apps/cocos/**",
            "tests/cocos_gameplay.test.mjs",
            "tests/cocos_creator_gate_test.py",
            "scripts/ci/verify_cocos_creator_web.py",
            "scripts/ci/verify_cocos_web_browser.mjs",
        ):
            self.assertIn("'" + tracked + "'", pr_trigger)
        editor_job = workflow.split("  actual-editor-web-desktop:", 1)[1]
        admission = editor_job.split("    runs-on:", 1)[0]
        self.assertIn("github.event.action == 'labeled'", admission)
        self.assertIn("github.event.label.name == 'run-cocos-editor'", admission)
        self.assertIn("github.event.pull_request.head.repo.full_name == github.repository",
                      admission)
        self.assertIn("runs-on: [self-hosted, Windows, cocos-creator-3.8.8]",
                      editor_job)


if __name__ == "__main__":
    unittest.main()
