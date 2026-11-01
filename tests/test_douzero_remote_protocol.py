#!/usr/bin/env python3
"""Pure-stdlib schema alignment for optional CHttp HTTP/2 and WS inference.

This is a DATA contract test only. It does not pretend a remote C++ service,
authenticated TLS, or real original-trained model is already running.
"""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
p = json.loads((ROOT / "ai/douzero/inference-protocol.v1.json").read_text())
h = (ROOT / "ai/douzero/include/mygames/douzero_remote_ticket.h").read_text()
c = (ROOT / "ai/douzero/src/douzero_remote_ticket.c").read_text()
assert p["schema"] == "mygames.douzero.inference/1"
assert p["method"] == "douzero.q.evaluate"
assert p["objective"] == "wp"
assert p["transports"] == ["http2-jsonrpc-2.0", "websocket"]
assert p["source"]["kfcore_original_profile"] == "douzero.original.wp"
assert p["source"]["legacy_profile_not_accepted"] == "ddz-dmc-replay-v1"
assert p["limits"]["max_actions_per_chunk"] == 64
assert p["limits"]["max_full_legal_actions"] == 16384
assert p["limits"]["max_chunks_per_ticket"] == 256
assert p["limits"]["max_pending_per_turn"] == 1
assert p["limits"]["x_landlord"] == 319
assert p["limits"]["x_farmer"] == 430
assert p["limits"]["history"] == 810
assert p["limits"]["action_width"] == 54
assert p["limits"]["max_request_body_bytes"] >= 32768
assert len(p["required_request"]) == len(set(p["required_request"]))
assert len(p["required_response"]) == len(set(p["required_response"]))
request = set(p["required_request"])
response = set(p["required_response"])
identity = {
    "protocol_version", "ticket_id", "request_id_base", "game_id",
    "room_generation", "owner_epoch", "transport_generation", "turn_id",
    "seat", "landlord_seat", "role", "model_version", "model_sha256",
    "rules_profile", "objective", "legal_list_sha256",
    "legal_action_count", "chunk_index", "chunk_count",
}
assert identity <= request and identity <= response
assert {"request_id", "x", "z", "actions", "deadline_ms"} <= request
assert {"request_id", "scores"} <= response
assert not {"actions", "x", "z", "deadline_ms"} & response
assert p["json_encoding"]["u64_fields"].startswith("canonical unsigned decimal")
assert p["legal_list_digest"]["algorithm"] == "SHA-256"
assert p["legal_list_digest"]["owner_computed"]
assert p["legal_list_digest"]["service_echo_only"]
rules = p["rules"]
assert rules["owner_authoritative"] and rules["owner_must_revalidate_move_at_commit"]
assert rules["remote_reply_never_is_game_ack"]
assert rules["never_auto_pass_on_timeout"] and not rules["automatic_fallback"]
assert rules["legacy_model_profile_forbidden"]
assert not p["staged_contract"]["live_chttp_h2_transport_implemented"]
assert not p["staged_contract"]["live_ws_transport_implemented"]
assert not p["staged_contract"]["cpp_service_implemented"]
assert not p["staged_contract"]["production_game_owner_commit_hook_implemented"]
assert not p["staged_contract"]["full_original_legal_actions_qualified"]
assert "MYGAMES_DOUZERO_REMOTE_CHUNK_ACTIONS 64u" in h
assert "MYGAMES_DOUZERO_REMOTE_MAX_ACTIONS 16384u" in h
assert "MYGAMES_DOUZERO_REMOTE_SHA256_BYTES 32u" in h
assert "MYGAMES_DOUZERO_REMOTE_PROFILE_ORIGINAL_WP_V1" in h
assert "mygames_douzero_remote_best_index" in c
assert "mygames_douzero_remote_cancel" in c
assert "mygames_ddz_round_submit" not in c
assert "kfcore" not in c.lower() and "chttp" not in c.lower()
print("mygames.douzero_remote_protocol_contract: PASS "
      "H2/WS owner ticket only; no remote transport/deployed model claimed")
