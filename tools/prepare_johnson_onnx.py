#!/usr/bin/env python3
"""Prepare the Johnson fast-neural-style ONNX models for Pastiche.

Source: ONNX Model Zoo, validated/vision/style_transfer/fast_neural_style/model
(candy-9, mosaic-9, rain-princess-9, udnie-9, pointilism-9), which are exports
of the BSD-3 models from pytorch/examples/fast_neural_style. They declare a
fixed 1x3x224x224 input; every operator in the graph is shape-agnostic, so this
script only makes height/width symbolic, adds licence metadata and writes
models/johnson-<style>.onnx.

Usage: python tools/prepare_johnson_onnx.py <src_dir> [<dst_dir>=models]

Requires the `onnx` package only (no PyTorch).
"""
import os
import sys

import onnx
from onnx import shape_inference

STYLES = {
    "candy-9.onnx": "candy",
    "mosaic-9.onnx": "mosaic",
    "rain-princess-9.onnx": "rain_princess",
    "udnie-9.onnx": "udnie",
    "pointilism-9.onnx": "pointilism",
}

LICENSE_NOTE = (
    "BSD-3-Clause. Weights trained by the PyTorch examples authors "
    "(https://github.com/pytorch/examples/tree/main/fast_neural_style); "
    "ONNX export from the ONNX Model Zoo (https://github.com/onnx/models). "
    "Input: float32 NCHW RGB in 0..255, output same range (clamp)."
)


def make_dynamic(model: onnx.ModelProto) -> None:
    init_names = {i.name for i in model.graph.initializer}
    inputs = [i for i in model.graph.input if i.name not in init_names]
    if len(inputs) != 1 or len(model.graph.output) != 1:
        raise SystemExit("expected exactly one input and one output")
    for value in (inputs[0], model.graph.output[0]):
        dims = value.type.tensor_type.shape.dim
        if len(dims) != 4:
            raise SystemExit(f"{value.name}: expected 4 dims, got {len(dims)}")
        dims[0].dim_value = 1
        dims[1].dim_value = 3
        dims[2].ClearField("dim_value")
        dims[2].dim_param = "height"
        dims[3].ClearField("dim_value")
        dims[3].dim_param = "width"
    # Older exporters also stored the fixed shape in value_info; drop it so
    # shape inference does not complain about conflicts.
    del model.graph.value_info[:]


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    src_dir = sys.argv[1]
    dst_dir = sys.argv[2] if len(sys.argv) > 2 else "models"
    os.makedirs(dst_dir, exist_ok=True)
    for src_name, style in STYLES.items():
        src = os.path.join(src_dir, src_name)
        if not os.path.exists(src):
            print(f"skip {src_name}: not found")
            continue
        model = onnx.load(src)
        make_dynamic(model)
        model.producer_name = "pastiche/tools/prepare_johnson_onnx.py"
        model.doc_string = f"Johnson fast neural style, style '{style}'. {LICENSE_NOTE}"
        del model.metadata_props[:]
        for key, val in (("style", style), ("license", "BSD-3-Clause"),
                         ("source", "pytorch/examples fast_neural_style via onnx/models"),
                         ("input_range", "0-255"), ("output_range", "0-255")):
            entry = model.metadata_props.add()
            entry.key = key
            entry.value = val
        model = shape_inference.infer_shapes(model)
        onnx.checker.check_model(model)
        dst = os.path.join(dst_dir, f"johnson-{style}.onnx")
        onnx.save(model, dst)
        inp = [i for i in model.graph.input if i.name not in {x.name for x in model.graph.initializer}][0]
        dims = [d.dim_param or d.dim_value for d in inp.type.tensor_type.shape.dim]
        print(f"wrote {dst}  input {inp.name} {dims}  {os.path.getsize(dst) / 1e6:.1f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
