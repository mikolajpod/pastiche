#!/usr/bin/env python3
"""Compare images pixel by pixel.

Used to put numbers on questions that are otherwise answered with opinion, such
as how much a quantised model differs from the full-precision one. Reports the
same statistics as --selftest does for the ONNX path (mean and max absolute
difference in 8-bit levels) so the two are directly comparable.

    python tools/compare_images.py reference.png candidate.png [more.png ...]

Needs numpy and Pillow, so run it with the system Python rather than the MSYS2
one that a build shell puts first on PATH.

A warning about reading the output. For a feed-forward algorithm these numbers
mean what they look like: the same input must give the same picture, so any
real difference is a bug. For diffusion they do not. Sampling is iterative and
chaotic, so a difference far below the precision of the weights still steers
the trajectory to a different, equally valid image. Measured here, the same
model at the same precision on CPU versus Vulkan differs more than f32 versus
q8_0 on the same backend. Use these figures to compare how closely variants
track a reference, never as a quality score.
"""

import sys

import numpy as np
from PIL import Image


def load(path):
    img = Image.open(path).convert("RGB")
    return np.asarray(img).astype(np.int16)


def compare(reference, candidate, ref_path, cand_path):
    if reference.shape != candidate.shape:
        print(f"{cand_path}: size mismatch {candidate.shape} vs {reference.shape}")
        return

    diff = np.abs(candidate - reference)
    mean = diff.mean()
    peak = diff.max()
    # Share of pixels where any channel is off by more than one level; a
    # difference of one is rounding noise and says nothing.
    visible = (diff.max(axis=2) > 1).mean() * 100.0

    # PSNR, the usual way of reporting this; infinite for identical images.
    mse = (diff.astype(np.float64) ** 2).mean()
    psnr = float("inf") if mse == 0 else 10.0 * np.log10(255.0 * 255.0 / mse)

    print(
        f"{cand_path}\n"
        f"    vs {ref_path}\n"
        f"    mean diff {mean:6.2f}/255   max {peak:3d}   "
        f"pixels off by >1: {visible:5.1f}%   PSNR {psnr:.1f} dB"
    )


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2

    ref_path = argv[1]
    reference = load(ref_path)
    for cand_path in argv[2:]:
        compare(reference, load(cand_path), ref_path, cand_path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
