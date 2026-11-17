"""Comparison boards for the residential slice (roadmap HM0, docs/residential-target.md).

For each light tools/home_capture.py shot, one board: the slice at 3, 10, 30
and 100 m, from the air and over the roofs along the top, and the references of one region
(docs/reference/residential/<region>_*.jpg, the signed-off mood board's)
below, four to a row.

    python tools/home_board.py [LABEL] [--region na|uk|...]   -> build/home/<LABEL>/board_<light>.png

The region defaults to na (Los Angeles, the home city).
"""
import glob
import os
import sys

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REFS = os.path.join(ROOT, "docs", "reference", "residential")
VIEWS = [("home_003m", "3 m"), ("home_010m", "10 m"), ("home_030m", "30 m"), ("home_100m", "100 m"), ("home_aerial", "aerial"), ("home_roofs", "roofs")]
TILE_W, TILE_H = 448, 252   # 16:9
REF_COLS, REF_W, REF_H = 4, 0, 0
PAD, HEAD = 8, 26
BG, INK = (24, 24, 28), (225, 225, 225)


def fit(img, w, h):
    """img scaled to cover w x h, centre cropped."""
    s = max(w / img.width, h / img.height)
    img = img.resize((max(w, round(img.width * s)), max(h, round(img.height * s))), Image.LANCZOS)
    x, y = (img.width - w) // 2, (img.height - h) // 2
    return img.crop((x, y, x + w, y + h))


def board(folder, light, region):
    W = PAD + len(VIEWS) * (TILE_W + PAD)
    refs = sorted(glob.glob(os.path.join(REFS, f"{region}_*.jpg")))
    rw = (W - PAD * (REF_COLS + 1)) // REF_COLS
    rh = rw * 3 // 4
    rows = (len(refs) + REF_COLS - 1) // REF_COLS
    H = HEAD + HEAD + TILE_H + PAD + HEAD + rows * (rh + PAD)
    out = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(out)
    d.text((PAD, 6), f"{os.path.basename(folder)}: {light}", fill=INK)
    y = HEAD
    d.text((PAD, y + 6), "the residential slice", fill=INK)
    any_shot = False
    for c, (key, title) in enumerate(VIEWS):
        x = PAD + c * (TILE_W + PAD)
        p = os.path.join(folder, f"{light}_{key}.png")
        if os.path.exists(p):
            out.paste(fit(Image.open(p).convert("RGB"), TILE_W, TILE_H), (x, y + HEAD))
            any_shot = True
        else:
            d.rectangle((x, y + HEAD, x + TILE_W, y + HEAD + TILE_H), outline=(90, 90, 90))
        d.text((x + 6, y + HEAD + 6), title, fill=(255, 255, 0))
    y += HEAD + TILE_H + PAD
    d.text((PAD, y + 6), f"the references ({region})", fill=INK)
    y += HEAD
    for i, r in enumerate(refs):
        x = PAD + (i % REF_COLS) * (rw + PAD)
        out.paste(fit(Image.open(r).convert("RGB"), rw, rh), (x, y + (i // REF_COLS) * (rh + PAD)))
        d.text((x + 6, y + (i // REF_COLS) * (rh + PAD) + 6), os.path.basename(r)[:-4], fill=(255, 255, 0))
    if not any_shot:
        return None
    path = os.path.join(folder, f"board_{light}.png")
    out.save(path)
    return path


def main():
    args = sys.argv[1:]
    label, region = "now", "na"
    while args:
        a = args.pop(0)
        if a == "--region":
            region = args.pop(0)
        else:
            label = a
    folder = os.path.join(ROOT, "build", "home", label)
    for light in ("noon", "dusk", "night"):
        p = board(folder, light, region)
        print(p if p else f"no {light} shots in {folder}")


if __name__ == "__main__":
    main()
