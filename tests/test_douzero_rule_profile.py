"""Positive original DouZero legal-candidate vs actual C11 offline profile.

Uses the pinned UNMODIFIED upstream generator/detector and SAME authoritative
C11 classifier, with only two explicitly documented offline-only variations.
No Torch/NumPy, game data collection, second game engine, fake moves, or CI
probe. This validates CLASSIFICATION only: full environment parity is RED.
"""
from __future__ import annotations

import argparse
from collections import Counter
import ctypes as ct
from pathlib import Path
import random
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "third_party" / "DouZero"))
from douzero.env.move_generator import MovesGener
from douzero.env.move_detector import get_move_type

RANKS = tuple(range(3, 15)) + (17, 20, 30)
INDEX = {r: i for i, r in enumerate(RANKS)}
C11_TO_DOUZERO = {1:1, 2:2, 3:3, 4:6, 5:7, 6:8, 7:9,
                   8:10, 9:11, 10:12, 11:13, 12:14, 13:4, 14:5}


class Move(ct.Structure):
    _fields_ = [("kind", ct.c_int), ("high_rank", ct.c_uint8),
                ("chain_length", ct.c_uint8), ("card_count", ct.c_uint8)]


def mask(cards):
    counts = Counter(cards)
    bits = 0
    for rank, count in counts.items():
        i = INDEX[rank]
        capacity = 1 if i >= 13 else 4
        assert 1 <= count <= capacity
        for k in range(count):
            bits |= 1 << (4*i+k if i < 13 else 52+i-13)
    return bits


def classify(fn, cards):
    output = Move()
    code = fn(mask(cards), ct.byref(output))
    return None if code else output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--offline-library", type=Path, required=True)
    parser.add_argument("--hands", type=int, default=100)
    args = parser.parse_args()
    if not 1 <= args.hands <= 1000 or not args.offline_library.is_file():
        parser.error("real compiled C11 offline profile and bounded hands required")
    lib = ct.CDLL(str(args.offline_library.absolute()))
    original = lib.mygames_ddz_classify
    offline = lib.mygames_douzero_classify_move
    for fn in (original, offline):
        fn.argtypes = [ct.c_uint64, ct.POINTER(Move)]
        fn.restype = ct.c_int

    # Exact known upstream compatibility divergences, both distinct from
    # the unchanged production house-rule classifier.
    airplane = [7]*3 + [8]*3 + [9]*3 + [17]*3
    double_quads = [3]*4 + [4]*4
    assert classify(original, airplane) is None
    assert classify(original, double_quads) is None
    assert get_move_type(sorted(airplane))["type"] == 11
    assert get_move_type(sorted(double_quads))["type"] == 14
    m = classify(offline, airplane)
    assert (m.kind, m.high_rank, m.chain_length, m.card_count) == (9, 6, 3, 12)
    m = classify(offline, double_quads)
    assert (m.kind, m.high_rank, m.chain_length, m.card_count) == (12, 1, 1, 8)
    assert classify(offline, [3]*3+[5]*3+[7]*3+[17]*3) is None
    assert classify(offline, [7]*4+[8]*4+[9]*3) is None
    guard = Move(13, 11, 1, 4)
    assert offline(0, ct.byref(guard)) != 0
    assert offline(1 << 54, ct.byref(guard)) != 0
    assert (guard.kind, guard.high_rank, guard.card_count) == (13, 11, 4)

    rng = random.Random(20261102)
    compared = 0
    legacy_mismatches = 0
    for _ in range(args.hands):
        hand = [RANKS[c//4 if c < 52 else c-39]
                for c in rng.sample(range(54), 20)]
        for cards in MovesGener(hand).gen_moves():
            candidate = sorted(cards)
            expected = get_move_type(candidate)
            if expected["type"] == 15:
                raise AssertionError("original DouZero move generator yielded invalid move")
            old = classify(original, candidate)
            if old is None or C11_TO_DOUZERO[old.kind] != expected["type"]:
                legacy_mismatches += 1
            accepted = classify(offline, candidate)
            if accepted is None or C11_TO_DOUZERO[accepted.kind] != expected["type"]:
                raise AssertionError(f"offline profile deviates from original {candidate} {expected}")
            assert accepted.card_count == len(candidate)
            if "rank" in expected:
                sequence = expected["type"] in (8, 9, 10, 11, 12)
                official_high = (INDEX[expected["rank"]] +
                                 (expected["len"] - 1 if sequence else 0))
                if accepted.high_rank != official_high:
                    raise AssertionError(
                        f"original/C11 primary-rank mismatch: {candidate} "
                        f"{expected}, native_high={accepted.high_rank}")
                if sequence and accepted.chain_length != expected["len"]:
                    raise AssertionError("original/C11 sequence-length mismatch")
            compared += 1
    assert compared >= args.hands * 20
    print(f"mygames.douzero_native_move_profile: PASS original_actions={compared} "
          f"existing_house_rule_mismatches={legacy_mismatches} "
          "online_house_rules_unchanged=true full_env_qualified=false")


if __name__ == "__main__":
    main()
