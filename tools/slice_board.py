"""Comparison boards for the noodle bar slice (roadmap NB0, docs/noodle-bar-target.md).

For each light tools/slice_capture.py shot, one board: the slice at 3, 8, 25
and 80 m along the top, an office impostor tower at the same distances below
it (the quality bar), and the four references below.

    python tools/slice_board.py [LABEL]      -> build/slice/<LABEL>/board_<light>.png

The references (newVendorRef1.jpg, newVendorRef2.jpg, vendorRef1.png,
vendorRef2.png) are the user's, untracked in the repository's root; any that
are missing are left out.
"""
import os
import sys

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REFS = ["newVendorRef1.jpg", "newVendorRef2.jpg", "vendorRef1.png", "vendorRef2.png"]
DISTS = [3, 8, 25, 80]
TILE_W, TILE_H = 560, 315   # 16:9
PAD, HEAD = 8, 26
BG, INK = (24, 24, 28), (225, 225, 225)


def fit(img, w, h):
    """img scaled to cover w x h, centre cropped."""
    s = max(w / img.width, h / img.height)
    img = img.resize((max(w, round(img.width * s)), max(h, round(img.height * s))), Image.LANCZOS)
    x, y = (img.width - w) // 2, (img.height - h) // 2
    return img.crop((x, y, x + w, y + h))


def board(folder, light):
    rows = [("slice", "the noodle bar slice"), ("office", "office impostor tower (the bar)")]
    W = PAD + len(DISTS) * (TILE_W + PAD)
    grid_h = HEAD + len(rows) * (HEAD + TILE_H + PAD)
    refs = [Image.open(os.path.join(ROOT, r)).convert("RGB") for r in REFS if os.path.exists(os.path.join(ROOT, r))]
    # the references below, two to a row, each half the board wide
    ref_w = (W - 3 * PAD) // 2
    ref_h = [round(ref_w * r.height / r.width) for r in refs]
    row_h = [max(ref_h[i:i + 2]) for i in range(0, len(refs), 2)]
    H = grid_h + HEAD + sum(h + PAD for h in row_h)
    out = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(out)
    d.text((PAD, 6), f"{os.path.basename(folder)}: {light}", fill=INK)
    any_shot = False
    for r, (key, title) in enumerate(rows):
        y = HEAD + r * (HEAD + TILE_H + PAD)
        d.text((PAD, y + 6), title, fill=INK)
        for c, dist in enumerate(DISTS):
            x = PAD + c * (TILE_W + PAD)
            p = os.path.join(folder, f"{light}_{key}_{dist:02d}m.png")
            if os.path.exists(p):
                out.paste(fit(Image.open(p).convert("RGB"), TILE_W, TILE_H), (x, y + HEAD))
                any_shot = True
            else:
                d.rectangle((x, y + HEAD, x + TILE_W, y + HEAD + TILE_H), outline=(90, 90, 90))
            d.text((x + 6, y + HEAD + 6), f"{dist} m", fill=(255, 255, 0))
    y = grid_h
    d.text((PAD, y + 6), "the references", fill=INK)
    y += HEAD
    for i, (img, h) in enumerate(zip(refs, ref_h)):
        out.paste(img.resize((ref_w, h), Image.LANCZOS), (PAD + (i % 2) * (ref_w + PAD), y))
        if i % 2 == 1 or i == len(refs) - 1:
            y += row_h[i // 2] + PAD
    if not any_shot:
        return None
    path = os.path.join(folder, f"board_{light}.png")
    out.save(path)
    return path


def main():
    label = sys.argv[1] if len(sys.argv) > 1 else "now"
    folder = os.path.join(ROOT, "build", "slice", label)
    for light in ("night", "dusk", "noon"):
        p = board(folder, light)
        print(p if p else f"no {light} shots in {folder}")


if __name__ == "__main__":
    main()
