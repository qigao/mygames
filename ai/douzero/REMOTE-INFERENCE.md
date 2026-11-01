# DouZero Remote Inference V1 — Independent C++ Service (HTTP/2 + WSS)

## Decision and process boundary

- Train unchanged pinned original kwai/DouZero Actor–Learner in Python/PyTorch.
- Offline export trusted original three-role frozen weights to ONNX, verify
  Torch/ONNX Q numerical parity, pin KFCore package and ONNX SHA256.
- Run inference in a SEPARATE C++17 process using KFCore::dmc_q + CHttp::Server.
  The service has its own bounded worker threads; it does not block fixed
  CHttp connection/HTTP2-stream owners with ONNX model inference.
- MyGames Room/Game Owner remains pure C11. Use existing CHttp asynchronous
  HTTP/2 request/reply OR existing WSS binary sessions to obtain scores.
  Never link C++ KFCore/Torch/ONNX into the game process. Only C11 Room Owner
  can validate/commit actual legal physical card action and Game ACK.

     C11 Game Owner (only legal action/turn/COMMIT authority)
           |
           | actor-owned x/z + canonical full legal action list hash
           | mTLS HTTP/2 POST or WSS BINARY (no transport fallback)
           v
     INDEPENDENT C++ inference process
       CHttp mTLS admission and fixed stream/session owners
           -> bounded C++ worker queue (no owner blocking)
           -> KFCore::dmc_q (3 immutable role ONNX handles)
           -> Q-score reply bound to immutable request ticket
           |
           v
     C11 Game Owner aggregates ALL batches, checks live turn/generation,
     selects from ENTIRE legal list, revalidates physical move, commits.

Original DouZero upstream Python source and MyGames C11 rules remain unchanged.

## Actual existing CHttp/KFCore APIs

H2: chttp_server_post, chttp_server_response_defer,
chttp_server_deferred_reply, generation-safe per-stream cancel and H2 GOAWAY.
WS: chttp_server_websocket_with, H1 WSS/H2 Extended CONNECT binary frames,
chttp_server_websocket_session_capture and
chttp_server_websocket_send_binary (copied bounded async command admission).
No new H2/WS socket, poller, owner or automatic retry engine is introduced.

Server C++ module: ai/douzero/service/include/.../douzero_remote_service.hpp
and ai/douzero/service/src/douzero_remote_service.cpp. This is an optional
hostable module with fixed 1..16 workers and 1..1024 combined queued/running
tasks. It requires a server host configured with verified mTLS (and expected
client certificate SHA256 allowlist), HTTP2 enabled, at least 4832-byte
bounded request bodies, 392-byte replies, suitable CNet H2/WS capacities,
request/handler deadlines and exact TLS ALPN. It never accepts plaintext H1
POST, unverified WSS, wrong model artifact digest, or a model fallback.
Actual host process bootstrap and SDK network integration remain to be done.

KFCore stacked Draft PR #359 adds a separate semantic identity for the
ORIGINAL DouZero WP checkpoint: model_type=douzero-original-q,
semantic_contract=douzero.original.wp, semantic_version=1,
artifact_flavor=douzero-original-q-v1. Retired old DMC metadata is never
silently accepted as original trained DouZero. Shared tensor ABI:
z[N,5,162], xa[N,373|484], q[N] float32 with N=1..64.

## Canonical shared request/reply wire

Same binary body for HTTP/2 POST /v1/douzero/q with exact content type
application/vnd.mygames.douzero-q.v1, and WSS binary /v1/douzero/q with
required subprotocol mygames.douzero.q.v1.

Fixed 136-byte little-endian header:
  0..3: DZQI magic
  4: ABI v1; 5: kind=1 request/2 reply; 6: role; 7: WP objective=1
  8..55: request_id, room_epoch, game_id, turn_id, owner_generation,
         model_version; six uint64 LE
  56..63: first_action and total_actions; two uint32 LE
  64..71: action_count, x_dimension, history_dim, action_dim; uint16 LE
  72..103: SHA256 of original FULL canonical ordered legal-action list
  104..135: SHA256 of exact pinned original-WP ONNX role model artifact

