#!/usr/bin/env python3
"""Run a REAL Cocos Creator Editor Web Desktop build; never emulate Editor.

Requires an installed Cocos Creator 3.8.8 executable in an interactive
Windows/macOS GUI session. GitHub-hosted source tests do not satisfy this gate.
See: https://docs.cocos.com/creator/3.8/manual/en/editor/publish/publish-in-command-line
"""
import argparse
import hashlib
import hmac
import json
import re
import secrets
import subprocess
import sys
import time
from pathlib import Path


REQUIRED_VERSION = "3.8.8"
BUILD_OUTPUT_PREFIX = "mygames-m5i-web-desktop"
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
    parser.add_argument("--expected-head", help="Exact 40-digit git SHA for this build")
    parser.add_argument("--expected-creator-sha256",
                        help="Trusted SHA256 of installed executable, pinned by admin")
    return parser.parse_args()


def run_gate(creator: Path, project: Path, report: Path, timeout: int,
             platform_name: str = None, expected_head: str = None,
             expected_creator_sha256: str = None):
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

    # A project manifest or a runner label cannot prove which executable
    # was run. The trusted SHA pin is mandatory on the opt-in GUI runner.
    creator_hash = file_sha256(creator)
    if expected_creator_sha256 is not None:
        pin = expected_creator_sha256.lower()
        if not re.fullmatch(r"[0-9a-f]{64}", pin):
            raise RuntimeError("Expected Creator SHA256 must be 64 hex characters")
        if not hmac.compare_digest(creator_hash, pin):
            raise RuntimeError("Installed Creator executable SHA256 does not match pin")

    # Evidence must belong to one inspectable source commit. A missing Git
    # binary or an unexpected checkout is not an acceptable exact-head run.
    try:
        head_result = subprocess.run(
            ["git", "-C", str(project), "rev-parse", "HEAD"],
            capture_output=True, text=True, check=False)
    except OSError as exc:
        raise RuntimeError("Git is required for exact source HEAD evidence") from exc
    head = head_result.stdout.strip()
    if head_result.returncode != 0 or not re.fullmatch(r"[0-9a-f]{40}", head):
        raise RuntimeError("Cannot establish exact source HEAD from Git")
    if expected_head is not None:
        expected = expected_head.lower()
        if not re.fullmatch(r"[0-9a-f]{40}", expected) or head != expected:
            raise RuntimeError("Source HEAD differs from expected CI checkout SHA")

    # Every invocation gets a random, previously nonexistent output folder.
    # Never delete existing build output and never accept a previous build
    # merely because its mtime was within three seconds of this invocation.
    output_name = BUILD_OUTPUT_PREFIX + "-" + secrets.token_hex(8)
    output = project / "build" / output_name
    if output.exists() or output.is_symlink():
        raise RuntimeError("Isolated Creator output already exists")
    started = time.time()
    cmd = [str(creator), "--project", str(project), "--build",
           ("platform=web-desktop;debug=true;buildPath=project://build;" +
            "startScene=" + scene_uuid + ";outputName=" + output_name)]
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

    evidence = {
        "gate": "real-cocos-creator-cli-web-desktop",
        "project_requested_creator_version": REQUIRED_VERSION,
        "editor_binary_version_verified": False,
        "editor_runtime_invoked": True,
        "documented_success_exit_code": result.returncode,
        "source_head_sha": head,
        "expected_head_sha_matched": expected_head is not None,
        "tracked_start_scene_uuid": scene_uuid,
        "explicit_start_scene": True,
        "output_name": output_name,
        "output_isolated_per_run": True,
        "produced_index": "build/" + output_name + "/index.html",
        "index_size_bytes": index.stat().st_size,
        "index_sha256": file_sha256(index),
        "produced_bootstrap": "build/" + output_name + "/src/application.js",
        "bootstrap_sha256": file_sha256(application),
        "creator_executable_sha256": creator_hash,
        "creator_sha256_pin_verified": expected_creator_sha256 is not None,
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
        completed = run_gate(
            creator, project, report, args.timeout,
            expected_head=args.expected_head,
            expected_creator_sha256=args.expected_creator_sha256)
    except (RuntimeError, OSError, ValueError, json.JSONDecodeError) as exc:
        print("Cocos REAL Editor Web gate: NOT ACCEPTED — " + str(exc),
              file=sys.stderr)
        return 1
    print("Cocos real Editor CLI Web Desktop build: SUCCESS (exit 36, "
          "explicit Lobby startScene + isolated new index/application.js); "
          "Editor 3.8.8 binary identity, preview and "
          "actual UI visuals remain UNVERIFIED. Evidence: " + str(completed))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
