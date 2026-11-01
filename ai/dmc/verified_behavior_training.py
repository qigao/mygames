"""Fail-closed admission of C11 multi-behavior TRAIN and TUNING cohorts.

Never reads the TEST pool: a model must be frozen before a fresh held-out
evaluation. Provenance digests record exact bytes but are not signatures.
Only the source-verified mapping may be passed to the policy-balanced learner.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path

from replay import parse_episode
from train_policy import make_role_sampler
from verify_behavior_sources import (
    BEHAVIORS, HEX40, HEX64, MANIFEST, require_private, verify_cohort,
)


def _roster(value):
    if (not isinstance(value, (list, tuple)) or
            not 2 <= len(value) <= len(BEHAVIORS) or
            len(set(value)) != len(value) or
            not set(value).issubset(BEHAVIORS)):
        raise ValueError("explicit 2..3 distinct known C11 behavior sources required")
    return frozenset(value)


def admit_training_sources(*, train_cohorts, validation_cohorts,
                           required_behaviors, source_revision: str,
                           generator_sha256: str, objective: str):
    """Return exact private file paths and model-version-INDEPENDENT sources.

    All source/role/outcome coverage is validated before any PyTorch import.
    Output mappings are process-private; never dump episode IDs/paths into CI.
    """
    required = _roster(required_behaviors)
    if (not isinstance(source_revision, str) or
            HEX40.fullmatch(source_revision) is None or
            not isinstance(generator_sha256, str) or
            HEX64.fullmatch(generator_sha256) is None or
            objective != "win-sign"):
        raise ValueError("exact source/binary SHAs and win-sign objective required")
    train = [Path(x).absolute() for x in train_cohorts]
    validation = [Path(x).absolute() for x in validation_cohorts]
    if len(train) != len(required) or len(validation) != len(required):
        raise ValueError("each behavior source needs TRAIN and TUNING cohorts")
    if len({x.resolve(strict=True) for x in train + validation}) != len(
            train + validation):
        raise ValueError("source cohort root reused across pools")

    seen_ids, seen_hashes = set(), set()
    bundles, source_by_episode, files, all_episodes = {}, {}, {}, {}
    digest_inputs = []
    for split, roots in (("train", train), ("validation", validation)):
        present, local_labels, local_files, episodes = set(), {}, [], []
        for root in roots:
            behavior, verified_split, ids, hashes, _ = verify_cohort(
                root, source_revision=source_revision,
                generator_sha256=generator_sha256, objective=objective)
            if (verified_split != split or behavior not in required or
                    behavior in present or seen_ids & ids or seen_hashes & hashes):
                raise ValueError("mislabeled source/split, duplication or leak")
            present.add(behavior)
            seen_ids.update(ids)
            seen_hashes.update(hashes)
            marker = root / MANIFEST
            require_private(marker, directory=False)
            # The source manifest was already verified by verify_cohort.
            # Its exact bytes/digest bind the private checkpoint report.
            content = marker.read_bytes()
            data = json.loads(content)
            digest_inputs.append((split, behavior,
                                  hashlib.sha256(content).hexdigest()))
            for entry in data["episodes"]:
                private_file = root / entry["file"]
                require_private(private_file, directory=False)
                # Capture and VERIFY the exact bytes once, then parse those
                # very bytes into an in-memory immutable Episode. Passing only
                # paths to a later learner would allow an owner-side rewrite
                # between provenance admission and optimizer input reading.
                raw = private_file.read_bytes()
                if hashlib.sha256(raw).hexdigest() != entry["sha256"]:
                    raise ValueError("native Replay changed during source admission")
                game = parse_episode(raw)
                if (game.episode_id != entry["episode_id"] or
                        game.model_version != 1 or game.rules_version != 1 or
                        game.landlord_seat != entry["landlord_seat"] or
                        game.winner_seat != entry["winner_seat"]):
                    raise ValueError("source snapshot differs from sealed manifest")
                local_files.append(private_file)
                episodes.append(game)
                local_labels[game.episode_id] = behavior
        if present != required:
            raise ValueError("TRAIN/VALIDATION source rosters differ")
        # Check source AND actor reward-sign coverage in each split, BEFORE
        # importing Torch or allocating model weights; one source with only
        # winning or only losing games is not eligible.
        if set(local_labels) != {e.episode_id for e in episodes}:
            raise ValueError("source-to-episode attribution not exhaustive")
        for role in (0, 1, 2):
            make_role_sampler(
                episodes, role, verified_source_by_episode=local_labels)
        bundles[split] = len(local_files)
        files[split] = tuple(local_files)
        source_by_episode[split] = local_labels
        all_episodes[split] = tuple(episodes)
    bundle_hash = hashlib.sha256(
        "\n".join(f"{s}:{b}:{h}" for s, b, h in sorted(digest_inputs))
        .encode("ascii")).hexdigest()
    return {
        "train_files": files["train"],
        "validation_files": files["validation"],
        "train_episodes": all_episodes["train"],
        "validation_episodes": all_episodes["validation"],
        "train_source_by_episode": source_by_episode["train"],
        "validation_source_by_episode": source_by_episode["validation"],
        "sources": tuple(sorted(required)),
        "manifest_bundle_sha256": bundle_hash,
        "episodes_by_split": bundles,
        "source_provenance_not_cryptographic_attestation": True,
        "qualified_gameplay_skill": False,
    }


def exact_replay_paths(paths, expected):
    """Do not allow a caller to smuggle unverified TEST or foreign Replay."""
    actual_paths = [Path(x).absolute() for x in paths]
    verified_paths = [Path(x).absolute() for x in expected]
    if (len(actual_paths) != len(verified_paths) or
            len({p.resolve(strict=True) for p in actual_paths}) !=
            len(actual_paths) or
            set(p.resolve(strict=True) for p in actual_paths) !=
            set(p.resolve(strict=True) for p in verified_paths)):
        raise ValueError("optimizer Replay list differs from audited TRAIN/VALIDATION")
