#!/usr/bin/env python3
"""Build the AdaIN encoder / decoder ONNX models from the PyTorch weights of
naoto0804/pytorch-AdaIN (MIT) without PyTorch.

  encoder: vgg_normalised.pth, layers 0..30 of net.vgg (up to relu4_1)
           input  image    float32 [1,3,H,W], RGB in 0..1
           output features float32 [1,512,ceil(H/8),ceil(W/8)]
  decoder: decoder.pth, net.decoder
           input  features float32 [1,512,h,w]
           output image    float32 [1,3,8h,8w], RGB in 0..1 (unclamped)

The AdaIN operation itself is not part of either graph; Pastiche applies it in
C++ between the two (with statistics over the whole image, see DECYZJE.md D5).

Usage: python tools/prepare_adain_onnx.py <dir with decoder.pth and vgg_normalised.pth> [models]
Requires: numpy, onnx (no torch).
"""
import os
import sys

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper, shape_inference

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import torch_legacy  # noqa: E402

OPSET = 13

# Layer lists mirror net.py. ("conv", index, cin, cout, k), ("pad",), ("relu",),
# ("pool",), ("up",). The index is the position in the nn.Sequential and thus
# the key prefix in the state dict ("<index>.weight" / "<index>.bias").
VGG_TO_RELU4_1 = [
    ("conv", 0, 3, 3, 1), ("pad",), ("conv", 2, 3, 64, 3), ("relu",),
    ("pad",), ("conv", 5, 64, 64, 3), ("relu",), ("pool",),
    ("pad",), ("conv", 9, 64, 128, 3), ("relu",),
    ("pad",), ("conv", 12, 128, 128, 3), ("relu",), ("pool",),
    ("pad",), ("conv", 16, 128, 256, 3), ("relu",),
    ("pad",), ("conv", 19, 256, 256, 3), ("relu",),
    ("pad",), ("conv", 22, 256, 256, 3), ("relu",),
    ("pad",), ("conv", 25, 256, 256, 3), ("relu",), ("pool",),
    ("pad",), ("conv", 29, 256, 512, 3), ("relu",),
]

DECODER = [
    ("pad",), ("conv", 1, 512, 256, 3), ("relu",), ("up",),
    ("pad",), ("conv", 5, 256, 256, 3), ("relu",),
    ("pad",), ("conv", 8, 256, 256, 3), ("relu",),
    ("pad",), ("conv", 11, 256, 256, 3), ("relu",),
    ("pad",), ("conv", 14, 256, 128, 3), ("relu",), ("up",),
    ("pad",), ("conv", 18, 128, 128, 3), ("relu",),
    ("pad",), ("conv", 21, 128, 64, 3), ("relu",), ("up",),
    ("pad",), ("conv", 25, 64, 64, 3), ("relu",),
    ("pad",), ("conv", 28, 64, 3, 3),
]


class GraphBuilder:
    def __init__(self, prefix):
        self.prefix = prefix
        self.nodes = []
        self.inits = []
        self.n = 0
        self._const_cache = {}

    def name(self, kind):
        self.n += 1
        return f"{self.prefix}_{kind}_{self.n}"

    def const(self, key, array):
        if key in self._const_cache:
            return self._const_cache[key]
        name = f"{self.prefix}_c_{key}"
        self.inits.append(numpy_helper.from_array(array, name))
        self._const_cache[key] = name
        return name

    def add(self, op, inputs, attrs=None, kind=None):
        out = self.name(kind or op.lower())
        self.nodes.append(helper.make_node(op, inputs, [out], name=out, **(attrs or {})))
        return out

    def sequential(self, layers, sd, x):
        for layer in layers:
            kind = layer[0]
            if kind == "conv":
                _, idx, cin, cout, k = layer
                w = np.ascontiguousarray(sd[f"{idx}.weight"], dtype=np.float32)
                b = np.ascontiguousarray(sd[f"{idx}.bias"], dtype=np.float32)
                assert w.shape == (cout, cin, k, k), (idx, w.shape)
                assert b.shape == (cout,), (idx, b.shape)
                wn, bn = f"{self.prefix}_w{idx}", f"{self.prefix}_b{idx}"
                self.inits.append(numpy_helper.from_array(w, wn))
                self.inits.append(numpy_helper.from_array(b, bn))
                x = self.add("Conv", [x, wn, bn], {"kernel_shape": [k, k], "pads": [0, 0, 0, 0], "strides": [1, 1]},
                             kind=f"conv{idx}")
            elif kind == "pad":
                pads = self.const("pads1", np.array([0, 0, 1, 1, 0, 0, 1, 1], dtype=np.int64))
                x = self.add("Pad", [x, pads], {"mode": "reflect"})
            elif kind == "relu":
                x = self.add("Relu", [x])
            elif kind == "pool":
                x = self.add("MaxPool", [x], {"kernel_shape": [2, 2], "strides": [2, 2], "ceil_mode": 1})
            elif kind == "up":
                scales = self.const("scales2", np.array([1, 1, 2, 2], dtype=np.float32))
                x = self.add("Resize", [x, "", scales],
                             {"mode": "nearest", "nearest_mode": "floor", "coordinate_transformation_mode": "asymmetric"})
            else:
                raise ValueError(kind)
        return x


def build(prefix, layers, sd, in_name, in_shape, out_name, out_shape, doc):
    g = GraphBuilder(prefix)
    y = g.sequential(layers, sd, in_name)
    # rename the final output
    g.nodes[-1].output[0] = out_name
    graph = helper.make_graph(
        g.nodes, f"adain_{prefix}",
        [helper.make_tensor_value_info(in_name, TensorProto.FLOAT, in_shape)],
        [helper.make_tensor_value_info(out_name, TensorProto.FLOAT, out_shape)],
        initializer=g.inits, doc_string=doc)
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", OPSET)],
                              producer_name="pastiche/tools/prepare_adain_onnx.py")
    model.ir_version = 8
    for key, val in (("license", "MIT"), ("source", "naoto0804/pytorch-AdaIN (weights), Huang & Belongie 2017"),
                     ("input_range", "0-1"), ("output_range", "0-1")):
        entry = model.metadata_props.add()
        entry.key, entry.value = key, val
    model = shape_inference.infer_shapes(model)
    onnx.checker.check_model(model)
    return model


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else "models"
    os.makedirs(dst, exist_ok=True)

    vgg = torch_legacy.load(os.path.join(src, "vgg_normalised.pth"))
    dec = torch_legacy.load(os.path.join(src, "decoder.pth"))

    enc_model = build("enc", VGG_TO_RELU4_1, vgg, "image", [1, 3, "height", "width"],
                      "features", [1, 512, "fh", "fw"],
                      "VGG-19 (normalised) up to relu4_1 for AdaIN. Input RGB 0..1.")
    dec_model = build("dec", DECODER, dec, "features", [1, 512, "fh", "fw"],
                      "image", [1, 3, "height", "width"],
                      "AdaIN decoder. Output RGB 0..1 (unclamped).")
    for name, model in (("adain-encoder.onnx", enc_model), ("adain-decoder.onnx", dec_model)):
        path = os.path.join(dst, name)
        onnx.save(model, path)
        n_params = sum(int(np.prod(t.dims)) for t in model.graph.initializer)
        print(f"wrote {path}: {len(model.graph.node)} nodes, {n_params / 1e6:.2f} M params, {os.path.getsize(path) / 1e6:.1f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
