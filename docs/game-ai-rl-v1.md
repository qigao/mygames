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


## R3 numeric PyTorch to native C11 Q reference (2026-10-31 candidate)

ai/dmc/export_q_cpu.py is an explicitly requested, private offline
export of a previously immutable checkpoint-v3. It calls the existing
load_model_checkpoint(weights_only=True) admission. All three
role-specific tensors must exactly match the existing
mygames-douzero-q-v1 architecture, be finite float32, and use a
newer, expected immutable model version. Each role is serialized in
one CANONICAL little-endian float32 file, NOT a raw native struct or
unsafe pickle. Format MGQF0001 has a 64-byte header: version=1,
explicit model version, rules version, objective, role 0/1/2,
319/430 state-feature length, exact float count, payload bytes,
CRC32 field and zero-reserved. CRC32 is for accidental corruption,
NOT a signature or proof of trust. It requires an existing 0700
caller-owned private export directory and writes 0600 files via
same-directory temp+fsync, hard-link NO-CLOBBER and dir fsync.
A manifest with SHA256/source checkpoint digest is committed last.
A half-export never produces a qualified model manifest.

MyGames::DMCQCPUReference is a pure C11 STATIC module and a
numerical parity/conformance oracle, not an optimized inference
provider. A caller supplies serialized bytes and owned float32
memory; the loader independently verifies lengths, CRC32, finite
weights, exact model/rules/role/objective and reserved bytes BEFORE
publishing the model view. It never touches the output model or
arena on a malformed/truncated/nonfinite/old-version input.
The supported three per-role float counts are 1,457,665 landlord,
1,514,497 landlord_up, and 1,514,497 landlord_down.

The native reference reconstructs PyTorch's exact LSTM 5x162
sequence with hidden=128 (gate order i,f,g,o), then concatenates
the context with acting-role 319/430 binary state and 54 action
features, evaluates five ReLU 512-wide linear layers and one
scalar Q head. It reuses the LSTM context across 1..64 distinct
candidate actions in a batch. The inputs MUST be binary; no
hidden opponent hand, new card classifier, HTTP or ORM enters
this model. All output floats must be finite; errors leave the
caller's entire output buffer unchanged. A small C11 adapter
exposes the reference backend as an explicitly selected
MyGames::DMCQSelector score callback, so the sole M5a legal-list
generator and admission fences remain authoritative. The
reference is NOT deployed as an automatic CPU fallback.

mygames.ddz_dmc_q_cpu is a native portable CTest within the
EXISTING root CMake/CTest and M5a three-OS + Linux ASan/UBSan
workflow: tests three role-specific zero-weight images, exact
FC6.bias=1.5 numeric output, version mismatch, CRC corruption,
rechecksummed +Inf weight, too-small arena and non-binary/oversized
Q inputs. It does NOT need PyTorch or install any external SDK.

An independent optional configured-Torch parity script,
tests/test_dmc_q_cpu_parity_torch.py, executes a REAL backward
optimizer update per role using SYNTHETIC binary features,
exports a genuine v3 PyTorch state_dict, runs the C11 scorer
with the SAME feature/action vectors, and compares all 12 Q
outputs against PyTorch. Local PyTorch 2.10.0+cpu + NumPy 2.3.5
produced maximum absolute errors of:
landlord 1.862645149230957e-8;
landlord_up 4.470348358154297e-8;
landlord_down 1.4901161193847656e-8.
It also rejects bad CRC and stale model version. These values
are an ACTUAL Torch/C11 NUMERIC parity result but still only
synthetic inputs, NOT native-game Replay training, an independent
trained opponent evaluation, or an acceptable production KFCore
performance profile. This standalone optional script compiles
the existing library source directly and is not a new installer/
consumer target or CI workflow.

