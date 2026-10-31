# MyGames Game AI — R0/R1 native contract (Draft)

Stacked on MyGames M5j Draft PR #20, exact base
5c81c2d500422a7f17b2b89d48f8bbf3f8f2d25b.
Tracking Issue #21. This is an algorithm foundation, not a trained AI,
deployed bot, release, or approval to merge any parent PR.

## One authoritative game engine

DouDizhu rules remain exclusively in ddz_round.c / ddz_match.c.
The CFlow Game Owner and existing TurboDB store commit the staged
private checkpoint, command receipt and sanitized public-only Outbox
atomically. A Game ACK follows only a positively confirmed commit.
An AI model proposal never directly changes a game or settles points.

## R0 — complete per-player reward targets

MyGames::RLTargets is a pure C11 module. A transition has episode ID,
strictly increasing local state revision, actor and next actor IDs,
player count (max 8), model snapshot version, logical time delta,
separate terminated/truncated flags and full per-player reward vector.
The evaluator's seat is not implicitly the current actor.

For a complete, untruncated episode the Monte Carlo target is:

    G[t,p] = reward[t,p] + discount^time_delta[t] * G[t+1,p]

Output is step-major: one return per player at every transition.
time_delta=0 supports within-turn micro-decisions that do not advance
the game clock. Incomplete/truncated episodes MUST NOT silently become
terminal Monte Carlo labels; a future bootstrapped TD method must
define that behavior independently. All errors leave output untouched.

Distinguish WP (team win indicator) from nonmonetary MyGames M5g
game-point target. M5g landlord win settles [+2U,-U,-U] in landlord
seat order, and farmer win reverses the team signs. MyGames' bid,
bomb, rocket and spring scoring is NOT numerically equivalent to the
original DouZero ADP objective. Model-version and reward-objective
identity belong to subsequent private training records.

The M5h mg_game_results table is the immutable business settlement
ledger, NOT a model Replay Buffer: it lacks step observations,
candidate actions and ordered public move history. Future AI
trajectories must have a separate private, revisioned store and
must never appear in public MQTT Outbox or Cocos client state.

## R1 first slice — canonical complete legal actions

MyGames::DouDizhuAI takes only the currently acting player's own
physical-card mask plus sanitized mygames_ddz_public_view. No
private match, other hand, hidden deck or bearer credential is
passed to the enumeration function or model worker.

The implementation visits all bounded per-rank count subsets (no
more than 2^20 combinations for a maximum 20-card hand), picks the
lowest owned physical IDs as suit-equivalent canonical actions,
and uses ONLY the existing mygames_ddz_classify and
mygames_ddz_beats functions for v1 pattern legality and rank
comparison. This includes v1 airplane wing policy, chain lengths,
bomb, rocket, and PASS only when following another player.
It never silently truncates the legal candidate list or guesses
game rules. Query mode returns an exact count; capacity failure
leaves the caller's action buffer unchanged. Generation is
allocation-free, bounded in stack depth, and deterministic.

These masks are PROPOSALS only. For live bots the trusted Room Owner
must revalidate seat/identity/generation, owner epoch, game ID, turn ID,
and actual proposed physical-card mask via the existing
mygames_ddz_game_owner_play / bid commit path.

## Planned separate gates — NOT implemented here

1. Owner-side actor-only observation encoder and DouZero rank-space
   54-bit features, current private hand, role, opponents' public
   remaining counts and 15 ordered public moves. Existing private
   match checkpoint alone does not provide full historical features.
2. Offline pure-C11 self-play loop with bounded owned Episode data;
   terminal reward vectors derived from actual MyGames M5g
   settlement, not current-actor reward mixing.
3. Immutable model inference via selected KFCore adapter, offline
   PyTorch DMC learner, versioned private AI Replay and model
   publication. Pretrained DouZero weights require tested matching
   feature encodings, reward objectives and game rule variants.
4. Paired-deal, seat-rotated evaluation vs legal random/heuristic
   baselines, with WP, mean nonmonetary points, confidence intervals,
   throughput and inference latency. A source CI pass is not proof
   of strategic skill.
5. Bounded CFlow AI tickets: one live proposal per eligible turn;
   cancellation, BUSY, stale model output rejection, no background
   ORM/network mutation, no silent model fallback.
6. Optional IS-MCTS/MuZero/LightZero providers after the DMC
   baseline, only with correct information-set and multi-player
   reward semantics, and demonstrated paired evaluation benefit.

## Core acceptance

