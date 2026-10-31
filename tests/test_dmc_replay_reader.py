#!/usr/bin/env python3
"""Cross-language PRIVATE DMC v1 acceptance with a REAL C11 game fixture.

Requires only Python's standard library. Does not deserialize arbitrary
pickle or need a mock PyTorch training engine.
"""
from pathlib import Path
import json
import struct
import subprocess
import sys
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from ai.dmc.replay import (  # noqa: E402
    HEADER_BYTES, RECORD_BYTES, OBJECTIVE_GAME_POINTS,
    parse_episode, read_episode,
)


def expect_invalid(raw: bytes, reason: str) -> None:
    try:
        parse_episode(raw)
    except ValueError:
        return
    raise AssertionError(f"unexpectedly accepted invalid replay: {reason}")


def with_fixed_crc(raw: bytes) -> bytes:
    data = bytearray(raw)
    struct.pack_into("<I", data, len(data) - 4,
                     zlib.crc32(data[:-4]) & 0xFFFFFFFF)
    return bytes(data)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_dmc_replay_reader.py <C11 private fixture>")
    path = Path(sys.argv[1])
    raw = path.read_bytes()
    episode = read_episode(path)
    assert episode.episode_id == 77
    assert episode.model_version == 2
    assert episode.rules_version == 1
    assert episode.objective == OBJECTIVE_GAME_POINTS
    assert episode.landlord_seat == episode.winner_seat == 0
    assert len(episode.samples) == 58
    assert {r: sum(s.role == r for s in episode.samples) for r in range(3)} == {
        0: 20, 1: 19, 2: 19
    }
    assert all(s.turn_id == i + 1 for i, s in enumerate(episode.samples))
    assert episode.samples[0].z == bytes(810)
    assert any(episode.samples[-1].z)
    assert all(s.target == (12.0 if s.actor_seat == 0 else -6.0)
               for s in episode.samples)
    assert all(len(s.x) == (319 if s.role == 0 else 430)
               and len(s.z) == 810 and len(s.action) == 54
               for s in episode.samples)
    assert len(raw) == HEADER_BYTES + RECORD_BYTES * 58 + 4

    tampered = bytearray(raw)
    tampered[HEADER_BYTES + 32] ^= 1
    expect_invalid(bytes(tampered), "CRC32 byte corruption")
    tampered = bytearray(raw)
    tampered[HEADER_BYTES + 16] = 99  # invalid actor seat
    expect_invalid(with_fixed_crc(tampered), "forged actor with rechecksummed CRC")
    tampered = bytearray(raw)
    tampered[HEADER_BYTES + 24] ^= 1  # change target, then rechecksum
    expect_invalid(with_fixed_crc(tampered), "wrong team reward")
    tampered = bytearray(raw)
    tampered[46] = 1
    expect_invalid(with_fixed_crc(tampered), "nonzero reserved")
    expect_invalid(raw + b"\x00", "trailing data")
    expect_invalid(raw[:-1], "truncated CRC")
    tampered = bytearray(raw)
    tampered[8] = 2
    expect_invalid(with_fixed_crc(tampered), "wrong version")

    # The verified pure-stdlib reader must also be the parser used by
    # the actual PyTorch trainer in --verify-only mode (no torch dependency).
    proc = subprocess.run(
        [sys.executable, "ai/dmc/train_dmc.py", "--verify-only",
         "--replay", str(path), "--objective", "game-points",
         "--rules-version", "1"],
        cwd=Path(__file__).resolve().parents[1],
        capture_output=True, text=True, check=True
    )
    summary = json.loads(proc.stdout)
    assert summary["episodes"] == 1
    assert summary["completed_steps"] == 58
    assert summary["role_samples"] == {
        "landlord": 20, "landlord_up": 19, "landlord_down": 19
    }
    bad = subprocess.run(
        [sys.executable, "ai/dmc/train_dmc.py", "--verify-only",
         "--replay", str(path), "--objective", "win-sign"],
        cwd=Path(__file__).resolve().parents[1],
        capture_output=True, text=True
    )
    assert bad.returncode != 0
    print("mygames.dmc_replay_cross_language: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
