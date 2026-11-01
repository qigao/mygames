# DouZero remote inference — HTTP/2 RPC / WebSocket, R2 draft

## Architectural decision

**Train with original upstream DouZero / PyTorch. Run trained models in an
independent C++17 KFCore service. MyGames authoritative C11 Game Owner
communicates with the service through existing CHttp HTTP/2 RPC or WS.**

No C++ requirement is imposed on `MyGames::DouDizhuRound`, Match,
settlement or the core-only CMake targets. **Do not** embed PyTorch, ONNX
Runtime, a second C++ game rule engine, or a new native RPC framework in
MyGames. Existing `qigao/chttp` supports CHttp Client/Server JSON-RPC
over HTTP/1.1 and HTTP/2 with strict selected protocol, request IDs,
deadlines and native async cancellation; WS client/server APIs are present.
Existing `qigao/KFCore` Draft PR #357 provides a C++17 ONNX Runtime
inference core with a C-compatible batch-Q API, explicit package SHA256
verification, exact backend selection and no fallback. It is the intended
backend, not a claim that official trained DouZero weights already run
successfully in production.

```text
Original DouZero PyTorch Actor–Learner (offline only)
                  |
       explicit frozen export + verify
                  v
       KFCore signed/trusted model package
                  |
          C++17 inference service
            /             \
       HTTP/2 RPC      WebSocket
        request        stream/control
            \             /
              CHttp client
                  |
     C11 MyGames Game Owner (serial owner)
     ├─ native Round/Match/legal action list
     ├─ actor-owned hand + committed PUBLIC action history
     ├─ bounded request generation, epoch/turn/seat fences
     ├─ receives candidate Q scores; validates legal choice
     └─ authoritative play COMMIT + Game ACK (never the service)
```

## Responsibilities and independent deployment

- `mygames-inference` is a **separate** deployment/process, NOT a direct
  library dependency of the room server. It can be restarted, scaled
  independently, and use a selected KFCore backend (CPU/GPU).
- The service must **never** see the full 54-card deck, opponents'
  individual private hands, credentials, or Game Owner's private match
  snapshot. It receives only one acting player's `x_no_action` (319
  or 430 binary entries), `z` (5×162 history entries), and **the exact
  complete ordered legal-action rows** (N×54 binary entries).
- Service does not generate legal actions or select landlord/bids, and
  it never commits a turn. Inference response contains bounded float32
  Q scores **in exactly the request's legal-action order**. The
  single C11 Game Owner performs argmax and revalidates every action.
  The service may alternatively report an index only when the same
  complete score vector is returned and owner independently verifies it.
- A model session is immutable: provenance must name the **ORIGINAL
  upstream DouZero** model, `wp` objective, exact trained per-role
  weights SHA256, exact ONNX file SHA256, source/upstream commit and
  role. Never mistake retired `ddz-dmc-replay-v1` weights for an
  upstream original checkpoint. KFCore SHA256 checks integrity, but
  the deployment controller must authenticate model package origin.
- Role model handling: landlord, landlord_up, landlord_down are
  independent frozen models. Load/prewarm all required role packages
  before advertising readiness. No unqualified hot-reload or implicit
  fallback from GPU to CPU/heuristic inference.

## Versioned request and response contract

The canonical contract is
`ai/douzero/inference-protocol.v1.json`. The message format is
**transport-independent**; the same fields are used inside an HTTP/2
JSON-RPC 2.0 `douzero.q.evaluate` envelope and within a WS
frame. JSON examples are illustrative, not approval to copy
opponent-secret data into a request.

Required request fields:

- `protocol_version=1`, `request_id`, `game_id`,
  `room_generation`, `owner_epoch`, `turn_id`, `seat`,
  `role` (0 landlord, 1 up, 2 down), `model_version`,
  `model_sha256`, `rules_profile`, `objective=wp`.
- `x`: exact 319/430 binary values; `z`: exact 810 binary values;
  `actions`: 1..64 rows of 54 binary values **per individual
  request batch**. For more legal moves, split into consecutive
  numbered chunks and atomically assemble scores **before choosing
  any move**. Reject missing, duplicated or reordered chunks and
  different model/turn identity.
- `legal_list_sha256` binds the full ordered candidate list, NOT just
  the current 1..64 chunk. `chunk_index` and `chunk_count`
  bind chunk order; the request is invalid if overflow or inconsistent.
