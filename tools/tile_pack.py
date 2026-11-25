#!/usr/bin/env python3
"""tile_pack: turns the user's glazed tile images into the houses' six tile materials (residential HM5b2d).

    python tools/tile_pack.py [--src assets_src/tiles] [--preview out.png]

The images (docs: the six sets in house_surf.h HWT_*, in that order) are 1024 x 1024 photographs-in-style of a
grid of tiles with grey grout: four by four for the azulejo and Peranakan sets (15 cm tiles), six by six for the
zellige (10 cm). As drawn they don't quite tile: the grid runs a few pixels past the image's edge (or, in
zellige_a, most of a row), and its lines wander a pixel or two. So the grid is found and laid again:

- The grout is found as the grid of low-saturation, mid-grey lines: the lattice (pitch and phase) the image's
  grout profile along each axis fits best, each line then moved to the grout band's own centre nearby.
- Each tile's face (inside its grout) whose visible part spans most of a tile is lifted out; faces cut short by
  the image's edge by only a few pixels are stretched to size, others (zellige_a's last row and column) left out.
  A set whose art repeats over two tiles (azulejo_a: each tile one quarter of a rosette) keeps an even count.
- The faces are laid on an exact grid of n x n over the 1024 px image, in the order they came, with new grout
  between them (the art's own grout colour, mottled, half a joint at each edge), so copies join seamlessly.
- Height (the normal's alpha, for pbrGrainRelief, and orm.b): the grout at 0, each face up at a rounded arris
  and a little out of plane (hand-set); the Peranakan's moulded flowers and border raised over their ground (by
  how strongly painted), the zellige's hand-made faces uneven; azulejo flat glaze, barely wavy.
- The normal comes from that height at the set's true depth (metres, its file's "depth"); roughness (orm.g) the
  glaze's, grout rough; occlusion (orm.r) a little down in the joints.

Writes assets/textures/house_tile/<set>/{albedo,normal,orm}.png and assets/materials/house_tile_<set>.json (its
tile, albedo mean and depth), and prints each set's mean colour for the far shells' palette (house_palette.glsl
H_TILE, the mean times the file's scale).
"""
import argparse
import os

import numpy as np
from PIL import Image
from scipy import ndimage

OUT = 1024          # px a set's image, square
SCALE = 0.82        # the art's clean glaze to the city's palette (params.z): its white to the stand-in's 0.70

# name, nominal tiles across, a tile's metres, repeat (tiles) the art's pattern spans, the depth (m: tops to the
# joints), the face's relief kind, the glaze's roughness, the description for the material's file
SETS = [
    ("azulejo_a", 4, 0.15, 2, 0.0025, "flat", 0.14,
     "Azulejo, blue and white: Southern European townhouses' whole fronts and dados (HWT_AZULEJO_A)"),
    ("azulejo_b", 4, 0.15, 1, 0.0025, "flat", 0.14,
     "Azulejo, polychrome: Southern European townhouses' whole fronts and dados (HWT_AZULEJO_B)"),
    ("peranakan_a", 4, 0.15, 1, 0.0035, "moulded", 0.12,
     "Peranakan majolica, rose and green: South-East Asian shophouse dados (HWT_PERANAKAN_A)"),
    ("peranakan_b", 4, 0.15, 1, 0.0035, "moulded", 0.12,
     "Peranakan majolica, blue and yellow: South-East Asian shophouse dados (HWT_PERANAKAN_B)"),
    ("zellige_a", 6, 0.10, 1, 0.004, "handmade", 0.10,
     "Zellige, green and white stars on black: Middle Eastern villa and courtyard dados (HWT_ZELLIGE_A)"),
    ("zellige_b", 6, 0.10, 1, 0.004, "handmade", 0.08,
     "Zellige, a plain teal-green field: Middle Eastern villa and courtyard dados (HWT_ZELLIGE_B)"),
]
GROUT_H = 0.0       # the joints' height
ARRIS_M = 0.0015    # metres over which a face's edge rounds down into the joint


