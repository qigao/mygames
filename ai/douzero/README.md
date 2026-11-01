# DouZero upstream R0 — restart without the old custom RL algorithms

**Decision:** Replace the experimental locally invented MyGames DMC algorithm and its `shed-most`/`legal-index` training-distribution design with the real upstream [kwai/DouZero](https://github.com/kwai/DouZero).

## Source and ownership

- Pinned upstream commit: `718a5c920bf3361e34178a38f3b80458e176b351` (2024-06-26).
- Original files: `third_party/DouZero/`; 32 upstream blobs kept **byte-for-byte unchanged** (including Apache-2.0 LICENSE and original documentation/image). Upstream CI workflow alone is omitted because MyGames already has its own CI and should not add an unrelated workflow.
- Source identity: `ai/douzero/upstream.lock.json`, checked by `tests/test_douzero_upstream_source.py` against original Git blob SHA-1 for **every** file. Any upstream update must be an explicit, reviewed revision and new lock.
- Existing MyGames source license is AGPL-3.0; preserved upstream copyright and Apache-2.0 terms apply to this vendored original source. No modifications to original copyright notices.
- Native C11 `games/doudizhu/{src,include}`, secure Match, Round, Room Owner/settlement and other game/business code are retained from M5j. The original DouZero Python `douzero/env` is an **offline reference trainer environment**, not a new production rule authority.

## Ground truth training sequence

1. **Upstream first, unchanged.** From `third_party/DouZero`, install its actual requirements only in a user-selected Python environment. Use official `python3 train.py --actor_device_cpu --training_device cpu` for an initial unmodified CPU baseline (or upstream GPU arguments); `python3 evaluate.py` for upstream evaluation. Original `douzero/dmc/utils.py:act` performs continuous actor self-play, `dmc.py:learn` runs the learner, and updated parameters are synchronized to actors. `douzero/dmc/models.py` defines official three-role models. **No** replacement sampler, fixed offline heuristic data mix, hand-written DMC trainer or unqualified MyGames model weights.
2. **C11 rules compatibility gate.** Before routing the original actor loop to `MyGames::DouDizhuMatch`, compare exact rule variants, landlord bidding, legal-action ordering, PASS/trick clearing, bombs/rocket/spring and terminal objectives. Validate the DouZero observation/action encoding (`319/430`, `54`, `5×162`) and reward objective: upstream defaults to **ADP**, with optional **WP**; MyGames game-points are not automatically ADP-compatible.
3. **Narrow environment adapter, not a new algorithm.** Implement only an actor-owned C11-to-DouZero `Env` boundary after parity. Keep upstream Actor, model, buffers, optimizer, exploration `exp_epsilon`, process synchronization and checkpoint semantics, and test real actor/learner run end-to-end. Do not expose other players' hidden hands/deck to online model inputs.
4. **Evaluate model skill independently.** Upstream evaluation first; later same-deal C11 tests vs established baselines under matching rules, without reusing observed data as virgin holdout. KFCore/ONNX inference and secure Room Owner application are separate product acceptance gates.

## Run the untouched upstream baseline

```sh
cd third_party/DouZero
python3 -m pip install -r requirements.txt  # explicit operator action, NEVER CI auto-download
python3 train.py --actor_device_cpu --training_device cpu \
  --num_actors 1 --num_threads 1 \
  --total_frames 2000 --unroll_length 20 --batch_size 2 \
  --num_buffers 4 --save_interval 1 \
  --savedir /PRIVATE_OUTSIDE_GIT/douzero-r0-checkpoints
```

Create an owner-private directory at `/PRIVATE_OUTSIDE_GIT/douzero-r0-checkpoints` first and replace that example path with your chosen storage. The command is an explicit optional **small upstream runtime experiment**, not a verified result or competitive training prescription. Real training and evaluation need a configured PyTorch/RLCard environment and sufficient resources. The default official objective is `adp` unless explicitly changed. Save generated checkpoints outside the repository when running on a trusted training host.

The old v102/v103/v104 weights are **retired experimental artifacts, not training seeds or new DouZero checkpoints**; do not relabel/reuse them as upstream DouZero. No AI production deployment is authorized by this R0 source import.

## Retired implementation

The old Draft PR chain #22, #24–#27, #29–#30 implemented a separate custom DMC trainer, custom Replay and heuristic-data collection. None of that unmerged code is carried into this new branch. Preserve historical Git records for reproducibility, mark old PRs/issues obsolete, and make this pinned upstream algorithm the only active RL implementation in the new development line.


## Executed ORIGINAL CPU Actor-Learner and evaluation (R0)

**This is a real upstream training smoke, not a new algorithm.** Original
kwai/DouZero at 718a5c920bf3361e34178a38f3b80458e176b351 was executed
from the verified local MyGames exact source checkout
0f9ad67d703c1a86f6cf93d82051e566fbd040c3. The entire vendored
upstream source was unchanged (32/32 original Git blobs). Runtime:
Python 3.13; PyTorch 2.10.0 CPU; NumPy; pinned public metadata-only wheels
GitPython 3.1.43, gitdb 4.0.11 and smmap 5.0.1. Original training and
random baseline evaluation did not require RLCard. No PyTorch installation
or private model/replay upload was added to GitHub CI.

An unchanged upstream train.py run with the upstream default **ADP**
objective, one CPU Actor, one learner thread, unroll_length 10, batch_size
2, num_buffers 4 and requested total_frames 160 completed **200 frames**
(the asynchronous learner can overshoot a finite limit): actual frame
counts landlord/up/down **60/80/60**. The exact three RMSprop optimizer
steps were **3/4/3**; all last sampled MSE losses were finite:
2.72384 / 1.69367 / 2.74503. The original private full model.tar
checkpoint SHA256 was:

    c79f7cc05cc4c98506870e1e831171291000f4535f6e5b8d9a1bfa201b730ec3

The source-trained checkpoint was actually loaded and contained the original
three role state_dicts, RMSprop states, position_frames, frames and stats,
plus three separate role weight images. For this small sample the original
mean_episode_return logging field was **NaN** because sampled mini-batches
did not contain terminal done rows. It is not valid evidence of convergence.
A finite external CPU supervisor stopped the original still-running Actor
process group **after** the checkpoint was committed, without patching
upstream code or discarding weights.

Using the original generate_eval_data.py and evaluate.py on a local
48-deal **developer-only** schedule, trained landlord vs two random farmers
won **1/48** complete games, and two trained farmers vs random landlord
won **35/48** complete games. This verifies saved model loading and real
actions; it is **not** a strong trained policy, C11 integration,
generalization claim, or new strategy-skill qualification.

## R1 original DouZero vs authoritative MyGames C11 rule parity: RED

A separate genuine source comparison used ORIGINAL
douzero.env.move_generator.MovesGener and get_move_type against
the EXISTING authoritative MyGames M5j ddz_round.c compiled as a
Linux shared library. On 100 deterministic parity-only random landlord
hands (seed 20261102), **10,075 upstream legal candidate actions** were
generated: **10,074** classified with agreeing types; **1** was accepted
as airplane-with-single-wings by official DouZero but REJECTED by C11.

Exact counterexample, in upstream rank notation:

    [7,7,7, 8,8,8, 9,9,9, 17,17,17]

Official DouZero recognizes three consecutive triple groups plus a
fourth triple used as three single wings (TYPE_11_SERIAL_3_1).
The C11 v1 house-rule classifier prohibits an extra triple wing and
rejects it. A classifier-only example is [3,3,3,3,4,4,4,4]:
upstream move_detector recognizes TYPE_14_4_22 while C11 rejects two
quads as four-plus-two-pairs; this example is NOT claimed to be
generated in the sampled upstream move list.

Developer reproduction (builds the ONE original C11 Round, no extra
engine/installer/consumer):

    cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -fPIC -shared -I games/doudizhu/include games/doudizhu/src/ddz_round.c -o /PRIVATE_OUTSIDE_GIT/libmygames-ddz-round-audit.so
    python3 ai/douzero/audit_c11_rule_parity.py --c11-library /PRIVATE_OUTSIDE_GIT/libmygames-ddz-round-audit.so --hands 100 --seed 20261102 --report-only

Without the explicit **report-only** diagnostic switch the parity audit
must fail closed until every move agrees. It never changes upstream
source, performs training, or qualifies a native inference adapter.

Two other material differences prevent a drop-in C11 Env:

- Original DouZero Env.reset directly sets **20 landlord / 17 / 17**
  dealt cards and a fixed landlord *position*. MyGames C11 Match has an
  authoritative, separately staged **0..3 bidding** phase and a physical
  landlord-seat selection/kitty procedure.
- Original DouZero **ADP** terminal objective is signed 2^bomb_count.
  MyGames C11 M5j terminal scoring uses bid_base multiplied by
  2^(bomb_or_rocket_count+spring), including spring or counter-spring
  and explicit per-seat point deltas. Even DouZero WP (+1/-1) is not a
  complete replacement for the existing C11 settlement ledger.

**Decision: R1 C11 integration remains BLOCKED.** Choose, version and
test an explicit DouZero-compatible offline C11 game-rule profile
(including exact move legality, observations, reward contract and
original 319/430+54, 5x162 feature layout), without changing existing
M5j online game/settlement behavior. Then connect only a narrow C11
Env bridge to the UNMODIFIED upstream actor and learner. No new DMC
algorithm, heuristic source sampler, silent rule conversion or
unverified pretrained checkpoint is allowed.


## R0 extended original WP CPU training: complete finite reward statistics

With the SAME untouched upstream actor/learner and privately installed
GitPython-only metadata wheels, a second official experiment used
the EXISTING upstream **--objective wp** flag (rather than the default
ADP), CPU actor=1, learner threads=1, unroll_length=20, batch_size=2,
num_buffers=4, requested total_frames=1200, RMSprop defaults.
The asynchronous learner actually finished **1,280 frames**:
landlord/up/down **440/400/440 frames**, optimizer steps **11/10/11**.
Unlike the smaller ADP smoke, all last checkpoint loss and
mean_episode_return fields were finite:

| Upstream role | Last sampled loss | Last logged mean episode return |
| --- | ---: | ---: |
| landlord | 0.292679 | -0.909091 |
| landlord_up | 0.355744 | +0.933333 |
| landlord_down | 0.254079 | +0.939394 |

Re-loaded original full checkpoint SHA256:

    5fc21c8667be575dc8a1e7c1b10df55df136877eef36cc8c427c277d0868ead4

Original evaluate.py on the SAME 48 already-observed developer-only
deals found trained WP landlord **0/48** wins versus two random
farmers and trained WP farmers **37/48** wins versus random landlord.
The original checkpoint and three role-specific state dicts were kept
PRIVATE outside the Git worktree and not released. This is STILL
a bounded runtime smoke, not gameplay-skill acceptance or complete
training. Its return statistics describe the training actor's games,
not any frozen independent skill qualification.

## Real original WP vs C11 signed settlement comparison

A third independent script calls the ORIGINAL DouZero
Env._get_reward and the EXISTING C11
mygames_ddz_score_compute (not a replicated scoring implementation)
over the complete bounded matrix: bid base 1..3, all landlord
and winning seats, bomb counts 0..2, landlord nonpass 1..3 and
farmers nonpass 0..3.

Out of **801 native-accepted cases**, original **WP win/loss sign
agreed in all 801** with the C11 landlord team win sign. The full
ADP numeric reward magnitude differed from C11 settlement's
nonmonetary unit in **612/801** cases. A concrete example is
base bid 1, bombs 0 and landlord spring: MyGames unit 2 but
original DouZero ADP magnitude 1.

Reproduce against the exact existing C11 Match/Room/Round (offline
developer test; never a second production rules runtime):

    cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -fPIC -shared -I games/doudizhu/include games/doudizhu/src/ddz_match.c games/doudizhu/src/ddz_round.c games/doudizhu/src/room.c -o /PRIVATE_OUTSIDE_GIT/libmygames-ddz-score-audit.so
    python3 ai/douzero/audit_c11_reward_parity.py --c11-match-library /PRIVATE_OUTSIDE_GIT/libmygames-ddz-score-audit.so --objective wp

The same audit with **--objective adp** must FAIL CLOSED on the
existing rules; --report-only emits a diagnostic, never a valid ADP
parity gate. WP parity proves a **team outcome training target only**.
The existing owner-controlled MyGames bidding, move legality,
bomb/spring nonmonetary ledger and Game ACK MUST remain authoritative.
The R1 C11 adapter remains RED until every legal move and
observation/seat encoding also agrees.