Next separate gates: (1) run the already-implemented native C11
Replay -> PyTorch v3 training on materially more independent
completed OS-shuffled episodes; (2) prove native/KFCore optimized
backend parity and batched throughput under real legal-action
sizes; (3) paired-deck swapped-team 3-seat model-vs-baseline
tournament with sufficient independent seed clusters.


## R4: actual native C11 Replay -> v3 Torch training -> independent native Q parity

This is an ACTUAL local/offline acceptance run, not merely a planned
pipeline or synthetic-feature smoke. The original C11 M5a/M5b/M5g
offline generator was rebuilt under gcc 14.2.0 with strict
-std=c11 -Wpedantic -Werror from the exact public source snapshot
a2f47f09419ccca708ae48ef06f4cf3bdcb83110. For a disconnected
host, a one-time PR22-only PUBLIC source archive was retrieved
through GitHub Actions. It included NO private game Replay, weights,
hidden hands or credentials. The temporary source-upload step is
REMOVED from the normal ongoing M5a workflow by this change.

The original OS CSPRNG and canonical game engine generated 60 complete
independent win-sign episodes under an explicitly UNTRAINED source
policy model 1, split by WHOLE games:
  training    36 episodes, 1547 accepted decisions, roles 530/503/514
  validation  12 episodes,  541 accepted decisions, roles 186/177/178
  held-out    12 episodes,  468 accepted decisions, roles 161/152/155
Episode IDs are distinct: 20001-20036, 20101-20112, 20201-20212.
The strict Python Replay-v1 reader admitted all original C11
samples/terminal returns. No sampled private bytes were uploaded.

PyTorch 2.10.0+cpu actually ran 80 RMSprop/backward optimization steps
PER ROLE on the 36 actual native-game episodes, batch size 32,
learning rate 0.0001, random seed 31. The separate NEW model
version 2, source behavior model version 1, immutable checkpoint
ABI-v3 was written and reloaded. Private checkpoint SHA256:
d883272867820c855ab7d902faa5696ccf690c10de295c90a4131ac071998bc4
(the checkpoint BYTES and hidden information are not published).

Real independent completed-game test results:
  landlord      161 decisions: Q-target MSE 0.573339
  landlord_up   152 decisions: Q-target MSE 0.583183
  landlord_down 155 decisions: Q-target MSE 0.512185
  zero-Q baseline: MSE 1.0 for all three roles
Training-model-tuning validation MSE after training:
  landlord 0.645642; landlord_up 0.816696;
  landlord_down 0.976641.
This is OFF-POLICY terminal-return prediction, NOT a measured
win rate, and only 36 training games are insufficient to
prove strategic superiority or generalization at scale.

Then the REAL trained checkpoint was serialized via the existing
MGQF0001 frozen-role exporter; the pure C11 reference library
source from that same exact commit was compiled on the disconnected
host. New opt-in script tests/test_dmc_real_replay_parity_torch.py
loaded all three manifest/digest-checked private native role images
and the actual v3 trained PyTorch weights. It compared SAME REAL
HELD-OUT Replay feature bytes over 24 sampled accepted decisions PER
role (72 actions total):
  landlord      maximum absolute Torch/C11 Q error 5.364418e-7
  landlord_up   maximum absolute Torch/C11 Q error 7.152557e-7
  landlord_down maximum absolute Torch/C11 Q error 3.576279e-7
No values in this parity experiment are synthetic features.
The optional script rejects source train/tuning-test leakage,
model image checksum mismatch and Q values above tolerance;
only aggregate error and sample counts appear on stdout.
The optional test does not install PyTorch in normal GitHub CI,
which only checks its syntax. The underlying native C11 tests
remain in the existing M5a three-OS + Linux ASan CI graph.

Offline re-run (only with a private locally generated C11 Replay
corpus, configured Torch and native reference built from exact
source, never in a public CI artifact):

    python3 tests/test_dmc_real_replay_parity_torch.py \
      --checkpoint PRIVATE/models/dmc-v3-real-replay.pt \
      --export-dir PRIVATE/exports \
      --native-library PRIVATE/build/libddz_q_cpu_reference.so \
      --replay-dir PRIVATE/test --model-version 2 \
      --objective win-sign --max-per-role 24

