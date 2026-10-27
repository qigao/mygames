#!/usr/bin/env python3
"""Run a REAL Cocos Creator Editor Web Desktop build; never emulate Editor.

Requires an installed Cocos Creator 3.8.8 executable in an interactive
Windows/macOS GUI session. GitHub-hosted source tests do not satisfy this gate.
See: https://docs.cocos.com/creator/3.8/manual/en/editor/publish/publish-in-command-line
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path


REQUIRED_VERSION = "3.8.8"


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--creator", required=True,
                        help="Installed CocosCreator.exe or .app/Contents/MacOS/CocosCreator")
    parser.add_argument("--project", default=str(Path(__file__).resolve().parents[2] /
                                                 "apps" / "cocos"))
    parser.add_argument("--report", help="Local JSON evidence path (no secrets or private cards)")
    parser.add_argument("--timeout", type=int, default=900)
    return parser.parse_args()


def run_gate(creator: Path, project: Path, report: Path, timeout: int):
    if sys.platform not in ("win32", "darwin"):
        raise RuntimeError("Real Creator 3.8.8 Editor needs Windows/macOS GUI; "
                           "a Linux mocked CLI is not Editor acceptance")
    if not creator.is_file():
        raise RuntimeError("Installed Creator executable not found: " + str(creator))
    manifest = project / "package.json"
    scene = project / "assets" / "scenes" / "Lobby.scene"
    if not manifest.is_file() or not scene.is_file():
        raise RuntimeError("Real tracked Cocos project and Lobby.scene are required")
    package = json.loads(manifest.read_text(encoding="utf-8"))
    configured = package.get("creator", {}).get("version")
    if configured != REQUIRED_VERSION:
        raise RuntimeError("Project must request Creator 3.8.8, found " + str(configured))
    if timeout < 30:
        raise RuntimeError("Timeout must be at least 30 seconds")

    # Never overwrite a project-level scene or copy private ticket/hand data.
    # The Editor must create an actual Web Desktop index.html in this run.
    started = time.time()
    cmd = [str(creator), "--project", str(project),
           "--build", "platform=web-desktop;debug=true"]
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
    index = project / "build" / "web-desktop" / "index.html"
    if not index.is_file() or index.stat().st_size == 0:
        raise RuntimeError("Creator claimed build success but no nonempty "
                           "build/web-desktop/index.html was produced")
    if index.stat().st_mtime < started - 3:
        raise RuntimeError("Creator build appears stale: index.html predates this run")

    head = None
    head_result = subprocess.run(["git", "-C", str(project), "rev-parse", "HEAD"],
                                 capture_output=True, text=True, check=False)
    if head_result.returncode == 0:
        head = head_result.stdout.strip()
    evidence = {
        "gate": "real-cocos-creator-cli-web-desktop",
        "project_requested_creator_version": REQUIRED_VERSION,
        "editor_binary_version_verified": False,
        "editor_runtime_invoked": True,
        "documented_success_exit_code": result.returncode,
        "source_head_sha": head,
        "produced_index": "build/web-desktop/index.html",
        "index_size_bytes": index.stat().st_size,
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
          "fresh index.html); Editor 3.8.8 binary identity, preview and "
          "actual UI visuals remain UNVERIFIED. Evidence: " + str(completed))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
