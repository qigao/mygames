#!/usr/bin/env python3
"""Test-only actor-private+public reconstruction of ORIGINAL DouZero features.

This is a NUMPY-ENABLED OFFLINE acceptance, not CI's stdlib C11 test.
It calls pinned original DouZero GameEnv.get_legal_card_play_actions and
get_obs; NO separately invented feature encoder/learner or rule engine.
The builder receives ONLY acting seat's own hand and actual native public
view/committed public history. It never receives other players' individual
hands, hidden deck, complete C11 Round or an omniscient InfoSet.

Upstream get_obs may internally consume the combined unknown-card multiset:
derive it using public full-deck multiplicities minus OWN hand and publicly
played moves, never from either opponent's secret partition.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import importlib
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))

from test_douzero_round_state_parity import audit, card_rank, SEATS

# Optional, explicitly operator-installed NumPy. Standard M5a CI never
# installs NumPy or Torch, so this separate REAL feature test is offline.
import numpy as np

original_game = importlib.import_module("douzero.env.game")
original_obs = importlib.import_module("douzero.env.env").get_obs
FULL_CARD_COUNTS = Counter(card_rank(i) for i in range(54))
ROLE_DIMENSIONS = {"landlord": 319, "landlord_up": 430,
                   "landlord_down": 430}


def actor_only_infoset(seat, own_hand_mask, public_view, public_actions):
    """Only seat/own hand/public-state and committed PUBLIC action sequence."""
    if seat not in range(3) or public_view.turn_seat != seat:
        raise ValueError("acting seat/public view mismatch")
    name = SEATS[seat]
    own = sorted(card_rank(i) for i in range(54)
                 if (own_hand_mask >> i) & 1)
    if len(own) != public_view.remaining[seat]:
        raise ValueError("private acting hand/public remaining count mismatch")
    chronological = [list(cards) for _, cards in public_actions]
    last_by_seat = {role: [] for role in SEATS}
    played_by_seat = {role: [] for role in SEATS}
    played_total = []
    for acting_seat, cards in public_actions:
        if acting_seat not in range(3):
            raise ValueError("invalid public committed action seat")
        cards = list(cards)
        last_by_seat[SEATS[acting_seat]] = cards
        played_by_seat[SEATS[acting_seat]].extend(cards)
        played_total.extend(cards)

    # Reuse exact ORIGINAL candidate-generation method, not a new
    # move generator. Its required inputs are only the actor's owned hand
    # and the publicly committed chronological action sequence.
    scratch = original_game.GameEnv({})
    scratch.acting_player_position = name
    scratch.info_sets[name].player_hand_cards = own[:]
    scratch.card_play_action_seq = [list(cards) for cards in chronological]
    legal = scratch.get_legal_card_play_actions()

    # Every ordinary rank multiplicity in a 54-card deck is public.
    # Opponents' combined unseen rank multiset is derivable with NO access
    # to either opponent's individual hand/kitty partition.
    combined_unknown = FULL_CARD_COUNTS - Counter(own) - Counter(played_total)
    if sum(combined_unknown.values()) != sum(
            public_view.remaining[z] for z in range(3) if z != seat):
        raise ValueError("public history cannot reconstruct remaining unseen cards")

    info = original_game.InfoSet(name)
    info.player_hand_cards = own
    info.other_hand_cards = sorted(combined_unknown.elements())
    info.last_move = sorted(card_rank(i) for i in range(54)
                            if (public_view.last_cards >> i) & 1)
    info.last_move_dict = last_by_seat
    info.played_cards = played_by_seat
    info.num_cards_left_dict = {SEATS[s]: int(public_view.remaining[s])
                                for s in range(3)}
    info.card_play_action_seq = chronological
    info.legal_actions = legal
    info.bomb_num = sum(1 for cards in chronological
                        if cards in original_game.bombs)
    info.three_landlord_cards = None
    info.all_handcards = None  # never supply other seat private partitions
    info.last_pid = (SEATS[public_view.last_seat]
                     if public_view.last_seat < 3 else None)
    return info


def run(library, games, seed):
    counters = defaultdict(int)

    def compare(seat, own_hand, public_view, public_actions, expected):
        candidate = actor_only_infoset(
            seat, own_hand, public_view, public_actions)
        # Use original upstream code for BOTH sides; the only difference
        # is whether the input Infoset came from the omniscient original
        # GameEnv or restricted native actor/private+public event sources.
        if candidate.legal_actions != expected.legal_actions:
            raise AssertionError("original action ordering differs for actor-only view")
        if candidate.all_handcards is not None:
            raise AssertionError("actor projection leaked opponent hand partition")
        if Counter(candidate.other_hand_cards) != Counter(expected.other_hand_cards):
            raise AssertionError("publicly derivable unseen-card multiset differs")
        actual_obs = original_obs(candidate)
        expected_obs = original_obs(expected)
        for field in ("x_no_action", "z", "x_batch", "z_batch"):
            if not np.array_equal(actual_obs[field], expected_obs[field]):
                raise AssertionError(f"actor-only original DouZero {field} mismatch")
        n = len(candidate.legal_actions)
        dimension = ROLE_DIMENSIONS[SEATS[seat]]
        assert actual_obs["x_no_action"].shape == (dimension,)
        assert actual_obs["z"].shape == (5, 162)
        assert actual_obs["x_batch"].shape == (n, dimension + 54)
        assert actual_obs["z_batch"].shape == (n, 5, 162)
        counters[SEATS[seat]] += 1
        counters["all_obs"] += 1

    game_counts = audit(
        library, games, seed, actor_feature_check=compare)
    if game_counts["exact_actor_observation_checks"] != game_counts["actions"]:
        raise AssertionError("not every full-game state yielded exact actor features")
    if counters["all_obs"] != game_counts["actions"] or any(
            counters[role] == 0 for role in SEATS):
        raise AssertionError("missing any complete native acting role")
    return {
        "original_upstream_commit":
            "718a5c920bf3361e34178a38f3b80458e176b351",
        "complete_original_native_games": game_counts["completed_games"],
        "actor_observations_all_original_tensors_exact": counters["all_obs"],
        "landlord": counters["landlord"],
        "landlord_down": counters["landlord_down"],
        "landlord_up": counters["landlord_up"],
        "full_original_game_rules_qualified": False,
        "production_native_actor_bridge_enabled": False,
        "original_actor_learner_modified": False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--games", type=int, default=24)
    parser.add_argument("--seed", type=int, default=91024)
    args = parser.parse_args()
    result = run(args.library, args.games, args.seed)
    print("mygames.douzero_actor_only_original_features: PASS")
    print(result)


if __name__ == "__main__":
    main()