Independent remaining acceptance: a FROZEN learned model must
actually select legal actions inside same-deck, swapped-team
three-seat native games, with sufficiently many independent
deal clusters and confidence intervals; the optimized KFCore
runtime must prove numeric parity and throughput versus this
slow C11 reference. Do NOT infer a trained win rate or
production Bot qualification from lower Q-target MSE.


## M5k R5 — first ACTUAL frozen model legal-action tournament (2026-10-31)

The first real model-versus-baseline TOURNAMENT has now run, using
exact native public-source rules, a PRIVATE trained v3 checkpoint
with SHA256
d883272867820c855ab7d902faa5696ccf690c10de295c90a4131ac071998bc4
and actual v2 model weights learned on 36 independent OS-shuffled
C11 training games (80 RMSprop gradient updates per role).
The checkpoint bytes and game Replay feature rows remain PRIVATE.

New source: ai/dmc/include/mygames/ddz_dmc_tournament.h and
ai/dmc/src/ddz_dmc_tournament.c, compiled ONLY when
BUILD_TESTING=ON with MYGAMES_DDZ_MATCH_TESTING=1.
It invokes the existing selfplay_test_entropy() in the SINGLE
authoritative M5a/M5b/M5g native game engine and streams
the acting player's x/z/action features through the already
fenced MyGames::DMCQSelector. PyTorch NEVER sees a private
Match, shuffled deck, kitty or opponents' hidden hands.
No alternative/DDZ Python rules, test shuffle in production
or fallback scorer is provided. Any callback error, incomplete
Q, NaN/Inf, illegal action, stale model or rule error ABORTS
the entire candidate game without publishing a terminal result.
An isolated no-Torch CTest validates exact known same-deck,
team-swap and A-vs-A identity-null cancellation, as well as
model failure/NaN/omitted score/version rejection. This new
CTest is added to existing M5a 3-OS/ASan CI, with no separate
installer, consumer project or workflow.

New opt-in offline Python entrypoint ai/dmc/tournament.py loads
ONE exact private checkpoint v3 (weights_only) and exposes a
batched Q callback for canonical C11 candidate actions. It
caches the frozen LSTM history context and fixed first-layer
projection ONLY within the same actor observation, and
spot-checks its results against direct full PyTorch forward
with 2e-5 tolerance; this is exact network algebra, not a
heuristic replacement. The evaluator forms each cluster from
one exact TEST-ONLY seed's physical shuffled deck, all THREE
landlord positions, and TWO swapped candidate/baseline team
assignments (six completed C11 games per independent shuffle).
Two identity-check clusters also run A-vs-A model on all seats
under swapped labels to prove deterministic terminal
winner, signed win and nonmonetary points cancellation.
It prints only aggregates: no seed list, hidden hands,
physical cards or raw Replay/model bytes. Importantly these
test-only deterministic seeds are NOT OS CSPRNG samples and
should NOT be confused with the independent real-data generator.

Measured private local experiment: 30 distinct pseudo-random
test-deal seeds per baseline, exact same 30 seeds for both
comparisons; 180 model-versus-baseline native COMPLETE games
and 12 extra A-vs-A identity-null games per comparison.
Each 95% bootstrap interval resamples the 30 seed/deal
CLUSTERS (not the 180 correlated games/turns); 4000
replicates with fixed statistical seed. Outcomes:

  Baseline: deterministic "shed-most-cards"
    frozen model team win rate: 11.1111%
    exploratory 95% cluster bootstrap: 7.2222%..15.5556%
    candidate mean nonmonetary team points/game: -7.4667
    point interval: -9.8 .. -5.6667
    score-provider failures: ZERO

  Baseline: observation-hashed canonical legal-index
    frozen model team win rate: 53.8889%
    exploratory 95% cluster bootstrap: 48.3333%..60.0%
    candidate mean nonmonetary team points/game: +0.8
    point interval: -0.3333 .. +2.1333
    score-provider failures: ZERO

