"""Lay a milestone's captures beside the user's concept sheets (residential roadmap, HM5c1 on).

The user's rule (2026-11-20): every residential milestone checks its buildings and kits against the concept sheets
in docs/reference/international-residential, and puts the sheets beside the captures on its board. Each row of a
board is one style: its sheet (panel 1 the original house, 2-6 its Neon City retrofits), then the captures
tools/home_capture.py took of it (build/home/<label>/<light>_<view>.png).

    python tools/sheet_board.py OUT.jpg TITLE LIGHT VIEWS STYLE:LABEL:SHEET [STYLE:LABEL:SHEET ...]

VIEWS is a comma list (home_010m,home_030m,home_roofs); SHEET a file name in the sheets' folder.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHEETS = os.path.join(ROOT, "docs", "reference", "international-residential")
HOME = os.path.join(ROOT, "build", "home")
H = 330            # a row's height
LABEL_W = 130


def fit_h(img, h):
    return img.resize((max(1, round(img.width * h / img.height)), h), Image.LANCZOS)


def main():
    out, title, light, views = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4].split(",")
    rows = [a.split(":", 2) for a in sys.argv[5:]]
    font = ImageFont.load_default()
    built = []
    for style, label, sheet in rows:
        tiles = [fit_h(Image.open(os.path.join(SHEETS, sheet)).convert("RGB"), H)]
        for v in views:
            p = os.path.join(HOME, label, f"{light}_{v}.png")
            tiles.append(fit_h(Image.open(p).convert("RGB"), H) if os.path.exists(p) else Image.new("RGB", (round(H * 16 / 9), H), (40, 40, 40)))
        built.append((style, tiles))
    width = LABEL_W + max(sum(t.width + 6 for t in tiles) for _, tiles in built)
    board = Image.new("RGB", (width, 40 + len(built) * (H + 8)), (24, 26, 30))
    d = ImageDraw.Draw(board)
    d.text((8, 12), f"{title} ({light}): the sheet, then the game ({', '.join(v.replace('home_', '') for v in views)})", fill=(235, 228, 210), font=font)
    y = 40
    for style, tiles in built:
        d.text((8, y + H // 2), style, fill=(235, 228, 210), font=font)
        x = LABEL_W
        for t in tiles:
            board.paste(t, (x, y))
            x += t.width + 6
        y += H + 8
    board.save(out, quality=88)
    print("wrote", out, board.size)


if __name__ == "__main__":
    main()
