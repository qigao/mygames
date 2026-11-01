#!/usr/bin/env python3
"""Compare exact original DouZero reward to REAL MyGames C11 settlement.

Development-only, no second scoring engine and no change to upstream source.
The supplied shared library MUST be compiled from authoritative
games/doudizhu/src/{ddz_match,ddz_round,room}.c.

A positive WP result proves only team WIN/LOSS SIGN semantics; it does NOT
prove exact legal-action, bidding, ADP, spring, or full settlement equality.
"""
from __future__ import annotations

import argparse
import ctypes
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
UPSTREAM = ROOT / "third_party" / "DouZero"


class NativeCounters(ctypes.Structure):
    _fields_ = [
        ("bombs_rockets", ctypes.c_uint8),
        ("landlord_nonpass", ctypes.c_uint8),
        ("farmers_nonpass", ctypes.c_uint8),
    ]


class NativeSettlement(ctypes.Structure):
    _fields_ = [
        ("bid_base", ctypes.c_uint8),
        ("bombs_rockets", ctypes.c_uint8),
        ("spring", ctypes.c_int),
        ("landlord_seat", ctypes.c_uint8),
        ("winner_seat", ctypes.c_uint8),
        ("multiplier", ctypes.c_uint32),
        ("unit", ctypes.c_uint32),
        ("delta", ctypes.c_int32 * 3),
    ]


class RewardContext:
    """Context only; delegates ALL reward computation to original Env."""
    def __init__(self, won: bool, bombs: int, objective: str):
        self.objective = objective
        self._game_winner = "landlord" if won else "farmer"
        self._game_bomb_num = bombs


def audit(library: Path) -> dict:
    if library.is_symlink() or not library.is_file():
        raise ValueError("explicit authentic native C11 Match library required")
    sys.path.insert(0, str(UPSTREAM))
    from douzero.env.env import Env

    native = ctypes.CDLL(str(library.resolve(strict=True)))
    scorer = native.mygames_ddz_score_compute
    scorer.argtypes = [
        ctypes.c_uint8, NativeCounters, ctypes.c_uint8,
        ctypes.c_uint8, ctypes.POINTER(NativeSettlement),
    ]
    scorer.restype = ctypes.c_int

    valid, wp_equal, adp_different, first_gap = 0, 0, 0, None
    for base in (1, 2, 3):
        for landlord in (0, 1, 2):
            for winner in (0, 1, 2):
                for bombs in (0, 1, 2):
                    for landlord_moves in (1, 2, 3):
                        for farmer_moves in (0, 1, 2, 3):
                            out = NativeSettlement()
                            status = scorer(base, NativeCounters(
                                bombs, landlord_moves, farmer_moves),
                                landlord, winner, ctypes.byref(out))
                            if status:
                                continue  # original C11 rejects invalid counters
                            valid += 1
                            won = winner == landlord
                            wp = Env._get_reward(
                                RewardContext(won, bombs, "wp"))
                            adp = Env._get_reward(
                                RewardContext(won, bombs, "adp"))
                            if (sum(out.delta) != 0 or
                                    out.landlord_seat != landlord or
                                    out.winner_seat != winner):
                                raise ValueError("C11 settlement invariant violation")
                            native_win_sign = (1.0 if out.delta[landlord] > 0
                                               else -1.0)
                            if wp == native_win_sign:
                                wp_equal += 1
                            native_unit = abs(out.delta[landlord]) / 2.0
                            if abs(adp) != native_unit:
                                adp_different += 1
                                if first_gap is None:
                                    first_gap = {
                                        "bid_base": base,
                                        "bombs": bombs,
                                        "spring": out.spring,
                                        "native_unit": out.unit,
                                        "original_douzero_adp_abs": abs(adp),
                                    }
    return {
        "schema": "mygames-original-douzero-c11-reward-parity-v1",
        "original_upstream_commit":
            "718a5c920bf3361e34178a38f3b80458e176b351",
        "valid_native_settlement_cases": valid,
        "original_wp_sign_equal_to_native_landlord_team": wp_equal,
        "original_adp_abs_differs_from_native_unit": adp_different,
        "first_adp_counterexample": first_gap,
        "wp_sign_compatible": valid > 0 and valid == wp_equal,
        "adp_value_compatible": valid > 0 and adp_different == 0,
        "full_game_rules_compatible": False,  # separate move/bid gate
        "production_score_ledger_replaced": False,
        "trained_model_qualified": False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--c11-match-library", required=True, type=Path)
    parser.add_argument("--objective", choices=("wp", "adp"), default="wp")
    parser.add_argument("--report-only", action="store_true",
                        help="diagnostics only, never a green objective gate")
    args = parser.parse_args()
    result = audit(args.c11_match_library)
    print(json.dumps(result, sort_keys=True))
    key = "wp_sign_compatible" if args.objective == "wp" else "adp_value_compatible"
    if not result[key] and not args.report_only:
        raise SystemExit("original DouZero objective disagrees with C11 game score")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