These are ACTUAL model-selected/legal/native settled game
results, unlike earlier MSE-based claims. They show this
SMALL-CORPUS model performs VERY POORLY against shed-most
and does NOT demonstrate statistically reliable superiority
over the hashed legal-index baseline. The fixed, limited
test-only shuffle-seed distribution and 30 clusters constrain
any wider interpretation. No competitive win-rate claim is
made beyond these two explicitly defined baseline protocols,
and NO promotion/live Bot authorization follows.
The two complete 30-cluster runs completed locally in about
30.5 and 31.6 seconds with observation-level LSTM caching
and 1..64 canonical legal-action scoring batches, respectively.
This measures the local Python+native reference architecture,
NOT optimized KFCore production throughput.

Example on an offline machine with configured PyTorch and the
native TEST-ONLY shared library built from exact repo source:

    python3 ai/dmc/tournament.py \
      --checkpoint PRIVATE/models/dmc-v3-real-replay.pt \
      --native-library PRIVATE/build/libddz_tournament.so \
      --model-version 2 --seed-clusters 30 \
      --identity-clusters 2 --baseline shed-most

Repeat with --baseline legal-index. No private weights,
replays or raw actions may be uploaded into ordinary CI.
Actual C11 control/ABI tests continue in the existing CI.

STRICT remaining gate: collect materially larger,
diversely generated REAL OS-CSPRNG Self-Play training
data; retrain and independently validate a frozen model
against the stronger baseline with upper/lower uncertainty
thresholds; then compare KFCore optimized role-Q inference
against the existing float32 reference on full legal-action
sets and latency budgets. Current model FAILS the stronger
baseline criterion, and PR #22 remains DRAFT/UNMERGED.


## R5b — revised private DMC training and disjoint HOLDOUT tournament

After observing R5's initial 30-cluster development-tournament
scores, the training data were expanded using the same original
native C11 OS-CSPRNG game generator and legal/terminal rules,
in a SEPARATE private offline experiment:

  train       240 COMPLETE games / 10170 accepted actions
  validation   48 COMPLETE games /  2002 accepted actions
  test         48 COMPLETE games /  1953 accepted actions

This is a distinct corpus from R4's 36/12/12 samples.
The trained candidate remains a SINGLE selected
DouZeroQ architecture with three independent role heads.
It was trained from scratch with real Torch 2.10 CPU,
400 RMSprop gradient steps PER ROLE (batch=32,
learning rate 1e-4, seed=31), then saved and reloaded
as IMMUTABLE checkpoint-v3, NEW model_version=3,
behavior source model_version=1. Checkpoint SHA256:
da82f275e5c3d0fb1cc77a8f4d309f49cc6e41fe5da3ed3d2cc3952ad34a8ee5

On the new separate 48-game native test Replay, off-policy
prediction MSE was landlord 0.6721, landlord_up 0.7055,
landlord_down 0.7372. The previous candidate v2,
evaluated on those SAME 48 test games, got 0.7955,
0.8177 and 0.8213 respectively. However v3's
validation-set MSE remained 0.9347, 1.0967,
1.0747 (landlord/up/down), exposing instability or
overfitting; therefore training loss and test Q-MSE
are NOT promotion criteria by themselves.

R5 development tournament reused its first 30 test-only
shuffle/deal seeds for model-to-model DIAGNOSTIC
comparison. Frozen v3 under 180 native paired matches
per baseline (three landlord seats, two swapped teams):

  vs shed-most: 26.6667% model win, 95% cluster
    bootstrap 19.4444%-34.4444%,
    nonmonetary points/game -5.0667,
    interval -7.4 .. -3.1333
  vs legal-index: 74.4444% model win, 95% cluster
    bootstrap 68.8889%-79.4444%,
    points/game +4.6667,
    interval +3.2667 .. +6.0333

