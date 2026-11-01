#!/usr/bin/env python3
"""Audit ORIGINAL DouZero legal actions against authoritative MyGames C11.

Read-only developer tool; never a new rules engine, training corpus, or bot.
The existing MyGames C11 Round is loaded as a shared library explicitly built
from the real source. The pinned upstream source remains UNCHANGED.

Default is FAIL-CLOSED on ANY disagreement. --report-only is exclusively
for diagnosing currently known incompatible house-rule variants and cannot
qualify an adapter or pretrained checkpoint.
"""
from __future__ import annotations

import argparse
from collections import Counter
import ctypes
import json
from pathlib import Path
import random
import sys

ROOT = Path(__file__).resolve().parents[2]
UPSTREAM = ROOT / "third_party" / "DouZero"
RANKS = tuple(range(3, 15)) + (17, 20, 30)
TO_IDX = {rank: index for index, rank in enumerate(RANKS)}

# Original C11 mygames_ddz_move_kind -> ORIGINAL DouZero TYPE_N_*.
C11_TO_DOUZERO = {
    1: 1, 2: 2, 3: 3, 4: 6, 5: 7, 6: 8, 7: 9, 8: 10,
    9: 11, 10: 12, 11: 13, 12: 14, 13: 4, 14: 5,
}


class NativeMove(ctypes.Structure):
    _fields_ = [
        ("kind", ctypes.c_int),
        ("high_rank", ctypes.c_uint8),
        ("chain_length", ctypes.c_uint8),
        ("card_count", ctypes.c_uint8),
    ]


def physical_mask(cards: list[int]) -> int:
    """Use only canonical rank->physical-ID mapping, no game state/hands."""
    counts = Counter(cards)
    result = 0
    for rank, count in counts.items():
        if rank not in TO_IDX:
            raise ValueError("unknown original DouZero rank")
        index = TO_IDX[rank]
        capacity = 1 if index >= 13 else 4
        if not 1 <= count <= capacity:
            raise ValueError("invalid physical rank multiplicity")
        for suit in range(count):
            result |= 1 << (index * 4 + suit if index < 13
                            else 52 + index - 13)
    return result


def classify_native(lib, cards):
    result = NativeMove()
    status = lib.mygames_ddz_classify(
        physical_mask(cards), ctypes.byref(result))
    if status:
        return None
    return C11_TO_DOUZERO[result.kind]


def audit(library: Path, hands: int, seed: int) -> dict:
    if not library.is_file() or library.is_symlink():
        raise ValueError("explicit compiled authoritative C11 library required")
    if not 1 <= hands <= 10000 or not 0 <= seed < (1 << 64):
        raise ValueError("bounded deterministic PARITY-ONLY sample required")
    # No copy of the original policy/classifier implementation.
    sys.path.insert(0, str(UPSTREAM))
    from douzero.env.move_generator import MovesGener
    from douzero.env.move_detector import get_move_type

    lib = ctypes.CDLL(str(library.resolve(strict=True)))
    classifier = lib.mygames_ddz_classify
    classifier.argtypes = [ctypes.c_uint64, ctypes.POINTER(NativeMove)]
    classifier.restype = ctypes.c_int

    rng = random.Random(seed)  # not production entropy; audit reproducibility
    observed = 0
    agreed = 0
    source_invalid = 0
    errors = Counter()
    first_examples = {}
    for _ in range(hands):
        hand = [RANKS[card_id // 4 if card_id < 52 else card_id - 39]
                for card_id in rng.sample(range(54), 20)]
        for generated in MovesGener(hand).gen_moves():
            action = sorted(generated)
            original = get_move_type(action)["type"]
            actual = classify_native(lib, action)
            observed += 1
            if original == 15:
                source_invalid += 1
            elif actual == original:
                agreed += 1
            else:
                key = f"upstream_type_{original}_c11_{actual or 'REJECT'}"
                errors[key] += 1
                first_examples.setdefault(key, action)

    # Fixed counterexamples are independent of random hand sampling.
    # They can demonstrate mismatched classifier semantics even if not
    # generated as candidates in the chosen random test sample.
    cases = {
        "four_triples_as_airplane_singles":
            [7, 7, 7, 8, 8, 8, 9, 9, 9, 17, 17, 17],
        "two_distinct_quads":
            [3, 3, 3, 3, 4, 4, 4, 4],
    }
    fixed = {
        label: {
            "upstream_type": get_move_type(cards)["type"],
            "native_type": classify_native(lib, cards),
        } for label, cards in cases.items()
    }
    compatible = not errors and not source_invalid and all(
        row["upstream_type"] == row["native_type"]
        for row in fixed.values()
    )
    return {
        "schema": "mygames-douzero-native-rules-parity-audit-v1",
        "original_upstream_commit":
            "718a5c920bf3361e34178a38f3b80458e176b351",
        "hand_count": hands,
        "deterministic_audit_seed": seed,
        "generated_candidate_actions": observed,
        "matched_candidate_actions": agreed,
        "upstream_generator_invalid_actions": source_invalid,
        "mismatch_counts": dict(errors),
        "first_mismatch_actions_in_rank_notation": first_examples,
        "fixed_classifier_counterexamples": fixed,
        "C11_adapter_compatible": compatible,
        "inference_or_model_qualified": False,
        "training_data_generated": False,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--c11-library", required=True, type=Path)
    parser.add_argument("--hands", type=int, default=100)
    parser.add_argument("--seed", type=int, default=20261102)
    parser.add_argument("--report-only", action="store_true",
                        help="diagnostic only; never a green compatibility gate")
    args = parser.parse_args()
    summary = audit(args.c11_library, args.hands, args.seed)
    print(json.dumps(summary, sort_keys=True))
    if not summary["C11_adapter_compatible"] and not args.report_only:
        raise SystemExit("C11↔original DouZero move parity RED; adapter disabled")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