Run two native CTests in the existing no-SDK CMake/CI graph:
mygames.rl_targets (multi-player rewards, discount/time delta,
termination/truncation, NaN and atomic capacity validation);
mygames.ddz_ai_actions (small-hand exhaustive physical oracle
modulo suit equivalence, response/pass, output-capacity atomicity,
and real 20-card dealt-round validation through round_submit).
Both also run under the existing M5a Linux ASan/UBSan lane.
There is no installer/consumer project, new workflow family,
probing CMake logic, second DDZ rule engine or SDK change.
Keep all parent PRs Draft. Real Cocos Editor/Edge GUI, IAM/WSS
and production promotion remain separate unqualified gates.


## R1 implementation update — actor-only DouZero features and offline samples

MyGames::DouDizhuAIFeatures now uses **only** the acting player's
already-authorized hand and the public history, which must be extended from
accepted moves AFTER real Game COMMIT in online service. The current source
does **not** yet persist full public action history through owner restart;
there is no implicit backfill from the private checkpoint. Online inference
must remain disabled until a trusted, authenticated, durable event history
projection exists. The offline collector does not need that server feature.

The history is append-only, turn-fenced, tracks full public per-player
played-card aggregates and latest actions per seat, plus the last 15
chronological public moves. It has 15x54 DouZero action-history encoding
(left padded) and 319 landlord / 430 farmer role-specific state features.
Opponent combined unseen cards are reconstructed using (full 54 cards -
OWN hand - ALL publicly played cards), **never read from the secret hands**.
A model cannot get other players' hand partition or secret shuffled deck.

This source implements original DouZero feature dimensions and rank-count
layout, but **pretrained DouZero checkpoint compatibility is NOT claimed**:
MyGames' house rules, bidding/scoring objective, public trick reset semantics
and any model weight format must pass independent golden-vector and
matched-rule numeric tests first. No silent weights fallback.

MyGames::DMCCollector receives an accepted rule result and owns a caller-
provided bounded fixed-shape in-memory sample array (max 162 out-card
decisions, not bidding). After an actual finished, authoritative M5g
settlement, it seals every sample once with either team-WP (+1/-1) or
the exact nonmonetary signed per-seat game points. No intermediate
actor-reward mixing; no labels from truncated/incomplete games.
The target is a completed-data training record, **not yet** a durable
Replay store, gradient learner or runtime AI opponent.

The R0 RL transition next_actor_id == player_count now explicitly
represents a terminal transition with no next acting player, instead of
inventing a next seat after FINISHED.

The new native tests include 54-card real rule operations,
noninterference under changed opponent hidden hand distribution,
15-event sliding history, farmer and landlord feature offsets, complete
actual 3-player C11 match through spring settlement, and DMC labels
cross-checked against full reward-vector Monte Carlo returns.
Existing CMake/CTest, M5a three-OS core and Linux ASan+UBSan lane
are extended without creating extra workflow families or consumer apps.

**Remaining R1 blockers:** persistent trusted committed public move log,
offline data writer/Reader with provenance and versioned feature ABI,
PyTorch DMC learner and trained weights with actual paired-deal evaluation,
and single-Owner fenced bot-seat proposal path. None is complete here.


## R1 durable PUBLIC action journal (new candidate)

The M5k candidate extends the EXISTING TurboDB GameStore transaction
with mg_game_public_moves, an append-only canonical public-only row
keyed by (room_id,game_id,turn_id) and unique committed Outbox sequence.
On every accepted game.play / game.pass, the existing Owner-backed
game_store_candidate calculates the newly REVEALED physical-card mask
from the prior and successor authoritative rounds. A PASS is encoded by
the zero mask. It inserts this row BEFORE the same atomic COMMIT that
publishes the private M5c checkpoint, original Game ACK receipt, public
game.state_committed Outbox and (on terminal turn) M5h credit.

This does NOT append the secret shuffled deck, hidden private hands,
undealt bottom cards, login credentials, RNG seed, model state or
uncommitted speculative actions to any public source. The old PUBLIC
Outbox schema is unchanged: no raw-match blob or second event producer.
A failed transaction leaves neither a new action history row nor a Game
ACK. Replayed command receipts produce no second row or score credit.

A trusted CFlow Owner may call
mygames_room_store_public_game_moves() to read up to 162 canonical
public actions for a specified room/game, including historical finished
games after a process restart and Owner epoch claim. One SQLite SELECT
snapshot is validated in full: contiguous turn IDs starting at 1,
strictly increasing committed sequence IDs, physical-card no-reuse,
seat turn order, valid v1 move patterns and comparisons, legal PASS
placement, and correct maximum per-seat card counts. All error paths
leave caller's output action buffer unchanged; capacity reports an
exact required row count. Empty history is not proof that the game ID
exists or the querying principal has permission.

