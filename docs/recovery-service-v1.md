# M4 proposal: authenticated read-only recovery service (NOT IMPLEMENTED)

Status: **draft code, not qualified for deployment**. PR #1 contains
a local reconciler, verified TurboDB SQLite read-only source snapshots,
a strict recovery wire encoder, bounded per-principal admission gate and an
**experimental standalone loopback CHttp/CNet mTLS-only endpoint**. Linux
installed SDK build and multi-platform core tests pass, but real HTTPS
interoperability is **RED** (GmSSL TLS13 handshake failure and previously
observed SIGSEGV). There is no approved deployed service identity/rotation,
no independently supervised remote consumer, and no production no-loss claim.

Related: [MyGames #4](https://github.com/qigao/mygames/issues/4),
[Flowie #22](https://github.com/qigao/flowie/issues/22),
[PR #1](https://github.com/qigao/mygames/pull/1).

## Existing trusted foundation

The authoritative Room Owner commits checkpoint, command result and sanitized
Outbox as one TurboDB transaction. Delivered Outbox history remains queryable
by exact seq. The consumer keeps (source namespace, consumer ID, seq, SHA256
receipt) and its contiguous cursor/effect in a separate transaction. The
existing local --catchup/--verify-all CLI is bounded; it never sends an MQTT ACK.

The source reader **must** call
`mygames_room_store_open_readonly(driver,path,&source)`, which asks the pinned
TurboDB SQLite backend for `open_mode=read_only` / `SQLITE_OPEN_READONLY`.
Opening an absent/inconsistent archive fails without creating/repairing
schema, setting WAL, claiming owner epoch or publishing an ACK. All transactional
writer methods on that handle return `MYGAMES_STORE_READ_ONLY`. This
does not by itself authenticate a client or make an archive tamper-proof.

## Implemented policy-only admission core (NOT mTLS transport)

`MyGames::RecoveryGate` is now a dependency-free C11 library that copies
explicit policy entries and binds a caller-supplied **verified** TLS peer leaf
SHA256 fingerprint to one source namespace/consumer stream. It denies unknown
or disabled identities, duplicate stream registrations, malformed ACLs,
out-of-range cursor/page requests, concurrent work per principal and exhausted
per-process token buckets. Each granted read has a generation-fenced terminal
release so stale/double release cannot refund credits or free a newer grant.
CMake runs deterministic admission tests on the Linux/Windows/macOS core jobs.

**Critical:** The experimental CHttp handler now calls this module
using CHttp's `peer_certificate_sha256` from the TLS request view. It never
trusts a forwarded HTTP header and uses `CNET_TLS_CLIENT_AUTH_REQUIRED`,
certificate CA verification and an explicit leaf fingerprint pin. However
the real TLS13 handshake has **not** passed end-to-end. This is still
loopback CI/local-only, with no service identity rotation, distributed rate
limit or externally supervised cursor recovery. It is **not a production
recovery service**.

## Implementation and exact CI evidence (experimental)

- `mygames_room_store_read_archive_page()` pins checkpoint/journal and
  contiguous up-to-16 event bytes in a single readonly SQLite transaction.
  A stale/future cursor, hole, corrupt archive, writable handle and oversized
  request fail closed. No source mutation or ACK.
- `MyGames::RecoveryWire` strictly parses the only recovery URL parameters
  and serializes only canonically validated allowlisted events. Three-OS
  unit tests cover unknown/duplicate/overflow/encoded query injections.
- `mygames_recovery_service` starts a standalone CHttp server **only on
  127.0.0.1 with mTLS REQUIRED**. Network Owner callbacks only authenticate
  and admit a copied, bounded request; a dedicated bounded worker opens the
  source READONLY and uses CHttp deferred terminal completion. It never
  shares the Room gateway listener or writes a consumer cursor.
- Local administrative reconciliation now uses snapshot page reads and applies
  receipt/effect/cursor **per event**, allowing recovery at any partial-page
  boundary. This is not remotely supervised recovery.
- Source+wire tests and prior Flowie recovery remained green through
  [Actions #40225970773](https://github.com/qigao/mygames/actions/runs/40225970773).
  The overall CI is deliberately RED at the **real native mTLS step**:
  standard OpenSSL TLS ClientHello once crashed GmSSL (`SIGSEGV -11`);
  constrained P-256/ECDSA no longer crashes but native TLS13 rejects with
  `unexpected_message` before CHttp handler entry. Track upstream
  [Salts/CNet #1317](https://github.com/qigao/salts/issues/1317).

Do not downgrade to plaintext or disable client verification to make CI green.

## M4 network API contract (partially implemented, TLS gate RED)

A **separate** recovery service reads a source database with the read-only
handle. Do **not** add the endpoint to the loopback development room gateway
or give consumers a source-database file handle. A production listener must
require **CHttp/CNet TLS with client certificate verification** before routing.
Bind the verified CHttp `peer_certificate_sha256` to a provisioned principal
and immutable authorization tuple:

    principal_id -> (source_namespace, consumer_id, recovery.read)

Neither Topic, JSON payload, URL query nor forward/proxy certificate header
may select the identity or source namespace. Use bounded policy storage,
explicit revocation and overlapping pins for controlled certificate rotation;
test no-certificate, wrong-certificate, revoked, alternate namespace, wrong
role, expired credential and cross-tenant attempts. Deny by default. Never
log certificates, bearer secrets, private game state or raw payload.

Only one planned route, no writes:

    GET /internal/recovery/v1/events?after=<decimal-u64>&limit=<1..16>

`after` is the consumer's **last durably committed** global seq, not the
last seen MQTT packet or last page delivered by the HTTP server. Reject
negative, +sign, overflow, leading garbage, duplicate query keys, unexpected
parameters and values beyond INT64_MAX. `limit` is always explicit, max 16.

Planned success response, with events in increasing contiguous sequence:

    {
      "v": 1,
      "source_namespace": "<server-bound namespace>",
      "consumer_id": "<server-bound identity>",
      "owner_epoch": 1,
      "checkpoint_seq": 123,
      "retained_from_seq": 1,
      "events": [{"seq": 42, "payload": { "...": "canonical public event" }}],
      "resume_after": 42,
      "has_more": true
    }

The server must validate **each** archived public event against its exact
source epoch/seq and an immutable allowlist before returning it; never return
private cards, resume tokens or database paths. For a page, capture a stable
source checkpoint/high-water in a genuine read transaction (or detect concurrent
changes and fail closed), rather than assuming separate SELECTs are one
snapshot. Detect archive gaps at or below the checkpoint as corrupt, not
"end of history". A response is a **read**, NOT a commit/ACK/disposition.

Suggested fail-closed status mapping: 401 missing/invalid identity; 403
principal without permission; 400 invalid cursor/limit; 409 after beyond
checkpoint or incompatible source epoch; 410 after+1 older than retained
history (manual rebuild required); 429 rate limit with Retry-After; 503
transient source unavailable; 500/503 sanitized integrity failure. A client
must not advance its durable cursor for any of these statuses.

## Bounds, rate and supervision

Target hard bounds at first release (adjust only with measured installed-SDK
evidence): <=16 events/page, <=1 KiB per event, <=32 KiB serialized response,
one active recovery per principal+consumer, no more than 8 requests/sec with
burst 16 per principal, <=64 total in-flight requests. Enforce limits **before**
archive reads or allocating pages. Any memory-only token bucket is per-process:
multi-instance operation needs shared admission; do not imply distributed
rate enforcement without its own verification. `429` is a bounded failure, not
an implicit retry. CHttp network callbacks must not block on database
operations: copy a bounded request, admit work to a single archive reader
owner, and complete using CHttp deferred response with bounded shutdown drain.
If worker admission is full, respond 429/503 and leave both cursors unchanged.

Require explicit archive retention watermark and slowest authorized consumer
lag. Once `retained_from_seq > consumer_cursor+1`, refuse repair (410); no
auto-skip and no fabricated snapshot. Archive pruning, if later enabled, must
preserve the retention contract and must not be controlled by GET clients.

## Persistent cursor and fencing: primary truth is at the consumer

`MyGames::ConsumerProjection` already persists `last_seq` and SHA256
receipts transactionally with each local projection effect. The remote
consumer must (1) read `snapshot()` **after restart**, (2) request
`after=last_seq`, (3) verify ordered canonical payload/namespace/epoch, and
(4) call `apply()` in strict sequence. Only positive DB COMMIT authorizes
the next request cursor. An uncertain COMMIT quarantines processing until
independent DB reopen/audit. REPLAY with identical hash is safe; different
hash or a gap fails closed. Reconciliation and live MQTT input must share
one consumer-stream owner/serial queue or explicit transactional CAS conflict
resolution. A page's `resume_after` is an informational hint, NOT a
durable commit: a client must never save it before applying the page.

Optional *server-side* request progress is audit/admission metadata only. It
must **not** advance on page delivery and must not be treated as proof of
downstream COMMIT. Any future externally confirmed cursor requires a separate
versioned acknowledgment contract plus fencing and verifiable consumer
commit ownership; never add implicit write authority to the source archive.

## Flowie #22 dependency

Current Flowie 1.0.4 incoming QoS1 `on_message` returns before consumer
projection DB COMMIT, so native PUBACK can precede persistence. Do not block
Flowie worker or synthesize MQTT ACKs in MyGames.

The desired **optional** upstream mode retains a bounded delivery token
identified by (session generation, delivery sequence), offers native receive
gate/credit, and allows an external completion after receipt+effect+cursor DB
COMMIT. Native Flowie alone owns PUBACK/PUBREC/PUBREL/PUBCOMP, outbound and
control-packet progress. Delivery with uncommitted state, stale token, DUP,
lost transport and packet-ID reuse must have deterministic results. QoS0
flooding has no unlimited lossless pause promise.

Downstream acceptance tests must use a real authenticated Broker with a
**verified durable session** and fault injection:
- crash after inbound bytes, before projection COMMIT: no successful
  disposition; durable broker redelivers or authorized archive replay repairs
  from persisted cursor;
- crash after COMMIT, before PUBACK: redelivery is REPLAY with no second effect;
- concurrent live MQTT + archive replay, duplicate, gap, forged event,
  wrong namespace, lost connection, expired/stale token, bucket FULL and
  restart all fail closed or serialize safely;
- TLS/mTLS and installable SDK integration on Linux/Windows/macOS;
- negative reason codes must not count as positive settlement.

The M4 service can proceed independently of Flowie #22, but neither
path alone entitles us to claim end-to-end exactly-once or production no-loss.

## Completion gates (all currently open except first source-reader slice)

- [x] Source archive read uses a separate TurboDB SQLite READ_ONLY connection
  and rejects attempted write / absent database in tests (qualified; new endpoint TLS is still RED).
- [ ] Implement network CHttp service with mTLS, exact principal-to-namespace
  authorization and deployed credential lifecycle.
- [ ] Implement paginated read snapshot, canonical validation, retention
  watermark and no-private-data response bounds.
- [ ] Implement per-principal rate/in-flight capacity and deferred CHttp owner
  without blocking IO; exercise overflow, restart and multi-client fairness.
- [ ] Implement supervised consumer HTTP replay using its **own persisted**
  `ConsumerProjection` cursor and serialize it with live MQTT inputs.
- [ ] RED/GREEN crash/unknown COMMIT/duplicate/gap/security tests on exact
  installed-SDK head plus three-OS matrix; attach logs.
- [ ] Qualify Flowie #22 native delayed inbound ACK separately with durable
  broker session and compatible installed SDK; no compatibility fallback.
- [ ] Real business projection, source-retention policy and on-call
  instrumentation qualified before considering PR #1 merge.

**Do not merge PR #1 for production on documentation or loopback CI alone.**
