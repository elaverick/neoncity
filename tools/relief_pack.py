#!/usr/bin/env python3
"""relief_pack: puts a material's parallax height into its normal map's alpha (parallax depth roadmap, PD0).

    python tools/relief_pack.py MATERIAL [--highpass F] [--lo P] [--hi P]

MATERIAL names assets/materials/MATERIAL.json. Its height map (orm.b, as tools/pbr_pack.c wrote it from the
download's displacement) becomes the relief that pbrGrainRelief (shaders/pbr.glsl) steps through, written into
the alpha of the material's normal.png (whose RGB stays as it is), where:

- 1 is the relief's tops and 0 its hollows: the map is stretched so its P-th percentile (--lo, default 0.5)
  lands on 0 and its P-th (--hi, default 99.5) on 1. ambientCG's displacements rarely span the whole range
  (a brick's tops sit at 0.76), and the parallax counts down from 1, so unstretched the whole surface would sit
  sunk under its polygon and slide as the eye moves.
- --highpass F takes off the broad bulge first: the height less its blur over F of the image (a wrapped box
  filter, three passes: near a Gaussian), for sets whose displacement is mostly low swells (paving, concrete,
  roughcast), which parallax would turn into whole slabs swimming. 0 (the default) keeps it whole.

The material's file then wants "depth": metres from the tops to the hollows. orm.b is left alone (the ground's
wet relief and the snow read it). Prints the height's spread before and after.
"""
import argparse
import json
import os
import re
import sys

import numpy as np
from PIL import Image
from scipy import ndimage

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_def(name):
    path = os.path.join(ROOT, "assets", "materials", name + ".json")
    text = re.sub(r"//[^\n]*", "", open(path, encoding="utf-8").read())
    d = json.loads(text)
    base = os.path.dirname(path)
    return {k: os.path.normpath(os.path.join(base, v)) for k, v in d["maps"].items()}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("material")
    ap.add_argument("--highpass", type=float, default=0.0)
    ap.add_argument("--lo", type=float, default=0.5)
    ap.add_argument("--hi", type=float, default=99.5)
    a = ap.parse_args()
    maps = load_def(a.material)
    h = np.asarray(Image.open(maps["orm"]).convert("RGB"), dtype=np.float64)[:, :, 2] / 255.0
    sd0 = h.std()
    if a.highpass > 0.0:
        size = max(3, int(round(a.highpass * h.shape[0])))
        lo = h
        for _ in range(3):
            lo = ndimage.uniform_filter(lo, size=size, mode="wrap")
        h = h - lo
    p0, p1 = np.percentile(h, [a.lo, a.hi])
    if p1 - p0 < 1e-4:
        sys.exit(f"{a.material}: its height map is flat")
    r = np.clip((h - p0) / (p1 - p0), 0.0, 1.0)
    nrm = Image.open(maps["normal"]).convert("RGB")
    if nrm.size != (r.shape[1], r.shape[0]):
        r = np.asarray(Image.fromarray((r * 65535).astype(np.uint16)).resize(nrm.size, Image.BILINEAR), dtype=np.float64) / 65535.0
    out = Image.merge("RGBA", (*nrm.split(), Image.fromarray(np.round(r * 255).astype(np.uint8))))
    out.save(maps["normal"])
    print(f"{a.material}: {os.path.relpath(maps['normal'], ROOT)} alpha <- orm.b"
          f"{f' less its blur over {a.highpass:g} of the image' if a.highpass else ''}, stretched {p0:.3f}..{p1:.3f} to 0..1;"
          f" spread {sd0:.3f} -> {r.std():.3f}, median {np.median(r):.2f}")


if __name__ == "__main__":
    main()
