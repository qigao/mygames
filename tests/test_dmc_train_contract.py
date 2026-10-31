"""No-PyTorch admission tests using two REAL C11 offline Self-Play episodes."""
from pathlib import Path
import json
import subprocess
import sys

REPO = Path(__file__).resolve().parents[1]
TRAIN = REPO / "ai" / "dmc" / "train_dmc.py"


def run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(TRAIN), *args],
                          cwd=REPO, text=True, capture_output=True)


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_dmc_train_contract.py train.v1 validation.v1")
    first, second = sys.argv[1:3]
    successful = run("--verify-only", "--replay", first,
                     "--validation-replay", second,
                     "--objective", "win-sign", "--rules-version", "1")
    assert successful.returncode == 0, successful.stderr
    report = json.loads(successful.stdout)
    assert report["episodes"] == 1
    assert report["validation_episodes"] == 1
    assert report["completed_steps"] > 0 and report["validation_steps"] > 0
    assert all(report["role_samples"].values())
    assert report["source_model_versions"] == [12]
    # Do not split transitions from the SAME game into both learner pools.
    overlap = run("--verify-only", "--replay", first,
                  "--validation-replay", first, "--objective", "win-sign")
    assert overlap.returncode != 0
    assert "overlap" in overlap.stderr
    mismatched = run("--verify-only", "--replay", first,
                     "--validation-replay", second,
                     "--objective", "game-points")
    assert mismatched.returncode != 0
    missing_validation = run("--train", "--replay", first,
                             "--objective", "win-sign")
    assert missing_validation.returncode != 0
    assert "--validation-replay" in missing_validation.stderr
    # A frozen model has its OWN immutable ID, newer than source-policy 12.
    # Validate this before importing Torch or creating checkpoint bytes.
    missing_model_id = run("--train", "--replay", first,
                           "--validation-replay", second,
                           "--objective", "win-sign",
                           "--output", "forbidden-untrained.pt")
    assert missing_model_id.returncode != 0
    assert "--model-version" in missing_model_id.stderr
    stale_model_id = run("--train", "--replay", first,
                         "--validation-replay", second,
                         "--objective", "win-sign",
                         "--output", "forbidden-untrained.pt",
                         "--model-version", "12")
    assert stale_model_id.returncode != 0
    assert "candidate model-version must exceed" in stale_model_id.stderr
    verify_with_model = run("--verify-only", "--replay", first,
                            "--objective", "win-sign", "--model-version", "13")
    assert verify_with_model.returncode != 0
    # Verification must not import torch or create any model checkpoint.
    print("mygames.dmc_episode_disjoint_train_contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