def srgb_to_lin(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def lin_to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1.0 / 2.4) - 0.055)


def grout_mask(rgb):
    """Where a pixel looks like grout: grey (unsaturated) and neither the glaze's white nor a black."""
    mx, mn = rgb.max(2), rgb.min(2)
    sat = (mx - mn) / np.maximum(mx, 1e-3)
    lum = rgb.mean(2)
    return ((sat < 0.15) & (lum > 0.3) & (lum < 0.78)).astype(np.float64)


def find_lines(profile, nominal):
    """The grout lines along one axis: the lattice the profile fits best, each line moved to its band's centre.
    Returns the lines' positions (some past the image's ends, extrapolated) and the bands' median width."""
    size = len(profile)
    p = ndimage.uniform_filter1d(profile, 3, mode="nearest")
    best = None
    for pitch in np.arange(size / nominal * 0.85, size / nominal * 1.15, 0.25):
        for phase in np.arange(0.0, pitch, 0.5):
            xs = np.arange(phase, size, pitch).astype(int)
            score = p[xs].mean()
            if best is None or score > best[0]:
                best = (score, pitch, phase)
    _, pitch, phase = best
    lines, widths = [], []
    k0 = -1
    while phase + k0 * pitch > -pitch:
        k0 -= 1
    for k in range(k0, int(size / pitch) + 3):
        x = phase + k * pitch
        if x < -pitch * 0.5 or x > size + pitch * 0.5:
            continue
        r = int(pitch * 0.06)
        lo, hi = int(round(x)) - r, int(round(x)) + r + 1
        if lo < 0 or hi > size:
            lines.append(x)   # at or past an edge: the lattice's
            continue
        seg = p[lo:hi]
        peak = seg.max()
        if peak < 0.5:
            lines.append(x)
            continue
        c = lo + int(np.argmax(seg))
        a, b = c, c
        while a > 0 and p[a - 1] > 0.5 * peak:
            a -= 1
        while b < size - 1 and p[b + 1] > 0.5 * peak:
            b += 1
        lines.append((a + b) / 2.0)
        widths.append(b - a + 1)
    return np.array(lines), float(np.median(widths)) if widths else 3.0, pitch


def faces_along(lines, width, pitch, size):
    """The faces between consecutive lines whose visible span is most of a tile: (start, end) in pixels."""
    out = []
    half = width / 2.0
    for a, b in zip(lines[:-1], lines[1:]):
        f0, f1 = max(a + half, 0.0), min(b - half, size - 1.0)
        full = (b - a) - width
        if f1 - f0 >= 0.95 * full and full > 0.5 * pitch:
            out.append((f0, f1))
    return out


def wrap_blur(x, sigma):
    return ndimage.gaussian_filter(x, sigma, mode="wrap")


