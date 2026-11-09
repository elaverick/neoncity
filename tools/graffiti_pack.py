#!/usr/bin/env python3
"""graffiti_pack: turns a contact sheet of graffiti into the atlas the walls wear.

    python tools/graffiti_pack.py "<Graffiti Contact Sheet.png>" [--dest assets/textures/graffiti]

The sheet is 3072 x 1024, RGBA: two grids of 8 x 6 pieces side by side, 96 in all. The left
grid's alpha is clean (0 round each piece), its colour bled out under it. The right grid was
cut from a pale ground: its alpha never falls below about 0.3 there, and its colour is the
ground's white. So:

- Each cell is cut 4 px inside its edges (the left grid's lines between cells).
- The right grid's alpha is remapped from its ground (ALPHA_LO) up to solid (ALPHA_HI).
- Paint at a cell's edge that is small beside its piece is a neighbour's, and goes.
- Each piece is cropped to its alpha and centred, scaled to fit, in a square cell of the atlas
  (CELL px, GUTTER px clear round it), COLS x ROWS cells in reading order: the left grid, then
  the right.
- Under alpha 0 the colour is filled from the nearest paint (push-pull), so mips and bilinear
  filtering don't bleed the sheet's white or black into the edges.

Writes albedo.png (RGBA), and flat normal.png and orm.png (1 x 1): the paint takes the relief
of whatever it's sprayed on (shaders/city/surface.glsl: graffitiBand).
"""
import argparse
import os

import numpy as np
from PIL import Image
from scipy import ndimage

GRID_C, GRID_R = 8, 6        # each half of the sheet
CELL, GUTTER = 192, 6        # the atlas's square cells
COLS, ROWS = 12, 8           # 96 cells
INSET = 4                    # px cut from each sheet cell's edges
ALPHA_LO, ALPHA_HI = 0.46, 0.86   # the right grid's ground, and solid paint
KEEP = 0.04                  # alpha below this isn't part of the piece


def pieces(sheet):
    """The 96 pieces as float RGBA arrays, the left grid's first, in reading order."""
    H, W = sheet.shape[:2]
    half = W // 2
    out = []
    for side in range(2):
        for r in range(GRID_R):
            for c in range(GRID_C):
                x0 = side * half + round(c * half / GRID_C) + INSET
                x1 = side * half + round((c + 1) * half / GRID_C) - INSET
                y0 = round(r * H / GRID_R) + INSET
                y1 = round((r + 1) * H / GRID_R) - INSET
                p = sheet[y0:y1, x0:x1].copy()
                if side == 1:
                    p[..., 3] = np.clip((p[..., 3] - ALPHA_LO) / (ALPHA_HI - ALPHA_LO), 0.0, 1.0)
                p[..., 3] = np.where(p[..., 3] < KEEP, 0.0, p[..., 3])
                out.append(p)
    return out


def strays(p):
    """Clears what a neighbouring piece left in this cell: paint near the cell's edge that is
    smaller than half the piece (a grid line, the foot of the piece above; its drips and the overspray round it, merged in by a dilation, stay)."""
    lab, n = ndimage.label(ndimage.binary_dilation(p[..., 3] > 0.0, iterations=3))
    if n < 2:
        return p
    area = ndimage.sum(np.ones(lab.shape), lab, range(1, n + 1))
    E = 24   # px: what reaches this near the edge comes from over it
    edge = set(np.unique(np.concatenate([lab[:E].ravel(), lab[-E:].ravel(), lab[:, :E].ravel(), lab[:, -E:].ravel()]))) - {0}
    for k in edge:
        if area[k - 1] < 0.5 * area.max():
            p[lab == k, 3] = 0.0
    return p


def crop(p):
    ys, xs = np.nonzero(p[..., 3] > 0.0)
    if len(xs) == 0:
        return p
    return p[ys.min():ys.max() + 1, xs.min():xs.max() + 1]


