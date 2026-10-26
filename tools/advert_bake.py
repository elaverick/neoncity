#!/usr/bin/env python3
"""Turns a video into an advert for the city's screens: assets/adverts/<name>.ktx2 and,
if there isn't one yet, assets/adverts/<name>.json (src/render/advert_def.h).

The frames file is a KTX2 2D array texture, one layer per frame, block compressed
(BC7, or BC1 at half the size) with its whole mip chain, so the engine uploads it as
it is and the screens sample it with no decoding while the city runs. Each frame is:

- cropped of any black bars round the picture (ffmpeg's cropdetect over the whole
  video; bars thinner than 2% of the frame are taken for dark picture and kept);
- shown at its real shape: the video's sample aspect ratio is applied, so a portrait
  advert stored in a square frame comes out portrait;
- reduced to one pixel per LED (--rows LEDs high), averaged in linear light over the
  source pixels each covers, as are the mip levels below it.

The screens draw the LEDs themselves (shadeAdvert in shaders/city/signs.glsl), so the
source must be the plain picture, not one with an LED effect already in it. The
frames' mean colour (linear), which the screens throw on the clouds, is stored under
the key "NeonCity.mean".

    python tools/advert_bake.py "Drink Ad.mp4" drink [--rows 180] [--fps 30] [--format bc7|bc1] [--crop auto|none|W:H:X:Y]

--rows defaults to 180 (the 6-pixel LED pitch of a 1080-line video). --fps resamples
the video (default: its own rate). Needs ffmpeg and ffprobe on the PATH, and numpy.
An existing manifest is left alone; the tool says if its "fps" no longer matches.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import shutil
import struct
import subprocess
import sys
from fractions import Fraction
from pathlib import Path

import numpy as np

ASSETS = Path(__file__).resolve().parent.parent / "assets" / "adverts"
SLOT_SECONDS = 9.0     # src/core/advert_slots.h: ADVERT_SLOT_SECONDS
STATIC_SHARE = 0.025   # ADVERT_STATIC

MANIFEST = """// {title} (tools/advert_bake.py from {source})
{{
  "frames": "{name}.ktx2",   // the frames (KTX2: one layer each, block compressed, mipmapped)
  "fps": {fps},
  "weight": 4,               // its share of screen time on the screens it fits; each procedural scene has 1
  "brightness": 2.0,         // its colours' gain on the panel
  "glow": 0.4                // the share of each LED's light in its soft glow
}}
"""

# ---------------------------------------------------------------- the video

def probe(ffprobe: str, video: Path) -> dict:
    out = subprocess.run([ffprobe, "-v", "error", "-select_streams", "v:0", "-count_frames",
                          "-show_entries", "stream=width,height,sample_aspect_ratio,r_frame_rate,nb_read_frames",
                          "-of", "json", str(video)], capture_output=True, text=True, check=True)
    return json.loads(out.stdout)["streams"][0]


def detect_crop(ffmpeg: str, video: Path, w: int, h: int) -> tuple[int, int, int, int]:
    """The picture inside any black bars, over the whole video: (w, h, x, y)."""
    out = subprocess.run([ffmpeg, "-hide_banner", "-i", str(video), "-vf", "cropdetect=limit=16:round=2:reset=0",
                          "-f", "null", "-"], capture_output=True, text=True)
    found = re.findall(r"crop=(\d+):(\d+):(\d+):(\d+)", out.stderr)
    if not found:
        return w, h, 0, 0
    cw, ch, cx, cy = (int(v) for v in found[-1])
    # each bar, unless it's thinner than 2% of the frame: then it's dark picture, not a border
    left, right = (v if v >= 0.02 * w else 0 for v in (cx, w - cx - cw))
    top, bottom = (v if v >= 0.02 * h else 0 for v in (cy, h - cy - ch))
    return w - left - right, h - top - bottom, left, top


# ---------------------------------------------------------------- pixels

SRGB_TO_LINEAR = np.array([c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
                           for c in (np.arange(256) / 255.0)], np.float32)


def to_srgb8(lin: np.ndarray) -> np.ndarray:
    c = np.clip(lin, 0.0, 1.0)
    s = np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1.0 / 2.4) - 0.055)
    return np.rint(s * 255.0).astype(np.uint8)


def area_matrix(n_out: int, n_in: int) -> np.ndarray:
    """Averages n_in samples down (or up) to n_out, each weighted by its overlap."""
    m = np.zeros((n_out, n_in), np.float32)
    scale = n_in / n_out
    for o in range(n_out):
        a, b = o * scale, (o + 1) * scale
        for i in range(int(math.floor(a)), min(int(math.ceil(b)), n_in)):
            m[o, i] = (min(b, i + 1) - max(a, i)) / scale
    return m


def resize(lin: np.ndarray, my: np.ndarray, mx: np.ndarray) -> np.ndarray:
    return np.tensordot(np.tensordot(my, lin, axes=(1, 0)), mx, axes=(1, 1)).transpose(0, 2, 1)


def to_blocks(img: np.ndarray) -> np.ndarray:
    """(h, w, 3) -> (blocks, 16, 3), blocks row by row, edges repeated into partial blocks."""
    h, w, _ = img.shape
    bh, bw = -(-h // 4), -(-w // 4)
    pad = np.pad(img, ((0, bh * 4 - h), (0, bw * 4 - w), (0, 0)), mode="edge")
    return pad.reshape(bh, 4, bw, 4, 3).transpose(0, 2, 1, 3, 4).reshape(bh * bw, 16, 3)


# ---------------------------------------------------------------- block compression

def principal_ends(p: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Each block's colours' extent along their principal axis."""
    mean = p.mean(1)
    d = p - mean[:, None, :]
    cov = np.einsum("nki,nkj->nij", d, d)
    v = p.max(1) - p.min(1) + 1e-3
    for _ in range(8):
        v = np.einsum("nij,nj->ni", cov, v)
        v /= np.linalg.norm(v, axis=1, keepdims=True) + 1e-12
    t = np.einsum("nki,ni->nk", d, v)
    return mean + v * t.min(1, keepdims=True), mean + v * t.max(1, keepdims=True)