Because those DEVELOPMENT seeds were already observed,
new version ai/dmc/tournament.py explicitly supports two
DISJOINT seed schedules:
  --seed-schedule development (fixed 0xBADCAC1 test PRNG)
  --seed-schedule holdout     (fixed 0xBADCAC2 test PRNG)
and verifies there are NO repeated deal seed identities
between the two schedules. A selected frozen model MUST
be evaluated on HOLDOUT seeds, not on the schedule used
while choosing/training/checkpoint versions. These are
reproducible TEST-ONLY deterministic shuffles, not
OS-CSPRNG game sources or guaranteed IID population
sampling; results are protocol-specific exploratory
bootstrap estimates.

NEW, unseen holdout results on v3 (30 distinct
holdout-deal clusters per baseline; 180 complete C11
paired matches and 12 additional A-vs-A identity-null
games in each comparison, ZERO Q scorer failures):

  vs shed-most: 26.6667% model win,
    95% cluster bootstrap 20.5556%-33.3333%,
    average nonmonetary team points -4.2667,
    interval -5.5667 .. -2.9
  vs legal-index: 78.3333% model win,
    95% cluster bootstrap 72.7778%-83.8889%,
    average nonmonetary team points +5.2333,
    interval +4.0 .. +6.5333

Every model move was chosen from COMPLETE canonical C11
legal masks using the same version-bound pure native
DMCQSelector and frozen PyTorch Q. Every game reaches
the original M5a/M5b/M5g C11 terminal settlement.
The model never saw shuffled decks/opponents' hands,
and no private Replay bytes/weights were uploaded.
The result is a legitimate OFFLINE learned-policy
gameplay comparison, NOT a user/public deployment.

Promotion gate MUST require (a) frozen model reliably
beats the stronger shed-most baseline: held-out
95% deal-cluster bootstrap LOWER bounds for win rate
above 50% AND average nonmonetary team points above 0;
(b) robust results against multiple baselines;
(c) optimized KFCore model/provider parity/latency
and all prior IAM/Owner gates. Current v3 FAILS
(a) badly, therefore **NO online Bot, merge/release,
or general strategic-superiority claim is justified**.
Additional on-policy C11 trajectories and larger
diverse self-play training with new untouched holdouts
are needed before another promotion trial.


## M5k R6 — genuine frozen-v3 ON-POLICY C11 Replay collection and v4 regression (2026-10-31)

The previous model-v3 tournament is not the end of AI learning. A NEW
MyGames::DMCOnPolicy pure C11 offline module reuses the SAME CSPRNG
native Match, legal move generator, DMCQSelector full-list gate and
terminal DMC Replay encoder. It does NOT use the M5b deterministic
TEST-only shuffle; every game independently calls the original
OS CSPRNG and follows the sole native bidding/playing/settlement code.

All three acting players' action selections come from one immutable,
explicit, version-fenced frozen model (no scripted, random or CPU
score fallback). The scorer callback receives ONLY the current
actor's authorized 319/430 x, 5x162 z and 1..64 legal 54-card
action vectors, NEVER the match, secret shuffled deck, kitty or
opponents' hands. The C11 collector records every accepted action
and tags EVERY sample with the **behavior SOURCE model version**,
not the future trained candidate model version. On complete,
sealed and source-validated terminal settlement it stages a
canonical Replay v1 in bounded temporary memory and copies it
to caller-owned output atomically. Invalid/missing version,
wrong rules/objective, scorer false/NaN/unwritten Q, truncation,
any engine failure or too-small output rejects without altering
the output Replay buffer, output length or move count.