Request: exactly role-dependent 319/430 binary x bytes, 810 binary z
history bytes, and contiguous 1..64 ×54 binary action-feature bytes.
Reply: same exact ticket (kind=2) followed by count finite IEEE754
float32 Q values, little-endian. Max request = 4832 bytes, reply 392.
Do not send the other two players' private hands, the deck, room secrets or
trainer weights. Actor-owned card information is private: mTLS is mandatory.

The full-list digest is computed by the authoritative Game Owner:
  SHA256("mygames-douzero-legal-list-v1\\0" ||
         u32le(total_actions) ||
         all ordered (u64le(physical_mask) || 54 action-feature bytes)).
Only the owner knows the complete legal list; the inference service
echoes the digest but does not claim to recompute it from one chunk.
SHA256 is integrity binding, not identity/authentication by itself.

For large move lists do NOT truncate to Top-K: split at exact indices
0,64,128,... up to 65536 total. Every chunk has a unique owner-side
request_id, exact first_action/count and SAME game/turn/generation/model
and full-list SHA. Pure C11 mygames_douzero_score_collector receives
out-of-order H2/WS responses once each, strictly verifies full pending
ticket and finite scores, and becomes complete only after EVERY chunk.
A stream drop, WS backpressure, timeout, cancellation, duplicate or
untrusted/late response invalidates the AI proposal. Before selecting Q
argmax across ALL actions, C11 owner must re-check the LIVE immutable
ticket against its own actor generation and Round turn. Full original
physical-card legal action submit is then revalidated by C11 Round/Owner.
No fallback to another model or heuristic, and no automatic H2->H1
or HTTP2->WSS retry.

All C11 wire operations are endian-canonical, fixed-size bounded and
leave caller-owned outputs unchanged on any failed parse, capacity,
stale identity or nonfinite Q. Transport cancellation cannot commit a
game move. No inference result may bypass the game owner.

## Tested versus still RED

Normal MyGames C11 build has NO C++/SDK/Torch dependencies:
  cmake -S . -B build/game -DBUILD_TESTING=ON
  cmake --build build/game
  ctest --test-dir build/game -R mygames.douzero_remote_wire

Existing M5a Linux/Windows/macOS/ASan validates wire roundtrip,
original-WP only, all six owner identity fields, both hashes, batch count,
output atomicity, NaN rejection and unordered three-chunk 130-action
aggregation. It does not open a real network socket or load a model.

The optional C++ service module is compiled only by explicit SDK opt-in:
  cmake -S . -B build/inference-service \
    -DMYGAMES_ENABLE_REMOTE_INFERENCE_SERVICE=ON \
    -DMYGAMES_ENABLE_GATEWAY=OFF \
    -DMYGAMES_ENABLE_TURBODB=OFF \
    -DMYGAMES_ENABLE_FLOWIE=OFF

This requires a coherent installed CHttp::Server and KFCore::dmc_q. It is
a hostable service LIBRARY, not yet a turnkey secure executable; the
operator-owned process main, verified TLS CA/cert/key configuration,
actual SDK-built C++ host, real mTLS H2/WSS socket tests and cancellation
are separate gates. Do not represent the codec/CMake tests as those gates.

Current independent RED gates:
- Actual deployed CHttp H2/WSS client connector and Game Owner full-legal
  ticket generation and post-reply generation fencing.
- Compile/run of C++ service with coherent published CHttp/KFCore SDK,
  mTLS H2 and WSS network, deadlines, high concurrency and failures.
- Authentic original DouZero 3-role Torch checkpoint -> ONNX -> KFCore Q
  parity on real original legal-action input, not UNTRAINED ONNX fixture.
- Original-only move types vs existing online Round, staged bidding and
  durable actor-only public event projection before real AI promotion.

Every upstream DouZero Actor–Learner source blob remains unchanged.
Retired custom v102/v103/v104 are NOT original DouZero model packages.