def refit(p: np.ndarray, t: np.ndarray, e0: np.ndarray, e1: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """The endpoints that best give p at weights t (least squares), where solvable."""
    s = 1.0 - t
    a11, a12, a22 = (s * s).sum(1), (s * t).sum(1), (t * t).sum(1)
    b1, b2 = (s[..., None] * p).sum(1), (t[..., None] * p).sum(1)
    det = a11 * a22 - a12 * a12
    ok = det > 1e-6
    safe = np.where(ok, det, 1.0)[:, None]
    n0 = (a22[:, None] * b1 - a12[:, None] * b2) / safe
    n1 = (a11[:, None] * b2 - a12[:, None] * b1) / safe
    return np.where(ok[:, None], n0, e0), np.where(ok[:, None], n1, e1)


BC7_WEIGHTS = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64], np.int32)


def bc7_quantize(e: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Mode 6 endpoints: 7 bits a channel and a p-bit shared by the endpoint's channels."""
    best = None
    for pbit in (0, 1):
        q = np.clip(np.rint((e - pbit) / 2.0), 0, 127).astype(np.int32)
        r = q * 2 + pbit
        err = ((r - e) ** 2).sum(1)
        if best is None:
            best = [q, np.full(len(e), pbit, np.int32), r, err]
        else:
            better = err < best[3]
            best[0] = np.where(better[:, None], q, best[0])
            best[1] = np.where(better, pbit, best[1])
            best[2] = np.where(better[:, None], r, best[2])
            best[3] = np.minimum(err, best[3])
    return best[0], best[1], best[2]


def bc7_encode(p8: np.ndarray) -> tuple[bytes, np.ndarray]:
    """BC7 mode 6 (one subset, 16 weights): the blocks' bytes and what they decode to."""
    p = p8.astype(np.float32)
    e0, e1 = principal_ends(p)
    for it in range(3):
        q0, p0, r0 = bc7_quantize(np.clip(e0, 0, 255))
        q1, p1, r1 = bc7_quantize(np.clip(e1, 0, 255))
        pal = ((64 - BC7_WEIGHTS)[None, :, None] * r0[:, None, :] + BC7_WEIGHTS[None, :, None] * r1[:, None, :] + 32) >> 6
        idx = ((p[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(-1).argmin(-1)
        if it < 2:
            e0, e1 = refit(p, BC7_WEIGHTS[idx] / 64.0, e0, e1)
    recon = np.take_along_axis(pal, idx[..., None].repeat(3, -1), 1)
    # the first pixel's index has an implicit 0 top bit: swap the endpoints where it hasn't
    swap = idx[:, 0] >= 8
    q0, q1 = np.where(swap[:, None], q1, q0), np.where(swap[:, None], q0, q1)
    p0, p1 = np.where(swap, p1, p0), np.where(swap, p0, p1)
    idx = np.where(swap[:, None], 15 - idx, idx).astype(np.uint64)
    lo = np.full(len(p), 1 << 6, np.uint64)   # mode 6
    for c in range(3):
        lo |= q0[:, c].astype(np.uint64) << np.uint64(7 + 14 * c)
        lo |= q1[:, c].astype(np.uint64) << np.uint64(14 + 14 * c)
    lo |= np.uint64(127) << np.uint64(49)     # alpha: opaque
    lo |= np.uint64(127) << np.uint64(56)
    lo |= p0.astype(np.uint64) << np.uint64(63)
    hi = p1.astype(np.uint64) | (idx[:, 0] << np.uint64(1))
    for i in range(1, 16):
        hi |= idx[:, i] << np.uint64(4 + 4 * (i - 1))
    return np.stack([lo, hi], 1).astype("<u8").tobytes(), recon


def bc1_encode(p8: np.ndarray) -> tuple[bytes, np.ndarray]:
    """BC1, four-colour blocks: the blocks' bytes and what they decode to."""
    p = p8.astype(np.float32)
    e0, e1 = principal_ends(p)
    tw = np.array([0.0, 1.0, 1.0 / 3.0, 2.0 / 3.0], np.float32)

    def q565(e):
        q = np.rint(np.clip(e, 0, 255) * np.array([31, 63, 31]) / 255.0).astype(np.int32)
        r = np.stack([(q[:, 0] << 3) | (q[:, 0] >> 2), (q[:, 1] << 2) | (q[:, 1] >> 4), (q[:, 2] << 3) | (q[:, 2] >> 2)], 1)
        return (q[:, 0] << 11) | (q[:, 1] << 5) | q[:, 2], r.astype(np.float32)

    for it in range(3):
        c0, r0 = q565(e0)
        c1, r1 = q565(e1)
        swap = c0 < c1
        c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
        r0, r1 = np.where(swap[:, None], r1, r0), np.where(swap[:, None], r0, r1)
        e0, e1 = np.where(swap[:, None], e1, e0), np.where(swap[:, None], e0, e1)
        pal = r0[:, None, :] * (1.0 - tw)[None, :, None] + r1[:, None, :] * tw[None, :, None]
        idx = ((p[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(-1).argmin(-1)
        idx = np.where((c0 == c1)[:, None], 0, idx)
        if it < 2:
            e0, e1 = refit(p, tw[idx], e0, e1)
    recon = np.take_along_axis(pal, idx[..., None].repeat(3, -1), 1)
    bits = np.zeros(len(p), np.uint32)
    for i in range(16):
        bits |= idx[:, i].astype(np.uint32) << np.uint32(2 * i)
    out = np.zeros(len(p), dtype=[("c0", "<u2"), ("c1", "<u2"), ("i", "<u4")])
    out["c0"], out["c1"], out["i"] = c0, c1, bits
    return out.tobytes(), recon


FORMATS = {   # name: (VkFormat, KHR_DF_MODEL, bytes a block, encoder)
    "bc7": (146, 135, 16, bc7_encode),   # VK_FORMAT_BC7_SRGB_BLOCK
    "bc1": (132, 128, 8, bc1_encode),    # VK_FORMAT_BC1_RGB_SRGB_BLOCK
}


# ---------------------------------------------------------------- KTX2

def write_ktx2(path: Path, fmt: str, w: int, h: int, layers: int, levels: list[bytes], kv: dict[str, str]) -> None:
    vk_format, model, block, _ = FORMATS[fmt]
    # data format descriptor: one basic block, one sample covering the whole compressed block
    dfd_block = struct.pack("<IHHBBBBBBBBBBBBBBBB", 0, 2, 40, model, 1, 2, 0, 3, 3, 0, 0, block, 0, 0, 0, 0, 0, 0, 0)
    dfd_block += struct.pack("<HBBBBBBII", 0, block * 8 - 1, 0, 0, 0, 0, 0, 0, 0xFFFFFFFF)
    dfd = struct.pack("<I", 4 + len(dfd_block)) + dfd_block
    kvd = b""
    for key in sorted(kv):
        entry = key.encode() + b"\0" + kv[key].encode() + b"\0"
        kvd += struct.pack("<I", len(entry)) + entry + b"\0" * (-len(entry) % 4)
    n = len(levels)
    dfd_off = 80 + 24 * n
    kvd_off = dfd_off + len(dfd)
    pos = kvd_off + len(kvd)
    # the levels, smallest first, each on a 16-byte boundary
    offsets = [0] * n
    body = b""
    for i in reversed(range(n)):
        pad = -(pos + len(body)) % 16
        body += b"\0" * pad
        offsets[i] = pos + len(body)
        body += levels[i]
    head = b"\xabKTX 20\xbb\r\n\x1a\n" + struct.pack("<9I", vk_format, 1, w, h, 0, layers, 1, n, 0)
    head += struct.pack("<4I2Q", dfd_off, len(dfd), kvd_off, len(kvd), 0, 0)
    for i in range(n):
        head += struct.pack("<3Q", offsets[i], len(levels[i]), len(levels[i]))
    path.write_bytes(head + dfd + kvd + body)


# ---------------------------------------------------------------- the bake

def main() -> int:
    ap = argparse.ArgumentParser(description="Bake a video into an advert for the city's screens.")
    ap.add_argument("video", type=Path)
    ap.add_argument("name", help="the advert's name: lower case letters, digits and underscores")
    ap.add_argument("--rows", type=int, default=180, help="LEDs up the picture (default 180)")
    ap.add_argument("--fps", type=float, default=0.0, help="frames per second (default: the video's)")
    ap.add_argument("--format", choices=sorted(FORMATS), default="bc7", help="block compression (default bc7)")
    ap.add_argument("--crop", default="auto", help="auto (cut black bars), none, or W:H:X:Y in the video's pixels")
    a = ap.parse_args()

    if not re.fullmatch(r"[a-z][a-z0-9_]*", a.name):
        print("Error: names are lower case letters, digits and underscores", file=sys.stderr)
        return 2
    ffmpeg, ffprobe = shutil.which("ffmpeg"), shutil.which("ffprobe")
    if not ffmpeg or not ffprobe:
        print("Error: ffmpeg and ffprobe must be on the PATH", file=sys.stderr)
        return 2
    if not a.video.is_file():
        print(f"Error: no such video: {a.video}", file=sys.stderr)
        return 2

    s = probe(ffprobe, a.video)
    vw, vh = int(s["width"]), int(s["height"])
    rate, count = Fraction(s["r_frame_rate"]), int(s["nb_read_frames"])
    sar_text = s.get("sample_aspect_ratio", "1:1")
    sar = Fraction(*(int(v) for v in sar_text.split(":"))) if re.fullmatch(r"[1-9]\d*:[1-9]\d*", sar_text) else Fraction(1)
    if a.crop == "auto":
        cw, ch, cx, cy = detect_crop(ffmpeg, a.video, vw, vh)
    elif a.crop == "none":
        cw, ch, cx, cy = vw, vh, 0, 0
    else:
        cw, ch, cx, cy = (int(v) for v in a.crop.split(":"))
    aspect = cw * sar / ch
    rows = a.rows
    cols = max(1, round(rows * aspect))
    fps = Fraction(a.fps).limit_denominator(1000) if a.fps > 0 else rate
    frames = count if a.fps <= 0 else math.floor(count / rate * fps)
    seconds = frames / fps
    if seconds > SLOT_SECONDS * (1.0 - STATIC_SHARE):
        print(f"Error: {float(seconds):.2f} s is longer than a screen's slot "
              f"({SLOT_SECONDS * (1.0 - STATIC_SHARE):.3f} s after the static)", file=sys.stderr)
        return 2
    print(f"{a.video.name}: {vw}x{vh}, sample aspect {sar}; the picture {cw}x{ch} at ({cx}, {cy}), "
          f"shown {float(aspect):.3f}:1 -> {cols}x{rows} LEDs")

    vk_format, _, block, encode = FORMATS[a.format]
    dims = [(cols, rows)]
    while dims[-1] != (1, 1):
        dims.append((max(1, dims[-1][0] // 2), max(1, dims[-1][1] // 2)))
    mats = [(area_matrix(h, ch), area_matrix(w, cw)) for w, h in dims]
    levels = [bytearray() for _ in dims]
    total = np.zeros(3, np.float64)
    err = 0.0

    vf = f"crop={cw}:{ch}:{cx}:{cy}" + (f",fps={fps}" if a.fps > 0 else "")
    proc = subprocess.Popen([ffmpeg, "-v", "error", "-i", str(a.video), "-vf", vf, "-frames:v", str(frames),
                             "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], stdout=subprocess.PIPE)
    done = 0
    while done < frames:
        raw = proc.stdout.read(cw * ch * 3)
        if len(raw) < cw * ch * 3:
            break
        lin = SRGB_TO_LINEAR[np.frombuffer(raw, np.uint8).reshape(ch, cw, 3)]
        for k, (my, mx) in enumerate(mats):   # every level straight from the source, in linear light
            level = resize(lin, my, mx)
            if k == 0:
                total += level.reshape(-1, 3).mean(0)
            px = to_srgb8(level)
            data, recon = encode(to_blocks(px))
            levels[k] += data
            if k == 0:
                err += float(((recon - to_blocks(px).astype(np.float32)) ** 2).mean())
        done += 1
    proc.stdout.close()
    proc.wait()
    if done < frames:
        print(f"Error: the video gave {done} of {frames} frames", file=sys.stderr)
        return 2

    ASSETS.mkdir(parents=True, exist_ok=True)
    out = ASSETS / f"{a.name}.ktx2"
    mean = total / frames
    write_ktx2(out, a.format, cols, rows, frames, [bytes(l) for l in levels],
               {"KTXwriter": "NeonCity tools/advert_bake.py", "NeonCity.mean": " ".join(f"{v:.5f}" for v in mean)})
    psnr = 10.0 * math.log10(255.0 ** 2 / max(err / frames, 1e-9))
    print(f"Wrote {out}: {frames} frames of {cols}x{rows} at {float(fps):g} fps ({float(seconds):.2f} s), "
          f"{a.format.upper()}, {len(dims)} levels, {out.stat().st_size / 1e6:.1f} MB; {psnr:.1f} dB against the reduced frames")

    fps_text = f"{float(fps):g}"
    manifest = ASSETS / f"{a.name}.json"
    if not manifest.exists():
        manifest.write_text(MANIFEST.format(title=a.name.replace("_", " ").capitalize() + " advert",
                                            source=a.video.name, name=a.name, fps=fps_text),
                            encoding="utf-8", newline="\n")
        print(f"Wrote {manifest}: set its weight, brightness and glow")
    else:
        m = re.search(r'"fps"\s*:\s*([0-9.]+)', manifest.read_text(encoding="utf-8"))
        if not m or float(m.group(1)) != float(fps):
            print(f"Warning: {manifest.name} has \"fps\": {m.group(1) if m else '(none)'}; the frames are {fps_text}",
                  file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
