#!/usr/bin/env python3
"""vine_pack: turns the user's sheet of climbing plants into the atlas the houses' vines wear.

    python tools/vine_pack.py assets_src/vines/vines_six.png [--dest assets/textures/vine] [--preview out.png]

The sheet (residential roadmap HMv) is 1536 x 1024, RGBA: six plants side by side, each a column
about 250 px wide on a soft green ground that its alpha already cuts away (cleanly: the column's core
is solid, its ragged edges single leaves). In order: clematis, ivy, wisteria, passionflower,
bougainvillea, black-eyed Susan (the atlas keeps that order: src/gen/pmodel_retrofit.h HR_VINE_*).

- Each plant is cut at the clear gaps between them and centred in a CELL px wide cell, its outer
  EDGE px cleared, so neither filtering nor the shader's parallax reaches a neighbour.
- A plant's column doesn't tile up the wall as drawn: its top band (BAND rows) is laid over its foot
  and the two are joined along the cheapest seam across the column (the path, a row per column of
  pixels, where they differ least), so the column repeats without a visible line. That leaves
  H - BAND rows a tile, which are resampled to OUT_H (power of two, for the mips); the material's
  params give the tile's true height for the shader.
- Height (orm.b), for the vines' steep parallax (shaders/city/house.glsl shadeVine): 0 where nothing
  grows (the parallax falls through to the wall there), else the column's bulge (how far inside the
  foliage, by a distance transform), the leaves lit in the photograph standing over the dark gaps
  between them, and flowers standing out over the leaves.
- The normal comes from a softened height, so the leaves read as cupped and overlapping, not as
  terraces; occlusion (orm.r) a little deeper down; roughness (orm.g) glossy leaves, matt petals.
- Under alpha 0 the colour is the nearest leaf's, so mips don't bleed the green ground into edges.

Writes albedo.png (RGBA, the coverage in alpha), normal.png and orm.png.
"""
import argparse
import os

import numpy as np
from PIL import Image
from scipy import ndimage

PLANTS = ["clematis", "ivy", "wisteria", "passionflower", "bougainvillea", "black-eyed susan"]
CELL = 256          # px across a plant's cell
EDGE = 3            # px cleared at each side of a cell
BAND = 128          # rows of the top laid over the foot to make a column tile
OUT_H = 1024        # rows a tile is stored in
ALPHA_CUT = 0.12    # alpha under this is the ground's haze: none


