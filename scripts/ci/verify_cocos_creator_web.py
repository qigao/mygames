#!/usr/bin/env python3
"""Run a REAL Cocos Creator Editor Web Desktop build; never emulate Editor.

Requires an installed Cocos Creator 3.8.8 executable in an interactive
Windows/macOS GUI session. GitHub-hosted source tests do not satisfy this gate.
See: https://docs.cocos.com/creator/3.8/manual/en/editor/publish/publish-in-command-line
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
import time
from pathlib import Path


REQUIRED_VERSION = "3.8.8"
BUILD_OUTPUT_NAME = "mygames-m5i-web-desktop"
UUID_PATTERN = re.compile(
    r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"
)


def lobby_scene_uuid(project: Path) -> str:
    """Bind the build to the tracked Lobby.scene, not Editor's last selection."""
    scene_file = project / "assets" / "scenes" / "Lobby.scene"
    meta_file = project / "assets" / "scenes" / "Lobby.scene.meta"
    if not scene_file.is_file() or not meta_file.is_file():
        raise RuntimeError("Tracked Lobby.scene and Lobby.scene.meta are both required")
    meta = json.loads(meta_file.read_text(encoding="utf-8"))
    scene = json.loads(scene_file.read_text(encoding="utf-8"))
    uuid = meta.get("uuid") if isinstance(meta, dict) else None
    if not isinstance(uuid, str) or not UUID_PATTERN.fullmatch(uuid):
        raise RuntimeError("Tracked Lobby.scene.meta has no canonical scene UUID")
    if meta.get("importer") != "scene":
        raise RuntimeError("Lobby.scene.meta is not a Creator scene")
    if not isinstance(scene, list) or len(scene) < 2 or not isinstance(scene[1], dict):
        raise RuntimeError("Lobby.scene does not contain a serialized scene")
    if scene[1].get("__type__") != "cc.Scene" or scene[1].get("_id") != uuid:
        raise RuntimeError("Lobby.scene UUID and scene metadata disagree")
    return uuid


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--creator", required=True,
                        help="Installed CocosCreator.exe or .app/Contents/MacOS/CocosCreator")
    parser.add_argument("--project", default=str(Path(__file__).resolve().parents[2] /
                                                 "apps" / "cocos"))
    parser.add_argument("--report", help="Local JSON evidence path (no secrets or private cards)")
    parser.add_argument("--timeout", type=int, default=900)
    return parser.parse_args()