The actual TurboDB M5h ledger CTest covers first/second full real
58-decision matches with different landlords, injected rollback
before the final Outbox, duplicate/contradictory request replay,
capacity-atomic read, and both games still readable after SQLite reopen
and Owner epoch changes.

**Still a boundary:** The read API is privileged infrastructure, not
a public WebSocket endpoint or an AI worker credential. The unique
Owner must map these rows to a validated actor-only observation and
rebuild the last 15 moves before safely enabling online DMC, after
checking seat authorization and current game/turn against the
authoritative persisted match. This commit does not activate live bots,
a native DMC gradient learner, model inference or private AI Replay
storage. The new SQLite table is a Draft schema addition; no automatic
migration/reconstruction of history from earlier unjournaled game rows
is attempted or implied.


## R1 restart-safe PUBLIC history reconstruction (candidate)

The pure C11 mygames_ddz_ai_history_reconstruct() consumes only the
trusted, committed public move tuple (turn, actor seat, cards mask)
and the landlord seat, and MUST be supplied the independently
authoritative expected current public round snapshot. It rebuilds
the 20/17/17 starting counts, current acting seat, comparison move,
two-PASS trick clearing, physical-card uniqueness, per-seat played
aggregates and chronological 15-move window, using the same v1
classifier and public-history validation path as ordinary offline
self-play. The output is published atomically only if it matches
the expected authoritative public view exactly.

The existing M5h real TurboDB two-game test now bridges the actual
SQLite committed history rows to this pure C11 projection, compares
against the real private match's PUBLIC view at the 57th decision,
after each finished game, and after process reopen. Neither the ORM
reader nor the AI observation receives the private hands.

Online inference is STILL DISABLED: a trusted Room Owner must explicitly
bind the current bot principal and gameplay revision, load history
through this read API, take the bot's own private hand via the existing
authenticated owner method, and recheck model proposal turn/epoch
before the normal Game COMMIT. This candidate implements the data
reconstruction only. No trained model, actual bot, auth or production
cutover is claimed.


## R1 Private Replay v1 + offline DMC learner (2026-10-31 candidate)

MyGames::DMCReplay is a pure C11 canonical, little-endian codec. Only
a complete mygames_ddz_dmc_episode_seal() can be serialized.
It rechecks the original actor/role mapping, source turn IDs, model/rules
version, native 54-card action-to-feature encoding, exact (319/430,
810,54) binary arrays, role-specific Win-Sign or Game-Points labels,
and the true terminal winner. A truncated episode cannot yield data.
The byte array is encoded by fields; NO raw memcpy(struct) disk image.
No C11 core filesystem I/O or hidden inference dependency is added.

Exact v1 format:

* Magic bytes: MGDMC001. 64-byte header includes explicit v1, sealed,
  episode ID, source model snapshot, rules version, reward objective,
  step count, landlord/winner, record/payload sizes and zero-reserved.
* Each play/pass sample is exactly 1,326 bytes in little-endian:
  source turn u64, chosen physical mask u64, actor u8, role u8,
  state-feature length u16, zero-reserved u32, signed IEEE754 f64 target,
  430 padded state int8s, 810 chronological history int8s and 54 action
  int8s. Landlord state is 319 entries + 111 ZERO bytes of padding.
* CRC32 IEEE trailer (4 bytes) detects accidental corruption, NOT
  forgery or authenticity. Only a trusted, private producer may publish
  a replay file. No private replay bytes, own hand, opponent union or
  raw feature data may enter logs, public MQTT, Cocos or CI artifacts.

The reader validates the WHOLE file before publishing decoded
samples, including CRC, canonical padding, turn/role consistency and
true player-specific reward labels. Query and small output buffer errors
do not modify caller outputs. Maximum supported episode is 162
accepted gameplay decisions; bidding and unrecoverably truncated games
are deliberately unsupported in v1.

The standard-library-only ai/dmc/replay.py independently decodes and
validates the C11 file layout and reward targets; the existing M5a
Linux workflow executes tests/test_dmc_replay_reader.py against a
REAL full C11 game fixture generated in memory by the existing
mygames_ddz_dmc_episode_test CTest. A temporary private file is
removed on completion. No standalone installer/consumer project or
new CI workflow is created.

