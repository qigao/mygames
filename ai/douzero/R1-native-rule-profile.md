# DouZero R1 — native C11 move-classification compatibility profile

This is **not** a second DouDizhu game engine, a new DMC algorithm, or a
production Room Owner rule change. It closes only two observed differences
between original kwai/DouZero at commit
718a5c920bf3361e34178a38f3b80458e176b351 and the current authoritative
M5j C11 classifier, without modifying original upstream or online game rules.

## Scope

The BUILD_TESTING-only mygames_douzero_offline_rules shared target compiles
the same authoritative games/doudizhu/src/ddz_round.c plus the small offline
adapter ai/douzero/src/douzero_rule_profile.c. Ordinary moves ALWAYS use
mygames_ddz_classify. Two explicit original upstream move_detector
exceptions are accepted only by mygames_douzero_classify_move:

1. **Airplane with a fourth triple used as three single wings:** three
   consecutive triple ranks + one separate triple => 12 cards recognized
   by upstream as TYPE_11_SERIAL_3_1; the existing online C11 v1 house
   rules reject the extra triple. A full uninterrupted four-triple
   sequence remains a native TRIPLE_CHAIN.
2. **Two distinct quads:** original move_detector classifies eight cards
   as TYPE_14_4_22 and picks the higher quad as the primary rank,
   while existing online C11 rejects this case. This is a detector
   compatibility exception; the original ordinary generator does not
   necessarily enumerate two quads as a candidate.

The profile does NOT override mygames_ddz_round_submit, online Room Owner
validation, or existing M5j public/secure settlement APIs. Invalid masks
cannot mutate the caller output; it does not silently admit another bomb
wing, invalid straight or unrelated action.

## Original upstream vs real C11 test

The existing M5a native CTest checks both exceptions and unchanged online
classification across Linux/Windows/macOS, including Linux ASan/UBSan.
The Python-only stdlib test imports the actual unchanged pinned original
DouZero MovesGener and get_move_type, and compares every generated action's
type, primary/high rank, sequence length and card count with the C11
shared library. No PyTorch, RLCard, GPU or replacement model is installed.

A local Clang/C11 test ran deterministic seed 20261102 against **1,000
20-card original DouZero hands**, evaluating **99,755 candidate actions**.
The original online C11 classifier differed on **2**, while the strictly
offline profile matched **99,755 / 99,755** in type/main rank/length/count.
The smaller 100-hand suite had **10,075 original candidates**, one online
house-rule mismatch, and zero offline-profile mismatches.

Re-run using the already-built Linux M5a test target:

    python3 tests/test_douzero_rule_profile.py --offline-library build/ddz/libmygames_douzero_offline_rules.so --hands 1000

These deterministic tests are developer action-classification audits, not
production dealing entropy, original DouZero training data, or game skill.

## Original response-move selection, not just isolated action types

The original upstream move_selector.filter_type_11_serial_3_1 has a
specific response ordering: it compares the highest rank of **all triple
groups**, even when the extra triple is an airplane wing. The original
C11 mygames_ddz_beats instead compares the airplane body high rank.

A physically possible two-player counterexample (rank notation):
- Rival has 3+3+3 cards of ranks 3/4/5 and three cards of rank 2 as wings.
- Challenger has triple ranks 6/7/8 and three Q cards as wings.

Original DouZero GameEnv.get_legal_card_play_actions REJECTS the
challenger: the wing triple rank 2 in the previous move is higher than
the challenger wing rank Q. Online native C11 compares body 8 > 5 and
ACCEPTS. The offline-only mygames_douzero_beats_move now preserves the
original move_selector behavior without modifying mygames_ddz_beats.

The positive no-NumPy/no-Torch audit calls the **actual unchanged**
original GameEnv.get_legal_card_play_actions against 100 games of
physically disjoint 17-/20-card hands from a single 54-card deck.
Three rival moves per pair give **300 real original rival move sets
and 30,669 native candidate response comparisons**, all matching
the original GameEnv. This is pairwise response legality only; it does
not apply or commit any C11 Round action.

## R1 remains RED — not yet a complete authoritative C11 Env

This validates generated action classifications and pairwise response
eligibility against the original GameEnv only. It does NOT yet prove
whole-game PASS/turn transitions, bidding, 20/17/17 dealing, full original
infoset legal-list order/duplicates, serialized 319/430+54 and 5x162
actor features, or rewards through actual native C11 Match settlement.

Original DouZero Env starts with fixed landlord and no bidding.
MyGames C11 Match executes its own 0..3 staged bidding. Separate real
reward audit confirmed exact original WP win/loss sign matched 801/801
native settlement cases, while original ADP magnitude disagreed in
612/801 due to bid and spring multipliers.

No C11-powered DouZero Actor training environment or model promotion is
enabled yet. The next step is strict native Round turn/PASS progression
and full actor-only feature parity, followed
by the thinnest possible authoritative C11 Env adapter with the ORIGINAL,
unmodified upstream actor/learner. Never silently change online M5j rules
or reintroduce retired custom DMC/checkpoints as upstream weights.
