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
import importlib
from pathlib import Path
import random
import sys
from types import ModuleType

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT / "third_party" / "DouZero" / "douzero"
# Upstream env/__init__.py eagerly imports the full NumPy-backed game Env.
# This CI test exercises ONLY the original stdlib-only move-generator and
# detector files: install import package SEARCH PATHS, not fake methods or
# rewritten game code. Python then loads the actual pinned upstream .py files.
for name, source in (("douzero", UPSTREAM),
                     ("douzero.env", UPSTREAM / "env")):
    package = ModuleType(name)
    package.__path__ = [str(source)]
    sys.modules[name] = package
MovesGener = importlib.import_module("douzero.env.move_generator").MovesGener
get_move_type = importlib.import_module("douzero.env.move_detector").get_move_type

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


def response_parity(lib):
    """Call ORIGINAL GameEnv.get_legal_card_play_actions, not a reimplementation.

    Uses two disjoint actual physical 54-card hands; no invented overlapping
    hidden cards, no second game rules runtime, and no NumPy/Torch import.
    """
    GameEnv = importlib.import_module("douzero.env.game").GameEnv
    beats = lib.mygames_douzero_beats_move
    beats.argtypes = [ct.c_uint64, ct.c_uint64, ct.POINTER(ct.c_bool)]
    beats.restype = ct.c_int
    # Known four-triple-wing rivalry: pinned upstream selector compares the
    # HIGHEST triple including wing; online native only compares the body.
    prior = sorted([3]*3 + [4]*3 + [5]*3 + [17]*3)
    candidate = sorted([6]*3 + [7]*3 + [8]*3 + [12]*3)
    game = GameEnv({})
    game.acting_player_position = "landlord"
    game.info_sets["landlord"].player_hand_cards = candidate[:]
    game.card_play_action_seq = [prior[:]]
    assert candidate not in game.get_legal_card_play_actions()
    accepted = ct.c_bool(True)
    assert beats(mask(candidate), mask(prior), ct.byref(accepted)) == 0
    assert not accepted.value

    rng = random.Random(20261102)
    rival_count = 0
    action_count = 0
    for _ in range(100):
        physical = list(range(54))
        rng.shuffle(physical)
        rival_hand = [RANKS[c//4 if c < 52 else c-39]
                      for c in physical[:17]]
        actor_hand = [RANKS[c//4 if c < 52 else c-39]
                      for c in physical[17:37]]
        rival_options = MovesGener(rival_hand).gen_moves()
        actor_options = [sorted(x) for x in MovesGener(actor_hand).gen_moves()]
        game = GameEnv({})
        game.acting_player_position = "landlord"
        game.info_sets["landlord"].player_hand_cards = actor_hand[:]
        for _ in range(3):
            rival = sorted(rng.choice(rival_options))
            game.card_play_action_seq = [rival]
            original_responses = {
                tuple(x) for x in game.get_legal_card_play_actions() if x
            }
            for cards in actor_options:
                decision = ct.c_bool(False)
                code = beats(mask(cards), mask(rival), ct.byref(decision))
                if code != 0 or bool(decision.value) != (
                        tuple(cards) in original_responses):
                    raise AssertionError(
                        f"native response diverges from original DouZero "
                        f"rival={rival}, candidate={cards}, status={code}")
                action_count += 1
            rival_count += 1
    return rival_count, action_count


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
    rival_count, responses = response_parity(lib)
    print(f"mygames.douzero_native_move_profile: PASS original_actions={compared} "
          f"existing_house_rule_mismatches={legacy_mismatches} "
          f"original_rivals={rival_count} response_action_checks={responses} "
          "online_house_rules_unchanged=true full_env_qualified=false")


if __name__ == "__main__":
    main()
