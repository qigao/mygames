# DouZero R1c — exact actor-only original feature reconstruction

This is a **real, offline NumPy-enabled integration test** built on
[original pinned DouZero R0](../douzero/README.md) and R1b's verified
authoritative native C11 whole-round exercise. It does not implement a
replacement neural network, learned policy, hidden-information engine,
production CFlow Owner or a second DouDizhu game. The upstream original
`kwai/DouZero@718a5c920bf3361e34178a38f3b80458e176b351` model,
`get_obs` feature encoder, move generator, Actor and Learner remain
byte-for-byte unchanged.

## Restricted actor inputs, explicit privacy contract

On every native C11 Round step, the **test-only** origin is strictly:

- **One actor's authorized private hand**, picked by the current seat;
  the actor-only builder has NO reference to either other seat's private
  hand partition or any hidden deal/deck buffer.
- **Actual native C11 public view**: turn, acting physical seat, last
  committed move, public per-seat remaining counts and terminal state;
  no private hands, deck, or session keys in the projection.
- **Committed, ordered public move stream**, including which physical
  seat played or PASSed. This test stream is only in memory and does
  not pretend to be the actual production durable game-event journal.
- **Public constant card multiplicities**: full 54-card rank counts
  minus the actor's current private hand and the *publicly played* rank
  counts yield the combined unknown cards in opponents' hands, which
  the pinned upstream `get_obs` requires. This does NOT reveal the
  partition between the two opponents.
- The initial landlord and seat-to-role mapping are fixed to the
  original DouZero offline convention; this does NOT resolve the
  real M5j native staged bid and physical landlord selection.

The trusted test constructs a minimal original `InfoSet` by
replaying the native owned-hand plus public stream, uses the actual
**original GameEnv.get_legal_card_play_actions** and then invokes
the **original unmodified `get_obs`**. It explicitly leaves
`all_handcards=None` (no omniscient per-seat private hand copies).
The old algorithm is not reintroduced and no alternative feature
encoder is written.

## Positive executable evidence

An independent original-DouZero/real-native C11 local CPU test with
configured NumPy 2.3.5 (not installed/downloaded by standard CI)
executed **24 complete real matched original/native Round games**
with 1,919 successive current-actor observations. All input tensors
matched original pinned `get_obs` **element for element**, including:

| Actor role | Exact original observation matches |
| --- | ---: |
| Landlord | 648 |
| Farmer down | 642 |
| Farmer up | 629 |
| **Total** | **1,919** |

For each state, legal action **list and order**, `x_no_action`
(319 for landlord, 430 for farmers), `z` (5×162),
`x_batch` (N×373 or N×484), and `z_batch` (N×5×162)
match original upstream exactly. No model weights, checkpoint,
private user Replay or synthetic rule-approximation were involved.
The test uses **the identical common-action original/native game
states** from the R1b whole-round test and does NOT prove that original
upstream-only game actions can be committed by the online native Round.

Optional offline command, with NumPy already configured:

```sh
python3 tests/test_douzero_actor_feature_parity.py \
  --library build/ddz/libmygames_douzero_offline_rules.so \
  --games 24 --seed 91024
```

In GitHub's existing lightweight M5a native CI, the new test is
**syntax checked only**, because the image does not include NumPy
and the project deliberately forbids downloading Torch/NumPy as
part of ordinary C11 CI. The R1b 24 real full-round state tests
and upstream 32/32 provenance still execute in standard Linux CI.

## Remaining R1 acceptance gates

1. The native C11 House Rules deliberately reject original-only
   airplane-wing/quad moves. R1 offline classification/response tests
   are green; native production `round_submit` still refuses those
   combinations. No full-rule native Env has been enabled.
2. Original DouZero has a fixed landlord, not the C11 0..3 staged
   bidding protocol. Any mapping of original training to native
   real game requires an explicit versioned game-profile choice.
3. **Durable public history** and actor-seat/turn/generation-bound
   provenance on the real Game Owner must exist before an online model
   may be given these features. A local in-memory test event list is
   not such a production guarantee.
4. The original training `Actor–Learner` has not yet consumed an
   authoritative C11 Env end-to-end; model skill, frozen ONNX/KFCore
   Q parity, and release/deployment are separate RED gates.

The next implementation should be a minimal native public-event and
actor-private observation **adapter** consumed by the original
DouZero Env, not an additional DMC/replay/training algorithm.
