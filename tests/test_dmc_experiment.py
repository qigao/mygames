"""Private train/validation/test run verification on genuine C11-generated data.

Run only on the ephemeral private experiment root created by the M5a Linux
native job. No replay bytes, hands or card choices are printed or uploaded.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from ai.dmc.replay import read_episode


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: test_dmc_experiment.py PRIVATE_ROOT NATIVE_GENERATOR HEAD_SHA"
        )
    root = Path(sys.argv[1]).absolute()
    native = Path(sys.argv[2]).absolute()
    source_rev = sys.argv[3]
    manifest_file = root / "manifest.v1.json"
    info = manifest_file.lstat()
    assert stat.S_ISREG(info.st_mode) and stat.S_IMODE(info.st_mode) & 0o077 == 0
    assert info.st_nlink == 1
    manifest_raw = manifest_file.read_bytes()
    data = json.loads(manifest_raw)
    assert data["schema"] == "mygames-private-dmc-experiment-v1"
    assert data["source_revision"] == source_rev
    assert data["native_generator"]["name"] == native.name
    assert data["native_generator"]["sha256"] == hashlib.sha256(
        native.read_bytes()).hexdigest()
    assert data["source_policy"] == "untrained-mixed-baseline-v1"
    assert data["objective"] == "win-sign"
    assert data["rules_version"] == data["model_version"] == 1
    assert set(data["splits"]) == {"train", "validation", "test"}
    assert not (root / "models").exists(), "verify-only must never import Torch or save weights"
    episode_ids = set()
    total = 0
    for split in ("train", "validation", "test"):
        s = data["splits"][split]
        folder = root / split
        directory_info = folder.lstat()
        assert stat.S_ISDIR(directory_info.st_mode)
        assert directory_info.st_uid == os.geteuid()
        assert stat.S_IMODE(directory_info.st_mode) & 0o077 == 0
        assert s["episodes"] == len(s["records"]) == 3
        assert all(v > 0 for v in s["role_counts"].values())
        assert sum(s["role_counts"].values()) == s["decisions"]
        expected_names = {Path(v["file"]).name for v in s["records"]}
        assert {p.name for p in folder.iterdir()} == expected_names
        expected_ids = []
        role_count = [0, 0, 0]
        for row in s["records"]:
            path = root / row["file"]
            assert path.parent == folder
            st = path.lstat()
            assert stat.S_ISREG(st.st_mode) and st.st_uid == os.geteuid()
            assert stat.S_IMODE(st.st_mode) & 0o077 == 0 and st.st_nlink == 1
            sample = read_episode(path)
            assert sample.episode_id == row["episode_id"]
            assert sample.episode_id not in episode_ids
            episode_ids.add(sample.episode_id)
            expected_ids.append(sample.episode_id)
            assert sample.model_version == sample.rules_version == 1
            assert sample.objective == 1
            assert sample.landlord_seat == row["landlord_seat"]
            assert sample.winner_seat == row["winner_seat"]
            assert len(sample.samples) == row["accepted_actions"]
            assert hashlib.sha256(path.read_bytes()).hexdigest() == row["sha256"]
            for step in sample.samples:
                role_count[step.role] += 1
        assert expected_ids == list(range(expected_ids[0], expected_ids[0] + 3))
        assert dict(zip(("landlord", "landlord_up", "landlord_down"), role_count)) == s["role_counts"]
        total += s["decisions"]
    assert len(episode_ids) == 9 and total > 0

    # No regeneration in reuse mode. It must reopen every private Replay
    # and compare actual data against the original no-clobber manifest.
    same = [sys.executable, str(ROOT / "ai/dmc/experiment.py"),
            "--native-generator", str(native), "--private-root", str(root),
            "--source-revision", source_rev,
            "--first-id", str(min(episode_ids)), "--train-games", "3",
            "--validation-games", "3", "--test-games", "3",
            "--objective", "win-sign"]
    reuse = subprocess.run(same + ["--reuse"], cwd=ROOT,
                           capture_output=True, text=True, check=True)
    reused = json.loads(reuse.stdout)
    assert reused["reused_existing_data"] is True
    assert reused["torch_optimization_executed"] is False
    assert reused["verified"]["train_episodes"] == 3
    assert manifest_file.read_bytes() == manifest_raw
    changed_rev = subprocess.run(
        same + ["--reuse", "--source-revision", "0" * 40],
        cwd=ROOT, capture_output=True, text=True
    )
    assert changed_rev.returncode != 0
    assert b"source revision" in changed_rev.stderr.encode("utf-8")
    bad_generator = subprocess.run(
        [*same[:2], "--native-generator", sys.executable, *same[4:], "--reuse"],
        cwd=ROOT, capture_output=True, text=True
    )
    assert bad_generator.returncode != 0
    assert manifest_file.read_bytes() == manifest_raw
    assert len([v for v in root.iterdir() if v.name not in
                ("train", "validation", "test", "manifest.v1.json")]) == 0

    # Complete experiment is immutable: a second invocation MUST NOT alter
    # previously published private bytes, split ownership or metadata.
    duplicate = subprocess.run(
        [sys.executable, str(ROOT / "ai/dmc/experiment.py"),
         "--native-generator", str(native), "--private-root", str(root),
         "--source-revision", source_rev, "--first-id", str(min(episode_ids)),
         "--train-games", "3", "--validation-games", "3",
         "--test-games", "3", "--objective", "win-sign"],
        cwd=ROOT, capture_output=True, text=True
    )
    assert duplicate.returncode != 0
    assert manifest_file.read_bytes() == manifest_raw
    print("mygames.dmc_private_three_way_split: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
