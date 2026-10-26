# Room checkpoint v1 (pure C11 data codec)

This is an **internal, process-independent byte representation**, not a file
format users should edit and not a protocol a client may send. Code lives in
`games/doudizhu/src/room_checkpoint.c` and never opens files, sockets, or
storage handles. The transport-neutral codec is intended for a future
transactional TurboDB adapter, not as a substitute for that adapter.

## Canonical byte layout (little-endian)

The checkpoint has a 32-byte header followed by 0..32 fixed-size, 88-byte
room records. There is no implicit C struct padding or native-endian data.

| Bytes | Field |
|---|---|
| 0..3 | ASCII magic `MGR1` |
| 4 | schema version = 1 |
| 5 | populated room count, 0..32 |
| 6..7 | reserved, zero |
| 8..15 | next monotonic room serial (uint64 LE) |
| 16..23 | room owner epoch (uint64 LE; nonzero) |
| 24..27 | FNV-1a 32-bit over record payload, LE |
| 28..31 | reserved, zero |

Each room record:

| Offset | Field |
|---|---|
| +0..39 | NUL-padded canonical `room-N` ASCII id |
| +40 | `room_phase` (WAITING or STARTING) |
| +41 | number of seated players, 1..3 |
| +42..43 | reserved, zero |
| +44..51 | owner player id (uint64 LE) |
| +52..59 | room revision (uint64 LE, nonzero) |
| +60..83 | three seat player IDs (three uint64 LE) |
| +84 | seat-ready bitmask, only low three bits |
| +85..87 | reserved, zero |

No session resume token, client connection handle or other secret is serialized.

## Validation and failure atomicity

The decoder rejects unknown versions, truncated/trailing bytes, nonzero
reserved bytes, checksum failure, invalid room IDs, repeated room IDs, repeated
player memberships across rooms, a missing owner seat, noncanonical NUL
padding, invalid ready bits or a mismatched player count. The output directory
and output epoch are unchanged unless the **entire** checkpoint validates.

The encoder checks the source directory and constructs bytes in a temporary
bounded stack buffer. It does not emit a partial checkpoint. A capacity error
only reports the required byte count; the caller's bytes remain untouched.

The FNV-1a value provides **accidental corruption detection**, not
authentication. Production snapshots must use a storage-layer integrity and
authorization boundary.

## What checkpoint v1 does NOT deliver

- No durable write/read path, TurboDB schema/transaction, WAL or process
  recovery: these are still **unimplemented**.
- No atomic business command outcome / outbox transaction.
- No authenticated principal or connection recovery.
- No cross-host owner election or lease; `room_epoch` is an explicit value
  supplied by the caller and must be advanced by the durable owner authority.
- No cards, gameplay, bidding or final settlement state; this schema covers
  the current lobby/room lifecycle **only**.

Do not merge an owner recovery feature without a test that stores this
checkpoint in the selected durable repository, crashes/restarts the process,
loads a strictly verified snapshot, replays committed events and fences an old
owner epoch.