The optional operator-only ai/dmc/collect_onpolicy.py takes an
existing PRIVATE 0700 empty output directory and an explicitly
selected frozen PyTorch v3 checkpoint and native shared library,
both PINNED by separately supplied SHA256 hashes. It runs
the actual OS-CSPRNG native collector through the same frozen
Torch scorer used in R5 paired tournaments. Each complete game
is re-decoded using the independent strict Python canonical
Replay v1 reader, and then published as owner-only 0600 bytes
with fsync and atomic NO-CLOBBER hard-link. A last-published
onpolicy.v1.json binds behavior model version, source checkpoint
SHA256, native library SHA256, objective/rules, per-episode
Replay SHA256 and three-role counts; raw feature rows,
hidden hands, private weights and deck cards NEVER appear
in stdout or the manifest. The manifest's source Git revision
is explicitly an asserted claim for disconnected source
directories, not cryptographic repository attestation.
CRC32 detects corruption, not malicious tampering;
the entire private directory is a trusted local ACL boundary.
The current frozen-v3 Python scorer supports win-sign only;
game-points requires a separately qualified checkpoint.

A real LOCAL experiment ran 72 separate NEW completed games
with frozen model v3 controlling ALL THREE seats, each using
the original OS-CSPRNG shuffled deck:
  training      48 episodes / 2081 accepted moves, roles 717/679/685
  validation    12 episodes /  569 accepted moves, roles 193/188/188
  held-out      12 episodes /  595 accepted moves, roles 205/194/196
All episodes are disjoint and have unique IDs; frozen model v3
source checkpoint SHA256 is
da82f275e5c3d0fb1cc77a8f4d309f49cc6e41fe5da3ed3d2cc3952ad34a8ee5.
ZERO model callback failures, no public Replay, user accounts
or live Game Owner. These samples were not generated under
the earlier untrained baseline policy: their source version is 3.

Using this genuine behavior-v3 corpus, PyTorch 2.10 CPU
completed 240 RMSprop backward/optimizer steps PER ROLE,
batch 32, seed 43, objective win-sign, and produced an
immutable NEW candidate checkpoint ABI-v3, model_version=4,
source_model_versions=[3], SHA256:
c91f83016f7416afa9d18116ef18ebfc46ebdbfa906f5fa8bd2bf8f1c52d655c
(the private checkpoint BYTES remain local).

On EXACT same 12 completed game (595 action) unseen test:
  role           v3 source Q MSE     v4 new Q MSE
  landlord          1.376101          0.718754
  landlord_up       1.479194          0.752510
  landlord_down     2.055951          0.579389
However v4's model-selection VALIDATION MSE was
1.797933 / 1.869211 / 1.696703 across landlord/up/down,
all WORSE than zero-Q baseline MSE=1.0. This is a severe
generalization/overfit or sampling instability indicator,
not evidence of competitive skill. v4 MUST stay
unpromoted, must not be described as beating v3 in games,
and must never be tested on R5's already consumed
holdout as though it were fresh. A totally new paired-deal
schedule and stronger baseline must be reserved if v4
ever becomes a candidate for strategy-level evaluation.

The new 3-OS C11 test mygames.ddz_dmc_onpolicy uses an
explicit TEST scorer (card count, NOT a trained model)
to drive three REAL OS-random completed matches with 0/1/2
landlord positions; it decodes all three sealed Replay,
checks accepted actions, source-model version=42 for each
sample, role coverage, terminal labels, and all failure
atomicity conditions. Linux ASan+UBSan uses the same
existing native test graph. A pure-stdlib Python
test tests/test_dmc_onpolicy_admission.py validates
private files/no-clobber/directory ACL/symlink rejection
and mismatch SHA256 inputs WITHOUT importing Torch/NumPy.
No extra workflow, installer/consumer project or CI probe
has been added.

Next independent R6 gate: verify split manifests and
source checkpoint hashes together before any v4+ optimizer
admission, collect materially more frozen-v3/v4 source
episodes with diverse opponent policies, and measure
fresh-seed paired games against shed-most and multiple
nontrivial baselines with cluster-level confidence.
Optimized KFCore float32 throughput/numeric equivalence
to the earlier C11 reference remains unqualified.


