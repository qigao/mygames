"""Torch-free declaration tests for the selected KFCore/ONNX C11 Q ABI.

These fixtures validate ONLY tensor names/dynamic dims, not numerical
inference, game skill or an actual exported ONNX artifact.
"""
import sys
import hashlib
import json
import os
from pathlib import Path
import tempfile
from unittest import mock
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "ai" / "dmc"))

from export_q_onnx import (verify_onnx_signature, publish_verified_roles,
                           ROLES, no_clobber_json)


def tensor(name, dims, data_type="tensor(float)"):
    return SimpleNamespace(name=name, shape=dims, type=data_type)


def reject(fn):
    try:
        fn()
    except ValueError:
        return
    raise AssertionError("unsafe DMC ONNX declaration was admitted")


def main():
    for role in range(3):
        state = 373 if role == 0 else 484
        inputs = [tensor("z", ["batch", 5, 162]),
                  tensor("xa", ["batch", state])]
        outputs = [tensor("q", ["batch"])]
        verify_onnx_signature(inputs, outputs, role)
        verify_onnx_signature(list(reversed(inputs)), outputs, role)
        reject(lambda: verify_onnx_signature(inputs, outputs, 3))
        reject(lambda: verify_onnx_signature(
            [tensor("z", [1, 5, 162]), inputs[1]], outputs, role))
        reject(lambda: verify_onnx_signature(
            [inputs[0], tensor("xa", ["batch", state + 1])], outputs, role))
        reject(lambda: verify_onnx_signature(
            inputs, [tensor("q", ["batch", 1])], role))
        reject(lambda: verify_onnx_signature(
            inputs, [tensor("q", ["batch"], "tensor(double)")], role))
        reject(lambda: verify_onnx_signature(
            [inputs[0], tensor("z", ["batch", state])], outputs, role))
    # PUBLIC fake bytes; these are NOT model weights or an ONNX graph.
    # Exercise the complete three-role no-clobber publication contract
    # without installing Torch or creating a consumer project.
    with tempfile.TemporaryDirectory(prefix="mygames-dmc-onnx-contract-") as temp:
        root = Path(temp)
        os.chmod(root, 0o700)
        stage = root / ".staged"
        stage.mkdir(mode=0o700)
        report = {"candidate_model_version": 5, "role_packages": {}}
        for role, name in ROLES.items():
            folder = stage / name
            folder.mkdir(mode=0o700)
            model = folder / "model.onnx"
            model.write_bytes(("NOT_REAL_ONNX_ROLE_" + str(role)).encode())
            model.chmod(0o600)
            digest = hashlib.sha256(model.read_bytes()).hexdigest()
            no_clobber_json(folder, "model.json", {
                "variant": name, "version": "5",
                "artifacts": [{"sha256": digest}]})
            report["role_packages"][name] = {"onnx_sha256": digest}
        # Missing final role fails BEFORE publishing any package.
        model = stage / "landlord_down" / "model.onnx"
        saved = model.read_bytes()
        model.unlink()
        reject(lambda: publish_verified_roles(stage, root, report))
        assert not (root / "q_onnx_manifest_v1.json").exists()
        assert all(not (root / name).exists() for name in ROLES.values())
        model.write_bytes(saved)
        model.chmod(0o600)

        # A real exception midway through publication must rollback every
        # role directory, including ones already linked successfully.
        original_link = os.link
        def fault(source, destination, *args, **kwargs):
            dst = Path(destination)
            if dst.parent.name == "landlord_down" and dst.name == "model.onnx":
                raise OSError("injected private model publication failure")
            return original_link(source, destination, *args, **kwargs)
        with mock.patch("export_q_onnx.os.link", side_effect=fault):
            try:
                publish_verified_roles(stage, root, report)
            except OSError:
                pass
            else:
                raise AssertionError("injected publication failure was accepted")
        assert not (root / "q_onnx_manifest_v1.json").exists()
        assert all(not (root / name).exists() for name in ROLES.values())

        # All role manifests are completed together and published before
        # a final immutable root acceptance marker. Later overwrite fails.
        publish_verified_roles(stage, root, report)
        assert json.loads((root / "q_onnx_manifest_v1.json").read_text()) == report
        for name in ROLES.values():
            assert (root / name / "model.onnx").read_bytes() == (
                stage / name / "model.onnx").read_bytes()
        try:
            publish_verified_roles(stage, root, report)
        except FileExistsError:
            pass
        else:
            raise AssertionError("immutable three-role package was overwritten")

    print("mygames.dmc_kfcore_onnx_contract: PASS")


if __name__ == "__main__":
    main()
