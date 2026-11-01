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
