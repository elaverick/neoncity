#!/usr/bin/env python3
"""blind_pack: turns a picture of one venetian blind into the two textures the windows wear.

    python tools/blind_pack.py <source dir> [--dest assets/textures]

The source holds albedo.png, normal.png, orm.png and mask.png (white where the blind is
see-through, whose albedo is keyed magenta): one blind of about eleven slats, with a pull
cord and a bottom rail. It isn't a tile, so it makes two:

    blind       seven whole slats, cut mid-gap, that repeat up and across
    blind_rail  what's below them: the last slats, the rail and the cord's clip

Both are cut where the blind is see-through, so only the cord crosses the join.

- Across: the last columns are cross-faded into the first, so the image repeats. The cord
  goes to the middle.
- The mask becomes the albedo's alpha, inverted (1 is slat) and tightened to clean edges.
- The magenta and the other maps' contents in the gaps are replaced by the nearest slat's,
  so mips don't bleed them into the slats.
"""
import argparse
import json
import os

import numpy as np
from PIL import Image

ROW_TOP, ROW_SPLIT, ROW_END = 130, 740, 960   # mid-gap, mid-gap, and past the rail
PERIODS = 7                                   # slats between ROW_TOP and ROW_SPLIT
PITCH_M = 0.055                               # metres a slat, gap included
BLEND = 96                                    # columns cross-faded across the repeat
OUT_W = 1024


def load(src, name, mode):
    return np.asarray(Image.open(os.path.join(src, name + '.png')).convert(mode), np.float32) / 255.0


def fill_gaps(img, solid):
    """Each column's see-through rows take the nearest solid row's value above or below."""
    out = img.copy()
    H, W = solid.shape
    rows = np.arange(H)
    for x in range(W):
        ok = np.flatnonzero(solid[:, x])
        if len(ok) == 0:
            continue
        pos = np.clip(np.searchsorted(ok, rows), 0, len(ok) - 1)
        below, above = ok[pos], ok[np.maximum(pos - 1, 0)]
        near = np.where(np.abs(rows - above) <= np.abs(rows - below), above, below)
        out[:, x] = img[near, x]
    return out


def repeat_across(img):
    """Cross-fades the last BLEND columns into the first, for a width of W - BLEND."""
    W = img.shape[1]
    keep = W - BLEND
    out = img[:, :keep].copy()
    t = ((np.arange(BLEND) + 0.5) / BLEND).reshape((1, BLEND) + (1,) * (img.ndim - 2))
    out[:, :BLEND] = img[:, :BLEND] * t + img[:, keep:] * (1.0 - t)
    return out


def resize(img, size, wrap_y):
    """A resize that wraps across, and up too for an image that repeats that way."""
    w, h = size
    ny = 3 if wrap_y else 1
    tiled = np.tile(img, (ny, 3) + (1,) * (img.ndim - 2))
    channels = []
    for c in range(img.shape[2]):
        im = Image.fromarray(tiled[:, :, c], 'F').resize((w * 3, h * ny), Image.LANCZOS)
        channels.append(np.asarray(im))
    out = np.clip(np.stack(channels, axis=2), 0.0, 1.0)
    y0 = h if wrap_y else 0
    return out[y0:y0 + h, w:w * 2]


def save(path, img):
    mode = {3: 'RGB', 4: 'RGBA'}[img.shape[2]]
    Image.fromarray((img * 255.0 + 0.5).astype(np.uint8), mode).save(path, optimize=True)


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('--dest', default='assets/textures')
    a = ap.parse_args()

    albedo = load(a.src, 'albedo', 'RGB')
    normal = load(a.src, 'normal', 'RGB')
    orm = load(a.src, 'orm', 'RGB')
    mask = load(a.src, 'mask', 'L')

    # The mask's slats are a little noisy (0-0.1) and so are its gaps: tighten both. A slat's
    # edge pixel can carry magenta in the albedo, so the colours come from further in.
    alpha = np.clip((0.75 - mask) / 0.5, 0.0, 1.0)
    key = (albedo[:, :, 0] + albedo[:, :, 2]) * 0.5 - albedo[:, :, 1]
    solid = (mask < 0.25) & (key < 0.3)
    solid[1:-1] &= solid[:-2] & solid[2:]
    albedo = fill_gaps(albedo, solid)
    normal = fill_gaps(normal, solid)
    orm = fill_gaps(orm, solid)

    rgba = np.dstack([albedo, alpha])
    rgba, normal, orm = repeat_across(rgba), repeat_across(normal), repeat_across(orm)

    # the cord to the middle
    W = rgba.shape[1]
    gaps = rgba[ROW_TOP:ROW_SPLIT, :, 3] < 0.5
    cord = int(np.argmin(gaps.mean(axis=0)))
    shift = W // 2 - cord
    rgba, normal, orm = (np.roll(m, shift, axis=1) for m in (rgba, normal, orm))

    px_per_m = (ROW_SPLIT - ROW_TOP) / (PERIODS * PITCH_M)
    tile_w = W / px_per_m
    out_h = int(round(OUT_W * (ROW_SPLIT - ROW_TOP) / W))
    rail_h = int(round(OUT_W * (ROW_END - ROW_SPLIT) / W))
    tile_h = PERIODS * PITCH_M
    rail_m = (ROW_END - ROW_SPLIT) / px_per_m

    parts = {
        'blind': (ROW_TOP, ROW_SPLIT, out_h, True),
        'blind_rail': (ROW_SPLIT, ROW_END, rail_h, False),
    }
    for name, (r0, r1, h, wrap) in parts.items():
        d = os.path.join(a.dest, name)
        os.makedirs(d, exist_ok=True)
        A = resize(rgba[r0:r1], (OUT_W, h), wrap)
        save(os.path.join(d, 'albedo.png'), A)
        n = resize(normal[r0:r1], (OUT_W, h), wrap)
        save(os.path.join(d, 'normal.png'), n)
        save(os.path.join(d, 'orm.png'), resize(orm[r0:r1], (OUT_W, h), wrap))
        cov = A[:, :, 3]
        lin = srgb_to_linear(A[:, :, :3])
        mean = (lin * cov[:, :, None]).sum(axis=(0, 1)) / max(cov.sum(), 1e-6)
        print(f'{name}: {OUT_W} x {h}, covers {cov.mean():.3f}, mean linear albedo on the slats '
              f'{mean[0]:.3f} {mean[1]:.3f} {mean[2]:.3f} ({mean.mean():.3f})')

    print(f'cord at column {cord}, moved {shift}; {px_per_m:.0f} px/m')
    print(json.dumps({'blind_tile_m': [round(tile_w, 3), round(tile_h, 3)], 'rail_height_m': round(rail_m, 3)}))
    # OpenGL normals: on a slat, whose top catches the light in the albedo, green should fall going down
    mid = normal.shape[1] // 4
    print('normal green down one slat:', np.round(normal[ROW_SPLIT + 15:ROW_SPLIT + 75:10, mid, 1], 2))
    print('albedo down one slat:      ', np.round(rgba[ROW_SPLIT + 15:ROW_SPLIT + 75:10, mid, 1], 2))


if __name__ == '__main__':
    main()