### R6.1 strict on-policy train/validation/test source admission

The pure-standard-library ai/dmc/verify_onpolicy.py checks THREE
entire private on-policy directories before any optimizer starts.
It pins the selected immutable behavior v3 checkpoint's real SHA256
(and actual owned 0600 file bytes), the explicit native C11 shared
library SHA256, source revision CLAIM, exactly one behavior model
version, rules and win-sign objective, and the fact that the shuffle
path is the genuine OS-CSPRNG collector rather than deterministic
tournament shuffling.

Every onpolicy.v1.json and Replay must be owner-only regular files
with no extra hardlinks or symlinks. The verifier independently
reopens each Replay with the canonical CRC/terminal actor-role/target
reader, checks every Replay SHA256 and original Episode ID,
landlord/winner/role counts, source behavior model version, declared
Q batch counts and complete directory contents. No duplicate Episode
ID or file content may appear across train, validation or test.
A future target model version MUST be greater than the frozen
behavior model version. SHA256 is integrity/provenance bookkeeping
within a trusted private ACL, NOT a digital signature, and
source_revision_claim is explicitly a claim when using an offline
source tree without Git metadata; it is not represented as
cryptographic repository attestation.

The first real 48/12/12 on-policy corpus above passed this FULL
three-way auditor locally:
  behavior v3 -> candidate v4, train 48 / 2081 actions,
  validation 12 / 569 actions, test 12 / 595 actions,
  exact source checkpoint SHA256 pinned, zero overlaps.
Ordinary M5a GitHub CI now adds only the legitimate negative
verification test (missing manifests, source digest mismatch,
version reuse, empty completed-game cohort and train-as-test),
plus Python syntax; it does NOT upload private features or
fabricate a positive trained on-policy cohort, and does not
install Torch.

Using an actual accepted on-policy source still does NOT prove
that the newly fitted policy outperforms previous source v3
in SAME-DECK competitions. The candidate v4 has held-out Q
regression improvements but fails its separate validation
zero-Q MSE baseline. No deployment or release is allowed.


## R7 validation-aware DMC training; full-game/source/outcome sampling

The previous trainer selected every batch uniformly from accepted MOVES,
giving long matches disproportionate gradient weight, and always published
the very LAST optimizer weights even if an earlier tuning-validation snapshot
was better. These are plausible contributors to v4 instability, NOT a
proved explanation.

New opt-in --sampling episode-source-outcome draws a BEHAVIOR source model
version uniformly, then one winner/loser sign uniformly, then one COMPLETE
native Game episode uniformly, and finally one acting-role move uniformly.
No action legality or C11 return is regenerated. It requires both outcome
classes for ALL three roles under EVERY behavior source in both training
and tuning-validation splits. Train/validation must contain the exact same
source version set; missing classes/sources fail closed. A source-model
version number alone is NOT checkpoint authentication; private source
manifests/SHA-256 remain separate acceptance and model-mixing proof.

New opt-in --checkpoint-policy best-validation snapshots each role at the
minimum tuning-validation decision-MSE across positive-step intervals
--validation-interval N and final optimizer step; it CLONES immutable
parameter tensors before continuing optimizer updates. The private
optimizer report includes selected_validation_step_by_role.
Checkpoint-v3 field training_steps_per_role still records the total
performed update count, not the selected earlier snapshot step.

--require-validation-advantage is a STRICT optional publication gate:
ALL three roles must have finite validation MSE strictly below their
zero-Q validation baseline or NO model checkpoint is published.
That is a minimum regression admission, NOT proof of game strength.
Existing behavior stays the default: transition sampling + final-step
checkpoint. No production Game Owner changes or Torch added to CI.

Pure stdlib tests/test_dmc_training_policy.py checks synthetic whole-game
source/sign/game weighting, repeatability, duplicate and missing outcome
rejection and strict zero-Q admission. Actual C11 Replay-v1 reader
contract remains separately enforced by the existing M5a workflow.