def pack(name, nominal, tile_m, period, depth, kind, rough_face, src, rng):
    img = np.asarray(Image.open(src).convert("RGB")).astype(np.float64) / 255.0
    H, W, _ = img.shape
    gm = grout_mask(img)
    xl, xw, xp = find_lines(gm.mean(0), nominal)
    yl, yw, yp = find_lines(gm.mean(1), nominal)
    fx, fy = faces_along(xl, xw, xp, W), faces_along(yl, yw, yp, H)
    n = min(len(fx), len(fy))
    n -= n % period
    fx, fy = fx[:n], fy[:n]
    joint_px = 0.5 * (xw / xp + yw / yp)   # the joint as a fraction of the pitch
    joint = float(np.clip(joint_px, 0.018, 0.035))

    # the art's own grout colour: the median over the found bands
    gsel = gm > 0.5
    grout_rgb = np.median(img[gsel], axis=0) if gsel.any() else np.array([0.5, 0.5, 0.5])

    pitch = OUT / n
    c = (np.arange(OUT) + 0.5) / pitch
    ci = np.minimum(c.astype(int), n - 1)
    u = c - ci                                  # 0..1 across a tile, x and y alike
    g = joint / 2.0
    fu = np.clip((u - g) / (1.0 - 2.0 * g), 0.0, 1.0)
    srcx = np.array([fx[i][0] + fu[k] * (fx[i][1] - fx[i][0]) for k, i in enumerate(ci)])
    srcy = np.array([fy[i][0] + fu[k] * (fy[i][1] - fy[i][0]) for k, i in enumerate(ci)])
    YY, XX = np.meshgrid(srcy, srcx, indexing="ij")
    lin = srgb_to_lin(img)
    face = np.stack([ndimage.map_coordinates(lin[..., k], [YY, XX], order=3, mode="nearest") for k in range(3)], axis=2)
    face = np.clip(face, 0.0, 1.0)

    # distance into the face from its nearest joint, in px (negative in the joint)
    d1 = np.minimum(u - g, 1.0 - g - u) * pitch
    dist = np.minimum(d1[:, None], d1[None, :])
    cover = np.clip(dist + 0.5, 0.0, 1.0)       # the face's edge antialiased over a pixel

    # the grout: the art's colour, mottled (wrapped, so it tiles)
    mott = wrap_blur(rng.standard_normal((OUT, OUT)), 3.0)
    mott /= mott.std() + 1e-9
    grout = srgb_to_lin(grout_rgb)[None, None, :] * (1.0 + 0.06 * mott[..., None])
    alb = grout * (1.0 - cover[..., None]) + face * cover[..., None]

    # height: the face's own relief, tilted a little per tile, rounded down at its arris into the joint
    tid = (ci[:, None] * n + ci[None, :])
    tilts = rng.uniform(-1.0, 1.0, (n * n, 2))
    tx, ty = tilts[tid, 0], tilts[tid, 1]
    uu, vv = u[None, :] - 0.5, u[:, None] - 0.5
    if kind == "moulded":
        mx, mn = face.max(2), face.min(2)
        ink = np.clip(((mx - mn) / np.maximum(mx, 1e-3) - 0.15) / 0.3, 0.0, 1.0)
        relief = wrap_blur(ink, 1.5)
        fh = 0.62 + 0.30 * relief + 0.04 * (tx * uu + ty * vv)
    elif kind == "handmade":
        wav = wrap_blur(rng.standard_normal((OUT, OUT)), pitch * 0.18)
        wav /= np.abs(wav).max() + 1e-9
        fh = 0.84 + 0.10 * wav + 0.10 * (tx * uu + ty * vv)
    else:
        wav = wrap_blur(rng.standard_normal((OUT, OUT)), pitch * 0.25)
        wav /= np.abs(wav).max() + 1e-9
        fh = 0.93 + 0.03 * wav + 0.05 * (tx * uu + ty * vv)
    arris_px = ARRIS_M / tile_m * pitch
    t = np.clip(dist / arris_px, 0.0, 1.0)
    round_ = np.sqrt(1.0 - (1.0 - t) ** 2)       # a quarter round
    h = np.where(dist > 0.0, GROUT_H + (fh - GROUT_H) * round_, GROUT_H)
    h = wrap_blur(h, 0.6)
    h = (h - h.min()) / max(h.max() - h.min(), 1e-6)

    # the normal (OpenGL: +y up the image) from the height at the true depth
    mpp = tile_m * n / OUT                       # metres a pixel
    k = depth / mpp
    dx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * 0.5 * k
    dy = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) * 0.5 * k
    nrm = np.stack([-dx, dy, np.ones_like(h)], axis=2)
    nrm /= np.linalg.norm(nrm, axis=2, keepdims=True)

    face_w = np.clip(dist / max(arris_px, 1.0), 0.0, 1.0)
    rough = rough_face + (0.9 - rough_face) * (1.0 - face_w)
    ao = 0.78 + 0.22 * face_w

    dest = os.path.join("assets", "textures", "house_tile", name)
    os.makedirs(dest, exist_ok=True)
    to8 = lambda x: (np.clip(x, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
    Image.fromarray(to8(lin_to_srgb(alb))).save(os.path.join(dest, "albedo.png"))
    Image.fromarray(to8(np.concatenate([nrm * 0.5 + 0.5, h[..., None]], axis=2)), "RGBA").save(os.path.join(dest, "normal.png"))
    Image.fromarray(to8(np.stack([ao, rough, h], axis=2))).save(os.path.join(dest, "orm.png"))

    mean = alb.reshape(-1, 3).mean(0)
    with open(os.path.join("assets", "materials", f"house_tile_{name}.json"), "w", newline="\n") as f:
        f.write(f"// {desc_of(name)}\n")
        f.write(f"// The user's art (assets_src/tiles/{name}.png), its grid laid again by tools/tile_pack.py: "
                f"{n} x {n} tiles of {round(tile_m * 100)} cm.\n")
        f.write("// Drawn by houseTile (shaders/city/house.glsl); the six sets' ids run in HWT_* order (house_surf.h).\n")
        f.write("{\n")
        f.write('  "maps": {\n')
        f.write(f'    "albedo": "../textures/house_tile/{name}/albedo.png",\n')
        f.write(f'    "normal": "../textures/house_tile/{name}/normal.png",\n')
        f.write(f'    "orm":    "../textures/house_tile/{name}/orm.png"\n')
        f.write("  },\n")
        f.write(f'  "tile": {tile_m * n:.2f},                 // metres per image: {n} tiles\n')
        f.write(f'  "albedo_mean": [{mean[0]:.3f}, {mean[1]:.3f}, {mean[2]:.3f}],   // per channel (tile_pack): the tint divides by it\n')
        f.write("  // the albedo's variation shown (1 all); the normal map's strength (1 as made); the art's clean glaze to the city's palette\n")
        f.write(f'  "params": [1.0, 1.0, {SCALE}],\n')
        f.write("  // metres from the faces' tops down into the joints: parallax occlusion mapped (pbrGrainRelief)\n")
        f.write(f'  "depth": {depth}\n')
        f.write("}\n")
    print(f"{name}: {len(fx)}x{len(fy)} faces of lattice pitch {xp:.1f}/{yp:.1f} px, joint {joint * 100:.1f}% "
          f"(art {joint_px * 100:.1f}%), grout {np.round(grout_rgb, 2)}, mean linear [{mean[0]:.3f}, {mean[1]:.3f}, "
          f"{mean[2]:.3f}] x{SCALE} = [{mean[0] * SCALE:.2f}, {mean[1] * SCALE:.2f}, {mean[2] * SCALE:.2f}]")
    return lin_to_srgb(alb), h, nrm


def desc_of(name):
    return next(s[7] for s in SETS if s[0] == name)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--src", default=os.path.join("assets_src", "tiles"))
    ap.add_argument("--preview", default=None, help="also write each set twice over (2 x 2), its height and normal")
    args = ap.parse_args()
    rows = []
    for k, s in enumerate(SETS):
        rng = np.random.default_rng(0x711E + k)
        alb, h, nrm = pack(*s[:7], os.path.join(args.src, s[0] + ".png"), rng)
        if args.preview:
            half = lambda x: np.asarray(Image.fromarray((np.clip(x, 0, 1) * 255).astype(np.uint8)).resize((512, 512), Image.LANCZOS)) / 255.0
            two = np.tile(half(alb), (2, 2, 1))
            hh = np.repeat(np.tile(half(h), (2, 2))[..., None], 3, axis=2)
            nn = np.tile(half(nrm * 0.5 + 0.5), (2, 2, 1))
            rows.append(np.concatenate([two, hh, nn], axis=1))
    if args.preview:
        Image.fromarray((np.concatenate(rows, axis=0) * 255).astype(np.uint8)).save(args.preview)


if __name__ == "__main__":
    main()