def srgb_to_lin(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def plant_spans(alpha):
    """The six plants' column spans: split at the five clearest gaps between them."""
    W = alpha.shape[1]
    cover = ndimage.uniform_filter1d((alpha > 0.5).mean(axis=0), 9)
    cuts = []
    for k in range(1, len(PLANTS)):
        c = k * W // len(PLANTS)
        lo, hi = c - 60, c + 60
        cuts.append(lo + int(np.argmin(cover[lo:hi])))
    edges = [0] + cuts + [W]
    return [(edges[i], edges[i + 1]) for i in range(len(PLANTS))]


def cut_cell(img, x0, x1):
    """A plant centred (by its alpha's centre of mass) in a CELL px cell, its outer EDGE px clear."""
    H = img.shape[0]
    a = img[:, x0:x1, 3]
    cx = x0 + float((a.sum(axis=0) * np.arange(x1 - x0)).sum() / max(a.sum(), 1e-6))
    cell = np.zeros((H, CELL, 4), np.float32)
    s0 = int(round(cx - CELL / 2))
    for x in range(CELL):
        sx = s0 + x
        if x0 <= sx < x1:
            cell[:, x] = img[:, sx]
    cell[:, :EDGE, 3] = 0.0
    cell[:, CELL - EDGE:, 3] = 0.0
    return cell


def tile_vertically(cell):
    """The column made to repeat: its top BAND rows laid over its foot along the cheapest seam."""
    H, W = cell.shape[:2]
    pm = cell[..., :3] * cell[..., 3:4]
    feat = np.concatenate([pm, cell[..., 3:4]], axis=2)
    top, foot = feat[:BAND], feat[H - BAND:]
    err = np.sqrt(((top - foot) ** 2).sum(axis=2))     # BAND x W
    # the seam: a row in the band for each column, moving a row at most between neighbours, kept off
    # the band's own ends (so both images have room either side of it)
    cost = err.copy()
    cost[:8] += 1e3
    cost[BAND - 8:] += 1e3
    acc = cost.copy()
    back = np.zeros((BAND, W), np.int32)
    for x in range(1, W):
        prev = acc[:, x - 1]
        cand = np.stack([np.roll(prev, 1), prev, np.roll(prev, -1)])
        cand[0, 0] = np.inf
        cand[2, -1] = np.inf
        k = np.argmin(cand, axis=0)
        back[:, x] = np.arange(BAND) + (k - 1)
        acc[:, x] += cand[k, np.arange(BAND)]
    seam = np.zeros(W, np.int32)
    seam[-1] = int(np.argmin(acc[:, -1]))
    for x in range(W - 1, 0, -1):
        seam[x - 1] = back[seam[x], x]
    # the tile: the column less its top band; its first BAND rows the foot over the seam, the top under it
    # (so its last row runs on into its first: the foot's last row was the original's)
    out = cell[:H - BAND].copy()
    rows = np.arange(BAND)[:, None]
    over = rows < seam[None, :]                          # above the seam: the foot
    out[:BAND] = np.where(over[..., None], cell[H - BAND:], cell[:BAND])
    return out, float(err[seam, np.arange(W)].mean())


def heights(cell):
    """orm.b: 0 where nothing grows, else the column's bulge, lit leaves over dark gaps, flowers over leaves."""
    rgb, a = cell[..., :3], cell[..., 3]
    solid = a > 0.5
    lin = srgb_to_lin(rgb)
    luma = lin @ np.array([0.2126, 0.7152, 0.0722], np.float32)
    wrap = lambda f, s: ndimage.gaussian_filter(f, s, mode="wrap")
    # how far inside the foliage (the column bulges out from its ragged edges to its stems)
    inside = ndimage.distance_transform_edt(np.pad(solid, ((32, 32), (0, 0)), mode="wrap"))[32:-32]
    bulge = np.clip(inside / 22.0, 0.0, 1.0) ** 0.6
    # lit leaves stand over the dark gaps between them: the photograph's light, against its neighbourhood
    l = np.sqrt(np.maximum(luma, 0.0))
    local = l - wrap(l, 7.0)
    local = local / max(np.percentile(np.abs(local[solid]), 95), 1e-4)
    lift = np.clip(0.5 + 0.5 * local, 0.0, 1.0)
    lift = wrap(lift, 0.8)
    # flowers: saturated and not green
    mx, mn = rgb.max(axis=2), rgb.min(axis=2)
    sat = (mx - mn) / np.maximum(mx, 1e-4)
    green = rgb[..., 1] - 0.5 * (rgb[..., 0] + rgb[..., 2])
    flower = np.clip((sat - 0.25) * 3.0, 0.0, 1.0) * np.clip((0.02 - green) * 12.0 + 0.5, 0.0, 1.0)
    pale = np.clip((mn - 0.55) * 4.0, 0.0, 1.0) * np.clip(1.0 - sat * 2.5, 0.0, 1.0)   # white petals (passionflower)
    flower = wrap(np.maximum(flower, pale), 1.5)
    h = 0.12 + 0.38 * bulge + 0.36 * lift + 0.22 * flower
    h = np.where(solid, np.clip(h, 0.10, 1.0), 0.0) * np.clip((a - 0.35) / 0.3, 0.0, 1.0)
    return (h / max(np.percentile(h[solid], 99.5), 1e-4)).clip(0.0, 1.0), flower


def normals(h, a, strength):
    """Tangent-space normal (OpenGL: +y image up) from a softened height."""
    hs = ndimage.gaussian_filter(h, 1.6, mode="wrap")
    dx = (np.roll(hs, -1, 1) - np.roll(hs, 1, 1)) * 0.5
    dy = (np.roll(hs, 1, 0) - np.roll(hs, -1, 0)) * 0.5   # image up: rows run down
    n = np.stack([-dx * strength, -dy * strength, np.ones_like(h)], axis=2)
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    n[a < ALPHA_CUT] = (0.0, 0.0, 1.0)
    return n


def fill_under(rgb, a):
    """Colour under alpha 0 taken from the nearest leaf (so mips don't bleed the ground in)."""
    hole = a < 0.5
    if not hole.any() or hole.all():
        return rgb
    _, (iy, ix) = ndimage.distance_transform_edt(hole, return_indices=True)
    out = rgb[iy, ix]
    return np.where(hole[..., None], out, rgb)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sheet")
    ap.add_argument("--dest", default="assets/textures/vine")
    ap.add_argument("--preview", default=None, help="also write the atlas over black, its height and its normal side by side")
    ap.add_argument("--strength", type=float, default=9.0, help="the normal's slope per unit height per pixel")
    args = ap.parse_args()

    img = np.asarray(Image.open(args.sheet).convert("RGBA"), np.float32) / 255.0
    img[..., 3] = np.where(img[..., 3] < ALPHA_CUT, 0.0, img[..., 3])
    spans = plant_spans(img[..., 3])
    tiles, seams = [], []
    for (x0, x1), name in zip(spans, PLANTS):
        t, e = tile_vertically(cut_cell(img, x0, x1))
        tiles.append(t)
        seams.append(e)
        print(f"{name:18s} columns {x0:4d}-{x1:4d}  seam error {e:.3f}")
    tileH = tiles[0].shape[0]
    atlas = np.concatenate(tiles, axis=1)                                  # tileH x 6*CELL
    hs, fl = zip(*(heights(t) for t in tiles))
    h = np.concatenate(hs, axis=1)
    flower = np.concatenate(fl, axis=1)
    a = atlas[..., 3]
    n = normals(h, a, args.strength)
    ao = np.where(a > 0.5, 0.62 + 0.38 * np.sqrt(h), 1.0)
    rough = np.where(a > 0.5, 0.48 + 0.25 * flower, 1.0)
    rgb = fill_under(atlas[..., :3], a)

    def stored(x, mode=Image.LANCZOS):
        """A float image resampled to OUT_H rows (the tile's height in storage), 8-bit."""
        im = Image.fromarray((np.clip(x, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8))
        return im.resize((im.width, OUT_H), mode)

    os.makedirs(args.dest, exist_ok=True)
    stored(np.concatenate([rgb, a[..., None]], axis=2)).save(os.path.join(args.dest, "albedo.png"))
    stored(n * 0.5 + 0.5).save(os.path.join(args.dest, "normal.png"))
    stored(np.stack([ao, rough, h], axis=2)).save(os.path.join(args.dest, "orm.png"))
    print(f"atlas {6 * CELL} x {OUT_H} (a tile {tileH} rows as cut: {tileH / CELL:.3f} cell widths tall) -> {args.dest}")

    if args.preview:
        over = rgb * a[..., None]
        rows = [over, np.repeat(h[..., None], 3, 2), n * 0.5 + 0.5]
        twice = [np.concatenate([r, r], axis=0) for r in rows]             # two tiles up: the seam shows if it's there
        Image.fromarray((np.concatenate(twice, axis=1) * 255).astype(np.uint8)).save(args.preview)


if __name__ == "__main__":
    main()
