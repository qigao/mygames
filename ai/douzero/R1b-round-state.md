# DouZero R1b — real original-versus-C11 full-round state parity

This staged R1b experiment follows the pinned original
`kwai/DouZero@718a5c920bf3361e34178a38f3b80458e176b351`
and the native online MyGames C11 M5j `ddz_round.c`. It does **not**
replace either implementation, rewrite Actor–Learner or implement a second
rules engine. This comparison is a developer-only Python/ctypes test, not
a production `Game Owner` or AI inference adapter.

## What is actually compared

`tests/test_douzero_round_state_parity.py` creates one real shuffled
54-card physical deck per game and constructs the *same* three players'
hands in the original `GameEnv` and the original native
`mygames_ddz_round_deal` interface:

- Native physical seat `0=landlord`, `1=landlord_down`,
  `2=landlord_up`; original upstream `GameEnv` rotates positions
  `landlord → landlord_down → landlord_up`. Fixed landlord position
  in this comparison does **not** claim parity with native real bidding.
- Native C11 deals 17 cards cyclically to each seat plus three kitty to
  landlord; the upstream environment is explicitly initialized with
  the exact matching rank counts from those same physical cards.
  No opponent's hand is passed to a model or used to choose an action.
- At **every actual game turn**, query the original unchanged `MovesGener`
  for all candidate rank-actions and original unchanged `GameEnv` for
  which candidates are legal. Check the exact candidate-membership
  predicate against the isolated C11 `mygames_douzero_beats_move` and
  PASS eligibility from the **current native Round's own state**.
- A deterministic **test-only** actor chooses from the **intersection**
  of the actual original GameEnv legal actions and the original
  production `mygames_ddz_round_submit` accepted actions. It never
  submits a profile-only extra-triple-wing or double-quad action to the
  unchanged production Round, which still rejects those cases.
- Both original game engines perform the real accepted move and their
  resulting owned hands, public remaining counts, acting seat, turn
  identity, last legal action, trick-clearing after two PASS moves,
  terminal/winner semantics and C11 state validation are checked
  **after every action**.
- C11 Round's stale turn ID and wrong-physical-seat error paths are
  exercised on a copied real native game state and checked for exact
  byte-for-byte no-change to both Round and caller result buffers.
  No test-only entropy hook is added to the C11 library.

## Verified local positive result

The original upstream Python implementation and original native C11
`ddz_round.c` + test-only offline response-profile library completed
**24 independent, fully terminal original/C11 matched games** with
deterministic audit seed `91024`.

| Actual counter | Result |
| --- | ---: |
| Full completed real original/C11 games | **24** |
| Original generated candidate legal-membership comparisons | **38,842** |
| Committed original/native actions | **1,919** |
| Actual non-PASS plays | **1,090** |
| Actual PASS transitions | **829** |
| Two-PASS trick clears | **399** |
| Stale turn/wrong seat negative checks | **296** |

Every step matched and neither original game engine was modified.
The identical test is included in the EXISTING M5a Linux source CI;
Windows/macOS and Linux ASan/UBSan continue validating their original
native CTest targets.

## Strict limitations; R1 native Actor–Learner integration remains RED

- Original DouZero and existing C11 online house rules do **NOT** accept
  exactly the same complete action set. The earlier offline classifier
  and pairwise response profile handle known incompatible moves, but the
  original online `round_submit` intentionally rejects them. This
  run only proves whole-game semantics **on the common-action subset**.
  It does **not** validate complete original DouZero gameplay through an
  authoritative native C11 Round with every original action available.
- The source uses fixed landlord and no bid. Original DouZero has no
  staged landlord bidding; MyGames `Match` has genuine 0..3 bidding and
  private kitty selection. Bidding/landlord rotation remain separate
  RED gates.
- Original `GameEnv.get_infoset` can hold combined hidden opponent cards
  internally, while a production actor-only C11 adapter may only expose
  the acting player's private hand and authorized public history. Exact
  original `get_obs` feature parity (319/430+54 and 5×162 history),
  public and secret data non-disclosure and full long-history turn
  persistence are **NOT** proven in this test.
- Terminal team WP sign was shown consistent with 801 valid C11 native
  score combinations, but original ADP vs native bid/spring scoring
  differs; no equivalence of the whole business ledger is implied.
- No Torch/KFCore model is created, trained, restored, scored or promoted
  by this test. The original upstream Actor and Learner stay untouched.

**Next R1c:** implement the owner-only actor observation compatibility
contract by producing the exact original DouZero `get_obs` features
from verified native C11 actor-hand/public-move history; then explicitly
resolve original-only legal actions and the fixed-landlord vs native
bidding semantics before enabling the C11 Env adapter. Do not add a
replacement DMC algorithm or silently change existing online gameplay.
