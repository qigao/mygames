"""Pure-stdlib acceptance of actual C11 private corpus, no fake game engine.

The real native C11 generator has already emitted three OS-shuffled,
complete, canonical Replay files into an ephemeral PRIVATE 0700 folder.
No private Replay bytes or features may be printed or uploaded as artifacts.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from ai.dmc.replay import OBJECTIVE_WIN_SIGN, read_episode


def verify_explicit_native_behavior_sources(generator: Path) -> None:
    """Real C11 completed deals, NOT a model-skill or source-authenticity claim.

    The separate, source-verified cohort manifest is NOT implemented by this
    first slice: these Replay-v1 files must not be fed to a mixed-source
    optimizer using model_version=1 as though the behavior were identical.
    """
    for behavior, eid in (
        ("mixed-v1", 9301),
        ("shed-most", 9302),
        ("legal-index", 9303),
    ):
        with tempfile.TemporaryDirectory(prefix="mygames-dmc-behavior-") as tmp:
            root = Path(tmp)
            assert stat.S_IMODE(root.stat().st_mode) & 0o077 == 0
            args = [
                str(generator), "--private-dir", str(root),
                "--episodes", "1", "--first-id", str(eid),
                "--objective", "win-sign", "--behavior",
            ]
            # An unrecognized source must NOT silently become the old mixed
            # baseline, and no partial private Replay may appear on failure.
            rejected = subprocess.run(
                args + ["unsupported-source"], cwd=ROOT,
                capture_output=True, text=True,
            )
            assert rejected.returncode != 0 and list(root.iterdir()) == []
            accepted = subprocess.run(
                args + [behavior], cwd=ROOT, capture_output=True,
                text=True, check=True,
            )
            match = re.fullmatch(
                r"MYGAMES_PRIVATE_CORPUS_V2 behavior=" + re.escape(behavior) +
                r" episodes=1 steps=([0-9]+) landlord=([0-9]+)"
                r" landlord_up=([0-9]+) landlord_down=([0-9]+)"
                r" objective=win-sign",
                accepted.stdout.strip(),
            )
            assert match is not None
            path = root / f"episode-{eid}.mgdmc"
            assert sorted(x.name for x in root.iterdir()) == [path.name]
            info = path.lstat()
            assert stat.S_ISREG(info.st_mode)
            assert stat.S_IMODE(info.st_mode) & 0o077 == 0
            assert info.st_nlink == 1
            episode = read_episode(path)
            assert episode.episode_id == eid and episode.model_version == 1
            assert episode.rules_version == 1 and episode.objective == OBJECTIVE_WIN_SIGN
            assert episode.landlord_seat == (eid - 1) % 3
            assert episode.samples[0].actor_seat == episode.landlord_seat
            assert episode.samples[-1].actor_seat == episode.winner_seat
            actual_counts = [
                sum(x.role == role for x in episode.samples) for role in range(3)
            ]
            assert all(actual_counts)
            assert [len(episode.samples), *actual_counts] == [
                int(x) for x in match.groups()
            ]
            assert all(s.target in (-1.0, 1.0) for s in episode.samples)
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            no_clobber = subprocess.run(
                args + [behavior], cwd=ROOT, capture_output=True, text=True,
            )
            assert no_clobber.returncode != 0
            assert hashlib.sha256(path.read_bytes()).hexdigest() == digest
            assert sorted(x.name for x in root.iterdir()) == [path.name]


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_dmc_corpus.py PRIVATE_DIR NATIVE_GENERATOR")
    folder, generator = Path(sys.argv[1]), Path(sys.argv[2])
    expected_ids = (9101, 9102, 9103)
    assert folder.is_dir() and generator.is_file()
    assert stat.S_IMODE(folder.stat().st_mode) & 0o077 == 0
    expected_paths = [folder / f"episode-{i}.mgdmc" for i in expected_ids]
    assert sorted(p.name for p in folder.iterdir()) == sorted(
        p.name for p in expected_paths
    ), "no uncommitted/partial Replay artifacts"
    scores = []
    digests = []
    role_sums = {0: 0, 1: 0, 2: 0}
    for eid, path in zip(expected_ids, expected_paths):
        info = path.lstat()
        assert stat.S_ISREG(info.st_mode)
        assert stat.S_IMODE(info.st_mode) & 0o077 == 0
        assert info.st_nlink == 1
        episode = read_episode(path)
        assert episode.episode_id == eid
        assert episode.model_version == 1  # explicitly UNTRAINED mixed baseline
        assert episode.rules_version == 1
        assert episode.objective == OBJECTIVE_WIN_SIGN
        assert episode.landlord_seat == (eid - 1) % 3
        assert episode.winner_seat in range(3)
        assert episode.samples[0].actor_seat == episode.landlord_seat
        assert episode.samples[-1].actor_seat == episode.winner_seat
        for sample in episode.samples:
            assert sample.role in role_sums
            role_sums[sample.role] += 1
            assert sample.target in (-1.0, 1.0)
        scores.append(len(episode.samples))
        digests.append(hashlib.sha256(path.read_bytes()).hexdigest())
    assert all(role_sums.values())
    assert sum(role_sums.values()) == sum(scores)
    train, validation, heldout = map(str, expected_paths)

    base = [sys.executable, "ai/dmc/train_dmc.py"]
    verify = subprocess.run(
        base + ["--verify-only", "--replay", train,
                "--validation-replay", validation, "--objective", "win-sign",
                "--rules-version", "1"],
        cwd=ROOT, capture_output=True, text=True, check=True
    )
    report = json.loads(verify.stdout)
    assert report["episodes"] == 1 and report["validation_episodes"] == 1
    assert report["completed_steps"] == scores[0]
    assert report["validation_steps"] == scores[1]
    overlap = subprocess.run(
        base + ["--verify-only", "--replay", train,
                "--validation-replay", train, "--objective", "win-sign"],
        cwd=ROOT, capture_output=True, text=True
    )
    assert overlap.returncode != 0
    bad_obj = subprocess.run(
        base + ["--verify-only", "--replay", heldout,
                "--objective", "game-points"],
        cwd=ROOT, capture_output=True, text=True
    )
    assert bad_obj.returncode != 0
    # The native publisher MUST refuse to clobber an existing completed
    # episode, even when invoked again using a valid new OS-shuffled deck.
    duplicate = subprocess.run(
        [str(generator), "--private-dir", str(folder), "--episodes", "1",
         "--first-id", str(expected_ids[0]), "--objective", "win-sign"],
        cwd=ROOT, capture_output=True, text=True
    )
    assert duplicate.returncode != 0
    assert [hashlib.sha256(p.read_bytes()).hexdigest()
            for p in expected_paths] == digests
    assert sorted(p.name for p in folder.iterdir()) == sorted(
        p.name for p in expected_paths
    ), "duplicate write did not leave temporary/unsealed artifacts"
    verify_explicit_native_behavior_sources(generator)
    print("mygames.dmc_private_corpus_v1: PASS")
    print("mygames.dmc_explicit_c11_behavior_sources: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