Offline operator example (prevalidated private native Replay, configured
PyTorch required, no model download/fallback):

    python3 ai/dmc/train_dmc.py --train \
      --replay PRIVATE/train/*.mgdmc \
      --validation-replay PRIVATE/validation/*.mgdmc \
      --objective win-sign --rules-version 1 \
      --model-version NEW_ID --output PRIVATE/models/candidate-v3.pt \
      --steps 400 --batch-size 32 --learning-rate 0.0001 \
      --sampling episode-source-outcome \
      --checkpoint-policy best-validation \
      --validation-interval 20 --require-validation-advantage

Independent local exploratory experiment restored PUBLIC exact C11 source
a2f47f09419ccca708ae48ef06f4cf3bdcb83110 and used the genuine
OS-CSPRNG, authoritative untrained v1 self-play generator:
96 complete TRAIN games / 4,025 moves, separate 24-game
tuning-validation / 981 moves and 24-game final TEST / 1,017 moves.
Exact different episode ID ranges, same Torch 2.10 CPU seed 31,
160 optimizer updates per role, batch 32, LR=1e-4.
Only the sampling/selection policy differed. Offline return-Q MSE:

                                  landlord    landlord_up    landlord_down
    Old FINAL-step validation       1.0311      0.7893         1.1908
    New BEST validation             0.7661      0.6441         0.9085
    Old unseen test                 0.8619      0.8446         0.7126
    New unseen test                 0.7832      1.0106         0.7858

All three roles improved on the VALIDATION data used for selection.
Independent TEST was MIXED: landlord improved, farmer_down worsened,
farmer_up even exceeded zero-Q MSE 1.0. Thus no universal
out-of-sample improvement, stronger opponent win rate, v5 promotion,
or learned competitive advantage has been proven. This test has
already been observed; use entirely new reserved held-out games
for next model selection and tournament qualification.

## R7b — validation metrics must follow the admitted training distribution

With explicit `--sampling episode-source-outcome`, the NEW evaluation
objective is the exactly corresponding per-role stratified MSE:

`mean(source -> mean(sign +/- -> mean(complete game -> mean(role decision squared error))))`.

A long game and a short game inside the same source/outcome cell therefore
have EQUAL total influence on checkpoint selection and the zero-Q rejection
gate. Train and tuning-validation must each pass the existing same-source,
positive/negative and complete-game admission. Missing/bad/nonfinite per-game
measurements fail closed. `--checkpoint-policy best-validation` selects the
minimum aligned tuning metric at the configured intervals and last step;
selected role-specific steps are recorded, and immutable cloned weights are
restored before private publication. `--require-validation-advantage`
compares *that same aligned metric* against its aligned zero-Q MSE for every
role. An observed held-out TEST result may never be reused as a fresh test
after the model or training policy has been tuned against it.

The existing `--sampling transition` mode is unchanged: transition-weighted
MSE, final-step checkpoint defaults, and optional legacy MSE zero-Q gate.
The historical `validation_before`/`validation_after` report keys retain
their transition-weighted semantics in ALL modes for backwards comparison.
Explicit stratified mode ALSO reports
`validation_aligned_before`/`validation_aligned_after` with the exact
source/outcome/game weighting and
`validation_selection_weighting=episode-source-outcome`; the default
reports `validation_selection_weighting=transition`.

The pure-stdlib contract test uses differing one- versus twenty-decision
complete episodes to verify a case where ordinary transition MSE FAILS
the zero-Q baseline while aligned MSE PASSES. This establishes evaluation
arithmetic, NOT measured live game strength. Standard GitHub CI has no Torch
trainer install and therefore cannot assert real optimizer quality or
independent frozen tournament superiority. Strong shed-most paired-deal,
three-role/swapped-team NEW holdout and KFCore parity remain separate gates.
