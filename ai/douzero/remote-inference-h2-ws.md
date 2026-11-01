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


## R2a implemented pure C11 remote response ticket

The independent first implementation is
\`MyGames::DouZeroRemoteTicket\` in
\`ai/douzero/include/mygames/douzero_remote_ticket.h\` and
\`ai/douzero/src/douzero_remote_ticket.c\`, with a real native
\`mygames.douzero_remote_ticket\` CTest in the EXISTING CMake and M5a
Linux/Windows/macOS/Linux ASan/UBSan matrix. It is **network-agnostic**:
no CHttp, KFCore, C++, Python, model runtime, actor information encoder,
dynamic allocation, Game Owner private match pointer or user credentials
are linked. This is an Owner-facing **callback correlation primitive**,
not an integrated production Game Owner, an inference server, or a claim
that HTTP/2/WS are currently connected.

The C11 Owner creates immutable ticket identity with nonzero
\`room_generation\`, \`owner_epoch\`, \`transport_generation\`, \`game_id\`,
\`turn_id\`, \`ticket_id\`, role/seat/model/source SHA and the digest of
the FULL ordered legal list. Physical seat roles are checked relative to
the chosen landlord seat. Current original fixed-landlord WP profile
must be explicit; the old custom DMC profile has no alias.

The Owner supplies an already computed SHA256 over exactly:

    ASCII("mygames.douzero.legal.v1") || 0x00 ||
    uint32_little_endian(total_count) ||
    for each original ordered legal action: uint64_little_endian(physical_mask)

This covers PASS as zero and the **entire** original legal candidate
order, not just a single network chunk. The C11 correlator compares
SHA256 digest bytes but **does not compute or authenticate SHA**:
the trusted Game Owner must compute it using its existing Crypto
provider, authenticate the chosen model artifact, and rederive/recheck
the legal list at COMMIT. Client-supplied hashes are NEVER authoritative.

A ticket holds only metadata and a **caller-owned score scratch span**,
whose capacity must contain the complete count. The original complete
legal set may exceed the single request maximum 64, so every chunk
has a separate, bounded JSON-RPC/WS **wire request ID**:
\`wire_request_id_base + chunk_index\`. Each echoes a common owner-issued
logical \`ticket_id\`, full legal-list SHA and every other identity
field. Input \`request_id\`, model and legal digests, chunk ordinal/count,
all Q values, and local owner-monotonic absolute deadline are enforced
before writing any score slot. Remote responses can arrive only through
the existing serialized Owner callback path, never modifying a C11 Game
Owner directly from CHttp's async thread.

The full list is capped at 16,384 actions, at most 256 ordered chunks.
If the real legal list exceeds the cap the Owner must reject inference,
NOT silently truncate or take a "top 64" shortcut. No guessed action,
silent PASS, or heuristic fallback is permitted. On a complete vector,
the Owner verifies the CURRENT owner epoch/game/turn/seat/model/transport
generation and digest again; only then does it receive a single
winning **index**, using the earliest original legal-index on a Q tie.
It must still revalidate the corresponding physical action through
existing legal Game Owner COMMIT; the correlator cannot create Game ACKs.

C11 native tests include 130 scores split 64/64/2, input buffer
atomicity, duplicate/out-of-order/partial chunks, wrong model SHA,
legal list SHA, seat/role, game, owner epoch, connection generation,
deadline, finite Q failure, completion, one-shot index, cancellation
and late callbacks after a new transport generation. The tests use
PUBLIC deterministic score arrays, not a real model. They prove
response-ticket behavior only.

### Transport implementation gap

- Existing CHttp H2 JSON-RPC request encoding and typed response
  parsing are **not yet wired** to this C11 ticket: use the existing
  \`chttp_async_client_rpc_submit\` and cancellation handle rather than
  introducing another event loop. In wire JSON, every potentially
  64-bit integer ID is a canonical decimal **string** to avoid
  JavaScript number rounding; do not trust arbitrary numeric coercion.
- Existing CHttp's default \`max_request_body_bytes=8192\` is
  insufficient for a worst-case 64×54 action bit-array plus state and
  history. The qualified H2/WS service must explicitly configure
  a bounded **at least 32KiB** request-body budget, independently bound
  its maximum CSerde/frame/inflight memory, and prove malicious
  oversized input rejection.
- The actual C++17 KFCore inference host, HTTP/2 listener, WS
  reconnection and authenticated TLS/mTLS isolation have **not yet**
  been implemented or run in this MyGames PR. KFCore's separately
  versioned original DouZero WP model semantic profile is on
  [KFCore PR #359](https://github.com/qigao/KFCore/pull/359);
  actual trained upstream model Torch/ONNX/KFCore parity remains RED.
- Current online MyGames C11 house rules still reject some original
  DouZero-only actions. R2a does NOT enable production inference
  until full legal/actor-feature/bidding/history gates are satisfied.
