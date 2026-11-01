"""Whole-round state parity: unchanged DouZero GameEnv vs authoritative MyGames C11.

Both original engines play the SAME 54 physical cards (unique, no hidden-hand
overlap). A deterministic TEST-ONLY actor chooses from the INTERSECTION of
original legal actions and existing online C11 Round actions. The profile is
used to compare the complete candidate-membership set, not to bypass online
native move submission. No training, new rule engine, production entropy,
user data, model weights, second Room Owner, or new runtime API.
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
# Only the real upstream stdlib game implementation is needed; original
# package __init__.py eagerly imports NumPy even for unused model Env logic.
for package_name, folder in (
    ("douzero", UPSTREAM), ("douzero.env", UPSTREAM / "env")
):
    pkg = ModuleType(package_name)
    pkg.__path__ = [str(folder)]
    sys.modules[package_name] = pkg
GameEnv = importlib.import_module("douzero.env.game").GameEnv
MovesGener = importlib.import_module("douzero.env.move_generator").MovesGener

RANKS = tuple(range(3, 15)) + (17, 20, 30)
SEATS = ("landlord", "landlord_down", "landlord_up")
C11_OK, C11_NOT_YOUR_TURN, C11_STALE_TURN = 0, 5, 6


class Move(ct.Structure):
    _fields_ = [
        ("kind", ct.c_int), ("high_rank", ct.c_uint8),
        ("chain_length", ct.c_uint8), ("card_count", ct.c_uint8),
    ]


class Round(ct.Structure):
    _fields_ = [
        ("hands", ct.c_uint64 * 3), ("played_cards", ct.c_uint64),
        ("last_cards", ct.c_uint64), ("turn_id", ct.c_uint64),
        ("last_move", Move), ("remaining", ct.c_uint8 * 3),
        ("landlord_seat", ct.c_uint8), ("turn_seat", ct.c_uint8),
        ("last_seat", ct.c_uint8), ("passes", ct.c_uint8),
        ("winner_seat", ct.c_uint8), ("finished", ct.c_bool),
    ]


class Result(ct.Structure):
    _fields_ = [
        ("next_turn_id", ct.c_uint64), ("played_cards", ct.c_uint64),
        ("move", Move), ("acting_seat", ct.c_uint8),
        ("next_seat", ct.c_uint8), ("winner_seat", ct.c_uint8),
        ("remaining", ct.c_uint8 * 3), ("passed", ct.c_bool),
        ("trick_cleared", ct.c_bool), ("finished", ct.c_bool),
    ]


class FixedTestActor:
    def __init__(self):
        self.action = None

    def act(self, infoset):
        assert self.action in infoset.legal_actions
        return self.action


def card_rank(card_id):
    return RANKS[card_id // 4 if card_id < 52 else card_id - 39]


def mask_from_own_hand(physical_hand, action):
    """Select only actual cards from the current actor's OWN native hand."""
    result = 0
    for rank, needed in Counter(action).items():
        available = [
            i for i in range(54)
            if (physical_hand >> i) & 1 and card_rank(i) == rank
        ]
        if len(available) < needed:
            raise AssertionError("an action tried using another seat's hand")
        for i in available[:needed]:
            result |= 1 << i
    return result


def ranks(mask):
    return Counter(card_rank(i) for i in range(54) if (mask >> i) & 1)


def shared_deck(rng):
    """Deal identical card ownership to original GameEnv and M5j Round.

    C11 round_deal takes 17 cards cyclically at each physical seat plus
    the last three kitty cards for landlord. Original GameEnv uses fixed
    20/17/17 landlord/down/up positions. No bid parity is implied.
    """
    cards = list(range(54))
    rng.shuffle(cards)
    hands = (cards[:20], cards[20:37], cards[37:54])
    kitty = hands[0][-3:]
    deck = [0] * 54
    for seat, basic in enumerate((hands[0][:-3], hands[1], hands[2])):
        for i, physical_card in enumerate(basic):
            deck[i * 3 + seat] = physical_card
    deck[51:] = kitty
    assert sorted(deck) == list(range(54))
    return deck, hands, kitty


