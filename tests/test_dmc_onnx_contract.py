"""Torch-free declaration tests for the selected KFCore/ONNX C11 Q ABI.

These fixtures validate ONLY tensor names/dynamic dims, not numerical
inference, game skill or an actual exported ONNX artifact.
"""
import sys
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "ai" / "dmc"))

from export_q_onnx import verify_onnx_signature


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
    print("mygames.dmc_kfcore_onnx_contract: PASS")


if __name__ == "__main__":
    main()