ai/dmc/train_dmc.py --train is the actual offline PyTorch learner:
three role-specific LSTM (5x162 to 128) and 512-wide action-conditioned
MLP heads, per-role MSE on complete-episode Monte Carlo returns,
bounded minibatches, RMSprop and gradient norm clipping. The train
command REQUIRES configured PyTorch; it never silently switches
engines or invents model weights. --verify-only needs no PyTorch.
Checkpoints are newly written into a caller-controlled PRIVATE
directory with atomic replacement and explicit model/objective/source
metadata; they are NOT installed production inference weights.

This is a viable learning data and algorithm path, not an evaluation
of chess/real DouDizhu skill. The current test fixture contains one
scripted deterministic policy (20 landlord singles; 38 farmer PASS)
and cannot demonstrate learning quality. No bot is enabled, no
real-player training data is collected by default, and the M5h score
ledger remains the authoritative nonmonetary business record
rather than a training replay store. Separate acceptance still requires
many independent seeds and opponents, genuine bounded Self-Play
workers, paired-deal seat-rotated evaluation and a KFCore inference
binding with exact feature/model numerical agreement.


## R1 bounded native OFFLINE self-play (next slice)

MyGames::DMCSelfPlay exercises the actual C11 M5b/M5a secure game,
landlord bid3, legal action generator, exact actor-only observation,
rule-authorized turn transitions and terminal M5g settlement in-process.
There is NO network request, Room Owner state mutation, CHttp, Flowie,
SQLite Game ACK or new competing rules implementation. Card dealing
uses the actual OS CSPRNG only. The single first-player bid3 is an
EXPLICIT non-learning baseline: AI bidding remains a separate phase.

The policy callback sees ONLY the acting seat's own authorized hand
and public chronological history, plus the complete array of
legal physical-card masks. All callbacks must choose EXACTLY one
offered action. An invalid proposal, callback refusal, exhausted
bounded action list, insufficient episode sample capacity or a
truncated match produces an explicit non-success status and NO sealed
training labels. No silent first-legal, random model or other fallback.

Once finished, the sampler verifies the authoritative settlement,
seals the actor-correct WP or nonmonetary score labels and returns
a caller-owned episode for the private DMCReplay v1 codec.
A test-only reproducible xorshift selects among candidates using
its own local policy RNG; the ACTUAL game deck is still securely
randomized. That random baseline is not a trained game AI and
does not claim realistic bidding strategy.

The existing M5a native/ASan/UBSan CTest now exercises three
independent securely shuffled complete real games, verifying
reward vectors, Replay serialization, exact actor/role target and
failure semantics for an illegal policy and forced truncation.
The existing Linux CI additionally uses this self-play output as a
private ephemeral Replay and runs the actual Python --verify-only
trainer entry point. The fixture is securely temp-created and deleted,
without uploading bytes as a GitHub CI artifact.

Still missing: high-volume bounded parallel Actors/Replay persistence
with private ownership and metadata, proof of unbiased training-policy
sampling under many games, real PyTorch gradient runtime CI (PyTorch
is deliberately NOT installed as an unpinned CI dependency), and
paired-deal seat-rotated evaluation with trained weights. The
first code path is a functional native sampler, not trained skill.


## R1 real gradient learner, immutable model ABI and disjoint validation

Native Self-Play now emits TWO independent, securely shuffled, complete
WIN_SIGN Replay files from one run, with distinct episode IDs 5000 and
5001; a third independent test case uses real M5g GAME_POINTS. The
Linux M5a source gate continues to check REAL C11->Python Replay and
now invokes the learner --verify-only on the two WIN_SIGN files as
entirely separate training and validation episodes. It must reject
duplicate episode IDs across the two pools and any rules/objective
mismatch without importing PyTorch.

The actual offline learner --train requires an EXPLICIT
--validation-replay (not the same episode or a per-transition split).
Every input has already passed the strict C11 Replay-v1 parser.
A disjoint complete-match validation pool must contain all three role
positions. Samples are never recombined across the original landlord,
landlord_up and landlord_down Q targets.

The PyTorch-only stable model is ai/dmc/model.py (mygames-douzero-q-v1):
5x162 chronological action history LSTM(162->128) concatenated with
role 319/430 state features + 54 candidate action features, five
512-wide ReLU layers and one Q scalar head. The optimizer uses
role-specific RMSprop/MSE with gradient clipping and explicit device
selection (no silent CUDA->CPU fallback). A full forward/backward
optimizer run is available when an explicitly configured offline
environment has PyTorch. The ordinary native CI deliberately DOES NOT
install heavy unpinned torch wheels.