def fit(p):
    """The piece scaled to fit CELL - 2 GUTTER, centred in a clear CELL square (premultiplied resize)."""
    inner = CELL - 2 * GUTTER
    h, w = p.shape[:2]
    s = min(inner / w, inner / h)
    nw, nh = max(1, round(w * s)), max(1, round(h * s))
    pm = p.copy()
    pm[..., :3] *= pm[..., 3:4]
    chans = [np.asarray(Image.fromarray(pm[..., k]).resize((nw, nh), Image.LANCZOS)) for k in range(4)]
    q = np.clip(np.stack(chans, -1), 0.0, 1.0)
    a = q[..., 3:4]
    q[..., :3] = np.where(a > 1e-4, q[..., :3] / np.maximum(a, 1e-4), 0.0)
    q[..., :3] = np.clip(q[..., :3], 0.0, 1.0)
    cell = np.zeros((CELL, CELL, 4), np.float32)
    y0, x0 = (CELL - nh) // 2, (CELL - nw) // 2
    cell[y0:y0 + nh, x0:x0 + nw] = q
    return cell


def push_pull(rgb, w):
    """rgb where w > 0, filled elsewhere from the nearest weighted colour, coarse to fine."""
    if min(rgb.shape[:2]) <= 1:
        tot = w.sum()
        fill = (rgb * w[..., None]).sum((0, 1)) / tot if tot > 0 else np.zeros(3)
        return np.broadcast_to(fill, rgb.shape).copy()
    H, W = rgb.shape[:2]
    h2, w2 = (H + 1) // 2, (W + 1) // 2
    pr = np.zeros((h2 * 2, w2 * 2, 3), np.float32); pr[:H, :W] = rgb * w[..., None]
    pw = np.zeros((h2 * 2, w2 * 2), np.float32); pw[:H, :W] = w
    sr = pr.reshape(h2, 2, w2, 2, 3).sum((1, 3))
    sw = pw.reshape(h2, 2, w2, 2).sum((1, 3))
    small = np.where(sw[..., None] > 0, sr / np.maximum(sw, 1e-6)[..., None], 0.0)
    coarse = push_pull(small, np.minimum(sw, 1.0))
    up = np.repeat(np.repeat(coarse, 2, 0), 2, 1)[:H, :W]
    k = np.clip(w, 0.0, 1.0)[..., None]
    return rgb * k + up * (1.0 - k)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('sheet')
    ap.add_argument('--dest', default=os.path.join('assets', 'textures', 'graffiti'))
    args = ap.parse_args()
    sheet = np.asarray(Image.open(args.sheet).convert('RGBA'), np.float32) / 255.0
    ps = pieces(sheet)
    assert len(ps) == COLS * ROWS
    atlas = np.zeros((ROWS * CELL, COLS * CELL, 4), np.float32)
    for i, p in enumerate(ps):
        r, c = divmod(i, COLS)
        atlas[r * CELL:(r + 1) * CELL, c * CELL:(c + 1) * CELL] = fit(crop(strays(p)))
    a = atlas[..., 3]
    atlas[..., :3] = push_pull(atlas[..., :3], (a > 0.5).astype(np.float32) + 1e-3 * a)
    os.makedirs(args.dest, exist_ok=True)
    Image.fromarray((np.clip(atlas, 0, 1) * 255 + 0.5).astype(np.uint8), 'RGBA').save(os.path.join(args.dest, 'albedo.png'), optimize=True)
    Image.new('RGB', (1, 1), (128, 128, 255)).save(os.path.join(args.dest, 'normal.png'))
    Image.new('RGB', (1, 1), (255, 255, 128)).save(os.path.join(args.dest, 'orm.png'))
    print('%d pieces -> %s (%d x %d)' % (len(ps), args.dest, atlas.shape[1], atlas.shape[0]))


if __name__ == '__main__':
    main()