- `deadline_ms` is a transport relative deadline, with a separate
  owner-monotonic absolute cutoff; request IDs cannot be reused while
  pending. One outstanding inference ticket per active Game Owner turn.

Successful response must echo all identity fields, full-list digest,
chunk index/count and `scores` (finite JSON numbers in the exact
requested chunk order). Structured errors never include private
features, cards, model weights or user identities.

**Explicitly not supported:** accepting a requested move by index
without full-list hash/fence verification; retrying a timed-out request
as an automatically accepted action; coercing `NaN` to zero;
accepting a successful HTTP 200 or WS frame as Game COMMIT.

## Transports

**HTTP/2 RPC (default):** use the EXISTING
`chttp_async_client_rpc_submit` / HTTP/2 pool and JSON-RPC 2.0
method `douzero.q.evaluate`; exact H2 protocol, explicit authority,
TLS profile/mTLS, bounded per-call deadline, existing
`chttp_async_request_cancel`, native terminal completion once.
The same CHttp server owner mounts bounded RPC methods and refuses
new inference beyond configured inflight budget. **No downgrade to
HTTP/1.1** when H2 is explicitly requested.

**WebSocket (optional):** reuse existing CHttp WS client/server, with
persistent authenticated connection generation and the same canonical
request IDs, model/turn fences and chunk framing. Suitable for model
readiness / service telemetry or high-frequency streaming. Every
response must still be paired to the current generation; reconnect
invalidates outstanding WS tickets, and **no replay of stale actions**
may complete a turn. Do not add an independent WS timing/cancellation
loop if CHttp's owner already manages it.

## Security and failure rules

1. Authenticate the inference service (TLS 1.3 / mTLS or a proven
   private trusted boundary); encrypt actor private features in
   transit, enforce authorization and payload size limits. WS cannot
   share a public unauthenticated game channel.
2. Acquire an owner-issued ephemeral ticket
   `(room_generation,owner_epoch,game_id,turn_id,seat,model_sha256)`;
   copy an actor-only immutable feature snapshot. Never pass borrowed
   C11 Match private pointers across async callbacks/processes.
3. For each response, check current Owner ticket first; reject
   cancellation, deadline expiry, seat/role mismatch, model change,
   game/epoch mismatch, stale turn and legal-list digest mismatch.
4. Full candidate score vector must be finite and complete before
   choosing; deterministic tie-breaking is defined by canonical
   original legal action order. The service cannot send an arbitrary
   mask as an authoritative move.
5. Transport errors, nonfinite Q, oversize payload, model errors,
   process crash, duplicate response, timeout and disconnect all
   **fail closed**. Game Owner decides whether to wait, cancel or
   apply a separately authorized game-level timeout/forfeit policy;
   the inference service must never silently select `shed-most`,
   `legal-index` or `PASS`.
6. No inference-side DB writes, settlement, ledger, durable Game ACK
   or admission of opponent private data. Diagnostics reveal IDs,
   status, model artifact digest prefix and latency only.

## Implementation gates (not yet qualified)

- [ ] Original DouZero's **complete native legal action set** and
  original `get_obs` 319/430+54, 5×162 actor features verified against
  authoritative native Round **including original-only actions**.
  R1b/R1c only cover common-action games.
- [ ] Export actual **original** three-role upstream PyTorch weights to
  ONNX and make independent Torch↔KFCore C++ Q parity on genuine native
  actor-only observations at 1/7/64 actions and over-64 chunked legal
  lists. Existing KFCore #357 includes synthetic topology fixtures,
  **not actual trained original checkpoint certification**.
- [ ] Implement actual stand-alone C++17 inference host and CHttp
  HTTP/2 RPC wiring; prove real mTLS/authenticated request, deadline,
  cancellation, duplicate ID, stale response and process restart.
- [ ] Add optional WebSocket mode using exactly the same canonical
  contract with reconnection and stream-ordering tests.
- [ ] Native C11 owner still validates action, epoch, turn/seat and
  commits via its existing Game ACK; no late response can mutate owner.
- [ ] Run real complete original DouZero-trained models against a
  qualified native opponent baseline, separate from native transport
  tests and private holdout gate. No production AI promotion from
  source/contract CI.

**Design decision:** C++ is welcomed **where inference benefits**.
Inference is a remote service, not part of a C11 game runtime, and
neither a new training algorithm nor another native game authority.