def native_library(library):
    if library.is_symlink() or not library.is_file():
        raise ValueError("real MyGames C11 shared test target is required")
    lib = ct.CDLL(str(library.absolute()))
    signatures = (
        ("mygames_ddz_round_deal",
         (ct.POINTER(Round), ct.POINTER(ct.c_uint8), ct.c_uint)),
        ("mygames_ddz_round_validate", (ct.POINTER(Round),)),
        ("mygames_ddz_round_submit",
         (ct.POINTER(Round), ct.c_uint, ct.c_uint64, ct.c_uint64,
          ct.POINTER(Result))),
        ("mygames_ddz_classify", (ct.c_uint64, ct.POINTER(Move))),
        ("mygames_douzero_beats_move",
         (ct.c_uint64, ct.c_uint64, ct.POINTER(ct.c_bool))),
    )
    for name, args in signatures:
        fn = getattr(lib, name)
        fn.argtypes = args
        fn.restype = ct.c_int
    return lib


def audit(library, number_of_games, seed):
    if not 1 <= number_of_games <= 100 or not 0 <= seed < 2 ** 64:
        raise ValueError("bounded, deterministic developer-only comparison")
    lib = native_library(library)
    rng = random.Random(seed)
    counts = Counter()
    for game_index in range(number_of_games):
        deck, hands, kitty = shared_deck(rng)
        actors = {name: FixedTestActor() for name in SEATS}
        upstream = GameEnv(actors)
        upstream.card_play_init({
            "landlord": sorted(map(card_rank, hands[0])),
            "landlord_down": sorted(map(card_rank, hands[1])),
            "landlord_up": sorted(map(card_rank, hands[2])),
            "three_landlord_cards": sorted(map(card_rank, kitty)),
        })
        native = Round()
        cdeck = (ct.c_uint8 * 54)(*deck)
        assert lib.mygames_ddz_round_deal(ct.byref(native), cdeck, 0) == C11_OK
        assert native.turn_id == 1 and native.turn_seat == 0
        start_actions = counts["actions"]
        while not native.finished:
            if counts["actions"] - start_actions >= 500:
                raise AssertionError("test policy did not finish the game")
            assert lib.mygames_ddz_round_validate(ct.byref(native)) == C11_OK
            seat = int(native.turn_seat)
            position = SEATS[seat]
            assert upstream.acting_player_position == position
            for s in range(3):
                assert ranks(native.hands[s]) == Counter(
                    upstream.info_sets[SEATS[s]].player_hand_cards)
                assert native.remaining[s] == len(
                    upstream.info_sets[SEATS[s]].player_hand_cards)
            assert ranks(native.last_cards) == Counter(upstream.get_last_move())

            legal = upstream.game_infoset.legal_actions
            if not legal:
                raise AssertionError("original DouZero has no legal actions")
            original_set = {tuple(sorted(action)) for action in legal}
            generated = {
                tuple(sorted(action))
                for action in MovesGener(
                    upstream.info_sets[position].player_hand_cards).gen_moves()
            }
            generated.add(())  # PASS only if the actor is responding
            if not original_set <= generated:
                raise AssertionError("original legal move not in original generator")
            for action in generated:
                physical = mask_from_own_hand(native.hands[seat], action)
                expected = action in original_set
                if not action:
                    observed = native.last_seat != 3
                else:
                    value = ct.c_bool(False)
                    status = lib.mygames_douzero_beats_move(
                        physical, native.last_cards, ct.byref(value))
                    if status != C11_OK:
                        raise AssertionError(
                            f"C11 offline classifier rejected original action: {status}")
                    observed = value.value
                if observed != expected:
                    raise AssertionError(
                        "original GameEnv and C11 offline response disagree")
                counts["candidate_comparisons"] += 1

            # Verify owner turn and stale-result fences leave both the
            # authoritative Round and the out-result EXACTLY untouched.
            if counts["actions"] % 13 == 0:
                copied = Round.from_buffer_copy(bytes(native))
                before = bytes(copied)
                event = Result()
                ct.memset(ct.byref(event), 0x5A, ct.sizeof(event))
                untouched = bytes(event)
                assert lib.mygames_ddz_round_submit(
                    ct.byref(copied), seat, copied.turn_id + 1, 0,
                    ct.byref(event)) == C11_STALE_TURN
                assert bytes(copied) == before and bytes(event) == untouched
                assert lib.mygames_ddz_round_submit(
                    ct.byref(copied), (seat + 1) % 3, copied.turn_id, 0,
                    ct.byref(event)) == C11_NOT_YOUR_TURN
                assert bytes(copied) == before and bytes(event) == untouched
                counts["negative_fencing_checks"] += 2

            # No false claim of full compatibility: production C11 Round
            # CANNOT submit certain original DouZero profile-only moves.
            common = []
            for action in legal:
                physical = mask_from_own_hand(native.hands[seat], action)
                if physical:
                    classified = Move()
                    if lib.mygames_ddz_classify(
                            physical, ct.byref(classified)) != C11_OK:
                        counts["offline_only_actions"] += 1
                        continue
                trial = Round.from_buffer_copy(bytes(native))
                result = Result()
                if lib.mygames_ddz_round_submit(
                        ct.byref(trial), seat, native.turn_id,
                        physical, ct.byref(result)) == C11_OK:
                    common.append((action, physical))
            if not common:
                raise AssertionError("no common original/native online action")
            passed = [a for a in common if not a[0]]
            plays = [a for a in common if a[0]]
            if native.last_cards and counts["actions"] % 5 in (1, 2) and passed:
                action, physical = passed[0]
            elif plays:
                plays.sort(key=lambda item: (len(item[0]), tuple(item[0])))
                action, physical = plays[
                    (game_index + counts["actions"]) % min(3, len(plays))]
            else:
                action, physical = passed[0]

            actors[position].action = list(action)
            result = Result()
            turn = native.turn_id
            assert lib.mygames_ddz_round_submit(
                ct.byref(native), seat, turn, physical,
                ct.byref(result)) == C11_OK
            assert lib.mygames_ddz_round_validate(ct.byref(native)) == C11_OK
            upstream.step()

            counts["actions"] += 1
            counts["plays" if action else "passes"] += 1
            if result.trick_cleared:
                counts["cleared_tricks"] += 1
                assert native.last_cards == 0 and native.last_seat == 3
            elif action:
                assert native.last_cards == physical and native.last_seat == seat
            assert native.turn_id == turn + 1
            assert upstream.card_play_action_seq[-1] == list(action)
            assert bool(native.finished) == bool(upstream.game_over)
            if native.finished:
                assert upstream.get_winner() == (
                    "landlord" if native.winner_seat == 0 else "farmer")
                counts["completed_games"] += 1
            else:
                assert upstream.acting_player_position == SEATS[native.turn_seat]

    if (counts["completed_games"] != number_of_games or
            counts["passes"] < number_of_games or
            counts["cleared_tricks"] < number_of_games or
            not counts["candidate_comparisons"]):
        raise AssertionError("incomplete full-round/pass/trick coverage")
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--games", type=int, default=24)
    parser.add_argument("--seed", type=int, default=91024)
    args = parser.parse_args()
    result = audit(args.library, args.games, args.seed)
    print("mygames.douzero_complete_common_round_parity: PASS "
          + " ".join(f"{key}={result[key]}" for key in sorted(result)))
    print("full_original_rule_env_qualified=false "
          "original_upstream_unchanged=true online_house_rules_unchanged=true")


if __name__ == "__main__":
    main()
