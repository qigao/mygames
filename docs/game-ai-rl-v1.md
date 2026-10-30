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
