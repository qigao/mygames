"""Strict, standard-library-only reader for MyGames PRIVATE DMC Replay v1.

The format is written by the authoritative pure-C11 DMCReplay codec after a
complete offline match. It is NOT an authentication/signature mechanism and
must not be used with untrusted player uploads. Never log raw feature rows.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import math
import struct
import zlib

MAGIC = b"MGDMC001"
HEADER = struct.Struct("<8sIIQQIIIBBHIIQ")
RECORD = struct.Struct("<QQBBHId")
TRAILER = struct.Struct("<I")
HEADER_BYTES = 64
RECORD_BYTES = 1326
MAX_STEPS = 162
MAX_BYTES = HEADER_BYTES + MAX_STEPS * RECORD_BYTES + TRAILER.size
HISTORY_FEATURES = 810
MAX_STATE_FEATURES = 430
ACTION_FEATURES = 54
OBJECTIVE_WIN_SIGN = 1
OBJECTIVE_GAME_POINTS = 2


@dataclass(frozen=True)
class Sample:
    turn_id: int
    chosen_cards: int
    actor_seat: int
    role: int
    x: bytes
    z: bytes
    action: bytes
    target: float


@dataclass(frozen=True)
class Episode:
    episode_id: int
    model_version: int
    rules_version: int
    objective: int
    landlord_seat: int
    winner_seat: int
    samples: tuple[Sample, ...]


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(f"invalid private DMC replay: {message}")


def _expected_action(mask: int) -> bytes:
    _require(mask < 1 << 54, "chosen physical mask outside 54 cards")
    counts = [0] * 15
    for card in range(54):
        if mask & (1 << card):
            counts[card // 4 if card < 52 else card - 39] += 1
    result = bytearray(54)
    for rank in range(13):
        _require(counts[rank] <= 4, "invalid physical multiplicity")
        for i in range(counts[rank]):
            result[4 * rank + i] = 1
    _require(counts[13] <= 1 and counts[14] <= 1, "duplicate joker")
    result[52], result[53] = counts[13], counts[14]
    return bytes(result)


def _legal_unit(value: float) -> bool:
    return any(value == base * (2**k) for base in (1, 2, 3) for k in range(16))


def parse_episode(data: bytes) -> Episode:
    _require(HEADER_BYTES + TRAILER.size <= len(data) <= MAX_BYTES, "byte length")
    (
        magic, version, flags, episode_id, model_version, rules_version,
        objective, steps, landlord, winner, reserved, record_bytes,
        payload_bytes, reserved64
    ) = HEADER.unpack_from(data, 0)
    _require(magic == MAGIC and version == 1 and flags == 1, "header/version")
    _require(reserved == 0 and reserved64 == 0, "reserved header")
    _require(episode_id > 0 and model_version > 0 and rules_version > 0, "source version")
    _require(objective in (OBJECTIVE_WIN_SIGN, OBJECTIVE_GAME_POINTS), "reward objective")
    _require(landlord in range(3) and winner in range(3), "seat")
    _require(1 <= steps <= MAX_STEPS and record_bytes == RECORD_BYTES, "record count/size")
    _require(payload_bytes == steps * RECORD_BYTES, "payload byte count")
    _require(len(data) == HEADER_BYTES + payload_bytes + TRAILER.size, "exact size")
    (expected_crc,) = TRAILER.unpack_from(data, len(data) - TRAILER.size)
    _require(
        (zlib.crc32(data[:-TRAILER.size]) & 0xFFFFFFFF) == expected_crc,
        "CRC32 (detects corruption, not authenticity)"
    )

    first_target = RECORD.unpack_from(data, HEADER_BYTES)[-1]
    unit = 1.0 if objective == OBJECTIVE_WIN_SIGN else abs(first_target) / 2.0
    _require(math.isfinite(unit) and
             (objective == OBJECTIVE_WIN_SIGN or _legal_unit(unit)), "point unit")

    samples: list[Sample] = []
    for i in range(steps):
        start = HEADER_BYTES + i * RECORD_BYTES
        turn, cards, actor, role, x_count, unused, target = RECORD.unpack_from(data, start)
        _require(turn == i + 1 and actor in range(3) and unused == 0, "turn/actor")
        expected_role = (0 if actor == landlord else
                         2 if actor == (landlord + 1) % 3 else 1)
        _require(role == expected_role, "role-to-seat mapping")
        expected_x = 319 if role == 0 else 430
        _require(x_count == expected_x, "DouZero feature shape")
        begin = start + RECORD.size
        x_full = data[begin:begin + MAX_STATE_FEATURES]
        z = data[begin + MAX_STATE_FEATURES:
                 begin + MAX_STATE_FEATURES + HISTORY_FEATURES]
        action = data[begin + MAX_STATE_FEATURES + HISTORY_FEATURES:
                      begin + MAX_STATE_FEATURES + HISTORY_FEATURES + ACTION_FEATURES]
        _require(len(action) == ACTION_FEATURES and
                 all(v in (0, 1) for v in x_full + z + action), "feature binary values")
        _require(all(v == 0 for v in x_full[expected_x:]), "unused x padding")
        _require(action == _expected_action(cards), "physical card/action mismatch")
        _require(math.isfinite(target), "nonfinite return")
        win_team = ((actor == landlord) == (winner == landlord))
        if objective == OBJECTIVE_WIN_SIGN:
            expected_target = 1.0 if win_team else -1.0
        else:
            sign = 1.0 if winner == landlord else -1.0
            expected_target = (sign * 2 * unit if actor == landlord else -sign * unit)
        _require(target == expected_target, "actor-specific reward mismatch")
        samples.append(Sample(turn, cards, actor, role,
                              x_full[:expected_x], z, action, target))
    _require(samples[0].actor_seat == landlord, "first turn not landlord")
    _require(samples[-1].actor_seat == winner, "final actor not winner")
    return Episode(episode_id, model_version, rules_version,
                   objective, landlord, winner, tuple(samples))


def read_episode(path: str | Path) -> Episode:
    p = Path(path)
    _require(p.is_file(), "replay file missing")
    _require(p.stat().st_size <= MAX_BYTES, "file over maximum")
    return parse_episode(p.read_bytes())