def run_gate(creator: Path, project: Path, report: Path, timeout: int,
             platform_name: str = None):
    # Injectable only for local self-tests; the CLI always uses sys.platform.
    platform_name = sys.platform if platform_name is None else platform_name
    if platform_name not in ("win32", "darwin"):
        raise RuntimeError("Real Creator 3.8.8 Editor needs Windows/macOS GUI; "
                           "a Linux mocked CLI is not Editor acceptance")
    if not creator.is_file():
        raise RuntimeError("Installed Creator executable not found: " + str(creator))
    manifest = project / "package.json"
    if not manifest.is_file():
        raise RuntimeError("Tracked Cocos package.json is required")
    scene_uuid = lobby_scene_uuid(project)
    package = json.loads(manifest.read_text(encoding="utf-8"))
    configured = package.get("creator", {}).get("version")
    if configured != REQUIRED_VERSION:
        raise RuntimeError("Project must request Creator 3.8.8, found " + str(configured))
    if timeout < 30:
        raise RuntimeError("Timeout must be at least 30 seconds")

    # Never overwrite a project-level scene or copy private ticket/hand data.
    # The Editor must create an actual Web Desktop index.html in this run.
    started = time.time()
    # The official 3.8 CLI supports startScene=<uuid> and outputName.
    # This avoids accidentally bundling a different scene selected in a
    # developer's previous Editor session, and gives a dedicated output.
    cmd = [str(creator), "--project", str(project), "--build",
           ("platform=web-desktop;debug=true;startScene=" + scene_uuid +
            ";outputName=" + BUILD_OUTPUT_NAME)]
    try:
        result = subprocess.run(cmd, timeout=timeout, capture_output=True,
                                text=True, errors="replace", check=False)
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError("Creator Editor build timed out: local GUI session required") from exc
    # The official 3.8 Creator CLI contract documents 36 as build success;
    # 32 and 34 are distinct failure codes. Do not treat process exit 0 as
    # proof of a successful actual Editor build.
    if result.returncode != 36:
        raise RuntimeError("Creator Editor did not report build success "
                           "(documented exit 36; got %d). Inspect local Editor "
                           "console. No Editor/Web acceptance is claimed." %
                           result.returncode)
    output = project / "build" / BUILD_OUTPUT_NAME
    index = output / "index.html"
    application = output / "src" / "application.js"
    if not index.is_file() or index.stat().st_size == 0:
        raise RuntimeError("Creator claimed build success but no nonempty "
                           "dedicated Web Desktop index.html was produced")
    # Creator 3.8's build process also generates src/application.js, the
    # bootstrapping script. Checking both is stronger than a stale HTML shell.
    if not application.is_file() or application.stat().st_size == 0:
        raise RuntimeError("Creator build lacks a nonempty src/application.js")
    if any(path.stat().st_mtime < started - 3 for path in (index, application)):
        raise RuntimeError("Creator build appears stale: index or application.js "
                           "predates this run")

    head = None
    try:
        head_result = subprocess.run(["git", "-C", str(project), "rev-parse", "HEAD"],
                                     capture_output=True, text=True, check=False)
        if head_result.returncode == 0:
            head = head_result.stdout.strip()
    except OSError:
        # An Editor installed on a workstation may not have git on PATH.
        # Report an absent source hash; do not fabricate an exact HEAD.
        pass
    evidence = {
        "gate": "real-cocos-creator-cli-web-desktop",
        "project_requested_creator_version": REQUIRED_VERSION,
        "editor_binary_version_verified": False,
        "editor_runtime_invoked": True,
        "documented_success_exit_code": result.returncode,
        "source_head_sha": head,
        "tracked_start_scene_uuid": scene_uuid,
        "explicit_start_scene": True,
        "output_name": BUILD_OUTPUT_NAME,
        "produced_index": "build/" + BUILD_OUTPUT_NAME + "/index.html",
        "index_size_bytes": index.stat().st_size,
        "index_sha256": file_sha256(index),
        "produced_bootstrap": "build/" + BUILD_OUTPUT_NAME + "/src/application.js",
        "bootstrap_sha256": file_sha256(application),
        "creator_executable_sha256": file_sha256(creator),
        "produced_in_this_run": True,
        "editor_scene_visual_acceptance": False,
        "browser_three_client_visual_acceptance": False,
        "notes": "Verify installed Editor 3.8.8 version, scene import, browser "
                 "UI and missing-script/material warnings separately; "
                 "the project manifest is not proof of installed binary version.",
    }
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    return report


def main():
    args = parse_args()
    try:
        creator = Path(args.creator).expanduser().resolve()
        project = Path(args.project).expanduser().resolve()
        report = (Path(args.report).expanduser().resolve() if args.report else
                  project / "build" / "cocos-editor-m5i-evidence.json")
        completed = run_gate(creator, project, report, args.timeout)
    except (RuntimeError, OSError, ValueError, json.JSONDecodeError) as exc:
        print("Cocos REAL Editor Web gate: NOT ACCEPTED — " + str(exc),
              file=sys.stderr)
        return 1
    print("Cocos real Editor CLI Web Desktop build: SUCCESS (exit 36, "
          "explicit Lobby startScene + fresh index/application.js); "
          "Editor 3.8.8 binary identity, preview and "
          "actual UI visuals remain UNVERIFIED. Evidence: " + str(completed))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