Model checkpoint v2 has strict ABI: architecture and feature contract,
rule set, objective, source model versions, separate training and
validation episode IDs, optimizer step count and the three explicit
role state_dicts. The trained candidate is first written in a private
same-directory temporary file, fsynced, loaded back via PyTorch
weights_only=True, and its Q outputs compared byte-exactly to
the original models. It is then atomically linked into a NEW
destination with NO-CLOBBER; existing model versions are never
overwritten. The output directory on POSIX must be private.
Full model and raw replay data are never uploaded to CI or written
to the public Outbox.

Training reports per-role validation MSE and MAE BEFORE and AFTER
optimization, plus a zero-Q prediction baseline, on distinct complete
episodes. These are OFF-POLICY regression metrics, NOT online win rate.
--evaluate-only opens a trusted checkpoint, refuses incompatible
rule/objective/model architecture and rejects evaluation data whose
episode IDs were used for training. Real policy strength still needs
matched-deal role-rotated competitive evaluation.

Example (trusted private offline machine with PyTorch already configured):

    python3 ai/dmc/train_dmc.py --train \
      --replay private/train/*.mgdmc \
      --validation-replay private/validation/*.mgdmc \
      --objective win-sign --rules-version 1 --device cpu \
      --steps 1000 --batch-size 64 \
      --output private/models/model-v2.pt

No production KFCore binding, deployment, adaptive self-play collector,
paired-seed tournament or trained-gameplay skill is claimed yet.


## Offline landlord-seat rotation and genuinely independent test sets

The offline MyGames::DMCSelfPlay config may explicitly choose a landlord
seat 0/1/2 for dataset role rotation. The sampler does not change the
shuffle or assign a landlord secretly: it performs the original M5b
bid(0) transitions for preceding bidders, followed by an authentic
bid(3) from the chosen seat. No production WebSocket or CFlow API
accepts this offline-only selector; model bidding is still NOT trained.

The existing M5a C11 test runs three real complete securely shuffled
matches with three DIFFERENT landlord seats. The first two WIN_SIGN
episodes are passed to the pure-stdlib Python training/validation
disjointness acceptance and cannot share episode IDs. This is role
rotation, not matched-deck or seat-swapped paired tournament proof.

The separate --evaluate-only CLI now excludes BOTH training and
model-tuning validation episode IDs from its accepted evaluation
dataset. Held-out prediction MSE/MAE may be measured only on fresh
episode IDs; such off-policy metrics are NOT a real game win-rate
or proof of strategic learning. True paired-deal competition remains
an independent gate.


## M5k R2 test-only matched-shuffle policy assessment

A TEST-ONLY symbol, mygames_ddz_dmc_selfplay_test_entropy(), reuses the
existing M5b mygames_ddz_match_start_test_entropy() to make the EXACT
same 54-card shuffle available under two different policy assignments.
This function and the deterministic entropy provider are compiled into
MyGames::DMCSelfPlay ONLY with BUILD_TESTING=ON. Production
BUILD_TESTING=OFF never includes the test-seeded entry point and
continues to use the unchanged OS CSPRNG admission path.

One test-only candidate policy sheds the most cards; a separate legal
hash-index baseline chooses among the supplied legal moves. Both see
only the currently acting player's authorized observation and legal
candidate list, never a secret match/deck. A complete paired comparison
per shuffle/landlord seat runs (A landlord vs B farmers) AND
(B landlord vs A farmers) with that same shuffled deck. Every played
move, bid/pass, terminal settlement and DMC label still goes through
the SINGLE original C11 rules engine, not a parallel simulator.

For the identity-null gate, A-vs-A and its swapped team assignment
MUST yield the identical terminal winner and M5g score, and the
candidate's aggregate signed win and points MUST cancel exactly.
The CTest exercises all THREE landlord seat rotations and at least
two independently seeded test-only shuffled deals. It prints only
PUBLIC aggregate signed-win and nonmonetary game-point totals:
no private hand/kitty/card order/actor seed/player credential.
No real-player source data or trained model is used.

ai/dmc/paired_stats.py consumes those aggregate seed-cluster rows. The
unit of bootstrap uncertainty is ONE independent 54-card shuffle,
not one of the six correlated matches within its 3-seat rotation.
For fewer than 30 independently sampled deal clusters, 95% bootstrap
intervals are explicitly NULL and results are marked
NOT statistically qualified. A synthetic 30-deal all-zero null
example verifies the interval math, but does NOT qualify the
actual 2-deal source CI sample as learned AI performance.

This is a **structural, symmetric evaluation harness**, not a
claim that the untrained shed-most policy beats a random baseline.
A true checkpoint-vs-heuristic experiment still requires a versioned
native policy inference adapter, independent seeded sets large enough
for uncertainty, and the explicit offline paired-score matrix.
Neither LightZero, MuZero nor another DDZ game engine has been added.


## M5k private C11 batch Replay generator (opt-in Linux offline)

A standalone *offline* training command, mygames_ddz_dmc_generate,
uses ONLY the existing MyGames::DMCSelfPlay static C11 engine plus
the canonical DMC Replay v1 encoder. It is an application executable
in the existing root CMake project, NOT a second game runtime, CFlow
server, installer/consumer test project or additional workflow.
Current file-publication API targets Linux openat/linkat/fsync;
the game kernel and private Replay codec remain multi-platform C11.
No new feature-detection probe/auto-fallback logic is used.

The operator must explicitly name an EXISTING Linux private directory
owned by the effective user, with no group/world access (mode 0700).
Symlink directories are rejected. Each sample is written as a new
0600 private file, then synced and atomically linked under a unique
episode-N.mgdmc name with no replacement of any existing file.
A crash can leave a hidden .partial file; partial bytes NEVER
become a committed Replay. Duplicate Episode IDs, unwritable paths,
full buffers or failed OS-CSPRNG/valid gameplay fail closed.
The file system is not an authoritative Game ACK or durable database.

The CLI accepts an explicitly bounded per-invocation 1..96 complete
games, positive Episode ID starting point and exactly one reward
objective (win-sign or game-points). It rotates offline landlord seats
by Episode ID through legal M5b bidding. A known **UNTRAINED**
mixed-policy-v1 chooses between shed-most and an explicit
observation-conditioned legal-index baseline. It sees ONLY its own
hand and PUBLIC game observations. Hidden deck/other hand partition,
model keys, client credentials and user/private session data are never
part of policy arguments or stdout. The original shuffled deck still
comes from the OS CSPRNG, not from deterministic regression entropy.

Example (existing private folder, explicitly scoped experiment):

    mkdir -m 700 -p /secure/local/dmc-train
    ./build/ddz/mygames_ddz_dmc_generate \
      --private-dir /secure/local/dmc-train \
      --episodes 9 --first-id 10001 --objective win-sign

Then choose DISJOINT COMPLETE GAME IDs for train, validation and
independent evaluation; do not partition turns from the same game.
The existing ai/dmc/train_dmc.py remains the sole model learner.
Only fully sealed private v1 Replay files qualify. No file is uploaded
to CI artifacts, GitHub, MQTT, Cocos or the M5h business ledger.

The existing M5a Linux native workflow now creates a temporary
private directory, generates three genuinely OS-shuffled episodes
with all three offline landlord seats, independently validates
them via the pure-stdlib Python Replay parser/learner CLI and
tests that a second publication attempt cannot overwrite the first.
CI removes the temporary samples on completion. This is a
small CONTRACT/SECURITY test, not an experimental dataset proving
generalization or improvement over a baseline; the private corpus
index/manifest, multi-worker supervision, trained-model action
inference and a statistically powered tournament remain separate.


## M5k three-way verified PRIVATE experiment orchestrator

ai/dmc/experiment.py runs ONE bounded, opt-in offline experiment
inside an existing operator-owned Linux private (0700) root. It calls
ONLY the existing pure C11 corpus generator in three explicit,
nonoverlapping complete-episode partitions: train, validation, test.
Each partition owns fresh episode IDs (never per-step random splits)
and is created with restrictive 0700 permissions, fails closed on
pre-existing names, and accepts at most 96 episodes per partition.

All Replay bytes are independently decoded by ai/dmc/replay.py,
reverified against exact rules_version=1, model_version=1,
objective, canonical per-seat role semantics, 0600/no-extra-hardlink
file permissions, and their original source-episode identifiers.
A PRIVATE manifest.v1.json is published only AFTER every
partition and its learner reader are verified. It contains the
EXACT Git commit SHA, objective, UNTRAINED source policy,
per-file SHA256 and public result/role counts. It does NOT
contain physical card/hidden-hand lists, private feature arrays,
private user IDs or raw game history. The operator owns these
samples, not the CFlow/TurboDB Game Owner or M5h business ledger.
A failed/interrupted experiment cannot publish a completed
manifest; do not recycle leftover private partial files as data.

The operator may explicitly add --train, requiring an already
configured PyTorch backend. This invokes the existing
ai/dmc/train_dmc.py optimizer with disjoint training/validation
games, writes an immutable PRIVATE model checkpoint, reloads and
checks numerical parity, and then independently scores a third,
previously unseen COMPLETE-game test partition. The private
manifest is the final admission step. Metrics are off-policy
DMC MSE/MAE, *NOT* paired-deck competitive win rate, and no
production KFCore inference promotion is implied. No torch
wheel is downloaded by the standard native CI gate.

Example on a trusted offline Linux machine:

    mkdir -m 700 /data/private-dmc-experiment
    python3 ai/dmc/experiment.py \
      --native-generator ./build/ddz/mygames_ddz_dmc_generate \
      --private-root /data/private-dmc-experiment \
      --source-revision "$(git rev-parse HEAD)" \
      --first-id 20001 --train-games 36 \
      --validation-games 12 --test-games 12 \
      --objective win-sign --train \
      --steps 1000 --batch-size 64 --device cpu

The existing M5a Linux source-acceptance job tests a SMALL
3/3/3 triple-split *without importing or downloading PyTorch*
using a temporary 0700 directory and nine genuine,
independently OS-shuffled complete native episodes. It checks
no-clobber retries, private permissions, digest/label consistency,
role coverage, source revision and no train/validation/test
ID overlap. These 9 games are CI evidence for the experiment
orchestration contract, not a trained-policy quality statistic.
The next separate acceptance remains real large-corpus PyTorch
training plus a KFCore/natively scored frozen-policy tournament.


## M5k immutable corpus REUSE / real offline training stage

The existing private experiment orchestrator now supports an explicit
--reuse switch: it NEVER reshuffles, regenerates or overwrites any
Replay. Instead, before training it verifies the immutable private
manifest, exactly matching source Git HEAD (not just a claimed SHA),
the actual native generator SHA-256, all on-disk Replay SHA-256s,
complete per-episode role/turn/terminal reward records, strict
train/validation/test ownership and source policy. Incompatible
model/rules/objective, partial data, extra root files, leaked
symlinks, duplicate IDs or changed source binary all fail closed.

This makes a two-stage operator workflow possible:
1) Generate/validate a larger private experiment once (without
PyTorch) using the OS-CSPRNG C11 source engine.
2) On the *same checked-out* trusted source version, invoke the
same command with --reuse --train and a preconfigured PyTorch
CPU/CUDA backend. This reruns complete Replay admission before
the first gradient step and will never redo a shuffled game.

The private source manifest remains immutable. A successful
training run separately publishes a new private training.v1.json
only after model weights have been loaded back, checked for exact
Q output parity, and independently evaluated on held-out episodes.
No-clobber applies to both the model and its training report:
a failed or interrupted run never silently publishes success.
The report contains model SHA-256 and aggregate MSE/MAE only.
Trained weight/Q metrics are NOT paired-game strategy win rates.

The existing M5a Linux CI verifies --reuse on the actual nine-game
native experiment, rejects mismatched source revision/binary,
ensures no replay regeneration, and still deliberately runs
WITHOUT installing large PyTorch wheels. Real Torch training
on full native Replay is an explicitly gated offline operation,
not performed or falsely claimed by this lightweight CI.


## M5k version-fenced bounded Q scoring boundary

MyGames::DMCQSelector is a pure, allocation-free C11, PRIVATE
offline/native inference boundary for an explicitly selected
model snapshot. It does NOT provide a built-in scoring engine,
a second DDZ rules implementation, random or best-legal fallback,
and has no dependency on PyTorch, KFCore, HTTP, Flowie or TurboDB.

The trusted caller MUST provide the COMPLETE legal mask list from
mygames_ddz_ai_legal_actions(); the selector rechecks every offered
physical mask using the acting player's own hand and authoritative
M5a classify/beat contract. It encodes the 319/430 acting-role
state feature and chronological 5x162 history ONCE per turn.
For each 1..65536 real legal actions, it streams complete, bounded
batches of at most 64 canonical 54-card action feature vectors
to an explicitly supplied model scoring callback. Every returned
Q is required to be finite, with a deterministic physical-mask
tie break for equal Q values. No Top-K truncation is permitted.

The provider must carry an immutable nonzero model version, exact
rules version and acting role. The selector also checks the exact
expected turn ID against the observed round revision. Missing
model, stale version/turn/role, illegal own-hand mask, scorer
failure, NaN/Inf result or over-capacity action count leaves the
output mask untouched and returns an explicit status. This is a
PROPOSAL ONLY; the unique existing CFlow Room Owner must
independently revalidate epoch/game/turn/seat and use the sole
M5a round_submit and durable M5c Game COMMIT path before any ACK.

A separate small MyGames::DMCQSelector policy adapter feeds the
existing OFFLINE C11 Self-Play callback directly. All THREE
role providers must be supplied by the experiment; if even one
fails or mismatches its required snapshot, Self-Play returns
POLICY_REJECTED with no terminal labels or implicit alternate
model. This is the native integration contract needed before
binding a genuine immutable PyTorch/KFCore model.

Existing M5a three-OS and Linux ASan+UBSan CTest now covers:
genuine 20-card M5a legal action list, complete argmax,
bounded multi-batch streaming, stale model/rules/turn/role,
NaN, scorer failure, illegal PASS and no-output mutation;
then a full genuine OS-shuffled offline MyGames match using
three explicit TEST-ONLY cardinality scorers. This proves a
model-shaped legal-proposal boundary, NOT real learned Q
weights, numeric Torch/KFCore parity or higher game strength.


### Strict model output-completeness acceptance

Every Q-batch score slot starts as NaN before invoking the trusted
model callback. A callback that returns success without writing
ALL action values now returns NONFINITE and rejects the ENTIRE
proposal; no unwritten score is implicitly treated as Q=0.
The exact C11 native and Linux ASan tests inject a deliberately
incomplete model callback and require that the proposed output mask
remain unchanged. This is a fail-closed native inference boundary,
not a model provider implementation or learned-policy claim.


### R2 strict full-legal-list admission (new candidate)

The earlier Q selector validated each offered card combination but trusted
the caller's claim that the supplied array contained ALL legal actions.
That is insufficient: a malicious/miswired subset, duplicate or reordered
list could bias the final Q choice despite every individual card mask
being rules-valid. The selector now queries and materializes the SAME
authoritative M5a generator's complete, deterministic ordered legal
action list within the existing 65536-action bound, byte-compares the
whole array, frees its bounded temporary memory, and only THEN submits
actions to an immutable model provider. Mismatch or memory exhaustion
fails closed, leaves output unchanged, and invokes no model callback.
No second classifier, rules runtime, Top-K shortcut, or random fallback.
The existing C11 native/ASan CI has negative coverage for missing,
duplicated and reordered individually valid candidate masks. The
extra legal-list generation is a known throughput cost to measure before
training millions of native Q-proposal turns; correctness is required
before pursuing caching or optimization.


## R3 immutable trained model version and frozen PyTorch Q provider

A real DMC Replay records the behavior/source-model snapshot, which
MUST NOT be confused with the NEW trained Q weights version. The
training CLI now requires an explicit positive uint64 --model-version
STRICTLY greater than every source Replay model_version. A newer
candidate cannot accidentally reuse the behavior version 1/12.

The private checkpoint contract advances from unpromoted v2 to v3:
strict field candidate_model_version, exact 3-role architecture,
feature/rules/objective/source-version identities, immutable training
and model-tuning validation IDs. torch.load(weights_only=True)
requires the expected model version and rejects mismatched weights
before evaluating predictions or publishing a source revision. The
operator-only experiment.py --train passes the same version into
actual PyTorch training AND independent held-out evaluation. The
existing M5a Python admission test refuses source-version reuse
and missing/forbidden candidate model IDs WITHOUT installing Torch.

ai/dmc/frozen_q.py is the configured-Torch-only offline scoring
provider. It consumes ONLY role-specific x (319/430), z (810) and
1..64 canonical rank-count action (54) numeric feature arrays and
one previously accepted immutable checkpoint. It validates all binary
dimensions, strict model/rules/objective identity, finite float32
Q outputs, and returns an exact per-action score vector. It has
NO hidden cards, no C11 rules replica, no fallback model, no
network/Room Owner handle, and does NOT itself authorize actions.
Its output is a future provider binding input to the existing pure
C11 DMCQSelector; native Q parity and KFCore production inference
remain independent acceptance gates.

A local configured PyTorch 2.10 CPU smoke executed actual gradient
steps for all THREE roles on synthetic numerical features and verified
bit-identical frozen score outputs vs direct Torch evaluation.
This was NOT training on the native C11 Replay corpus and cannot
qualify any strategy win rate.


The independent optional smoke entry
python3 tests/test_dmc_frozen_q_torch.py requires an EXPLICITLY
configured PyTorch 2.10+ CPU environment and exercises actual
RMSprop backward/optimizer steps for all three roles, strict
checkpoint-v3 reload, 3/3 frozen Q numeric parity and negative
version/rules/objective/batch-size cases. The sample features in
this optional smoke are SYNTHETIC and do not establish an observed
C11-native game win rate. The ordinary M5a Linux CI only checks
Python source syntax and its existing true native Replay privacy
contracts; it does NOT install heavyweight unpinned Torch wheels.
