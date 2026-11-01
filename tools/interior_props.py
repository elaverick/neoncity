#!/usr/bin/env python3
"""The office floors' printed matter (docs/interior-impostors.md, section 19): papers for the desks,
charts for the partitions, what the screens show. Written to assets_src/interiors/textures, as files
the baker reads. All of it is made up, by a seed: same seed, same pictures.

  python tools/interior_props.py [--dest assets_src/interiors/textures]
"""
import math
import os
import random
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONTS = "C:/Windows/Fonts/"
INK = (32, 32, 36)
MUTED = (110, 110, 116)
GRID = (214, 214, 214)
PAPER = (240, 237, 228)
# the charts' hues, in a fixed order (scripts/validate_palette.js passes it on the paper)
SERIES = ["#2a78d6", "#d9571e", "#1a9a7d", "#8a5fbf"]

WORDS = ("the quarterly review of district allocations shows a steady rise in tenancy across all towers while "
         "maintenance costs remain within the agreed band subject to approval by the facilities committee and "
         "a further report on the transit levy next month please note that all access badges expire at the end "
         "of the period and must be renewed through the usual channel before any work resumes on the upper floors").split()


def font(name, size):
    return ImageFont.truetype(FONTS + name, size)


def sheet(w, h, rng):
    """A sheet of paper: not quite even, a little foxed."""
    im = Image.new("RGB", (w, h), PAPER)
    noise = Image.effect_noise((w // 4, h // 4), 10).resize((w, h), Image.BICUBIC).filter(ImageFilter.GaussianBlur(4))
    tint = Image.merge("RGB", (noise, noise, noise))
    return Image.blend(im, tint, 0.06)


def lines(d, rng, x, y, width, bottom, f, step, colour=INK):
    while y < bottom:
        cx = x
        end = x + width * rng.uniform(0.82, 1.0)
        while cx < end:
            word = rng.choice(WORDS)
            d.text((cx, y), word, font=f, fill=colour)
            cx += d.textlength(word + " ", font=f)
        y += step
        if rng.random() < 0.14:
            y += step
    return y


def letterhead(d, w, title, sub):
    d.rectangle((70, 60, 118, 108), fill=INK)
    d.text((134, 60), title, font=font("arialbd.ttf", 34), fill=INK)
    d.text((134, 98), sub, font=font("arial.ttf", 20), fill=MUTED)
    d.line((70, 128, w - 70, 128), fill=INK, width=2)


def paper_memo(rng):
    w, h = 1024, 1448
    im = sheet(w, h, rng)
    d = ImageDraw.Draw(im)
    letterhead(d, w, "MEMORANDUM", "Facilities and Tenancy")
    f = font("arial.ttf", 24)
    y = 160
    for k, v in (("To", "All floor managers"), ("From", "Office of the Bursar"), ("Re", "Access and maintenance")):
        d.text((70, y), k + ":", font=font("arialbd.ttf", 24), fill=INK)
        d.text((170, y), v, font=f, fill=INK)
        y += 36
    lines(d, rng, 70, y + 30, w - 140, h - 200, f, 36)
    d.line((70, h - 150, 420, h - 150), fill=(30, 40, 110), width=3)
    return im


def paper_table(rng):
    w, h = 1024, 1448
    im = sheet(w, h, rng)
    d = ImageDraw.Draw(im)
    letterhead(d, w, "SCHEDULE OF ACCOUNTS", "Period ending")
    mono, bold = font("consola.ttf", 24), font("consolab.ttf", 24)
    cols = [70, 330, 540, 760]
    for c, t in zip(cols, ("Ref", "Unit", "Charge", "Balance")):
        d.text((c, 160), t, font=bold, fill=INK)
    d.line((70, 195, w - 70, 195), fill=INK, width=2)
    y = 210
    bal = rng.uniform(4000, 90000)
    while y < h - 170:
        ch = rng.uniform(-900, 2400)
        bal += ch
        row = (f"{rng.randint(10000, 99999)}-{rng.choice('ABCDEF')}", f"{rng.randint(2, 88)}/{rng.randint(1, 40):02d}", f"{ch:10,.2f}", f"{bal:10,.2f}")
        for c, t in zip(cols, row):
            d.text((c, y), t, font=mono, fill=INK)
        y += 34
        if rng.random() < 0.1:
            d.line((70, y, w - 70, y), fill=GRID)
            y += 8
    d.line((70, h - 150, w - 70, h - 150), fill=INK, width=2)
    d.text((540, h - 135), "Total", font=bold, fill=INK)
    d.text((760, h - 135), f"{bal:10,.2f}", font=bold, fill=INK)
    return im


def paper_form(rng):
    w, h = 1024, 1448
    im = sheet(w, h, rng)
    d = ImageDraw.Draw(im)
    letterhead(d, w, "WORK ORDER", "Complete in full")
    f, b = font("arial.ttf", 24), font("arialbd.ttf", 24)
    y = 165
    pen = (30, 40, 110)
    while y < h - 230:
        d.text((70, y), rng.choice(WORDS).capitalize() + " " + rng.choice(WORDS), font=b, fill=INK)
        d.rectangle((70, y + 36, w - 70, y + 92), outline=MUTED, width=2)
        cx = 84
        for _ in range(rng.randint(2, 6)):  # handwriting: a wandering stroke per word
            px, py = cx, y + 66
            for _s in range(rng.randint(5, 12)):
                nx, ny = px + rng.randint(4, 13), y + 66 + rng.randint(-11, 11)
                d.line((px, py, nx, ny), fill=pen, width=3)
                px, py = nx, ny
            cx = px + 18
        y += 122
    for i in range(3):
        d.rectangle((70 + i * 300, h - 170, 96 + i * 300, h - 144), outline=INK, width=2)
        d.text((108 + i * 300, h - 172), rng.choice(WORDS), font=f, fill=INK)
    return im


def paper_notes(rng):
    """A yellow legal pad, handwritten."""
    w, h = 1024, 1448
    im = Image.new("RGB", (w, h), (236, 222, 140))
    d = ImageDraw.Draw(im)
    for y in range(120, h, 48):
        d.line((0, y, w, y), fill=(150, 170, 200), width=2)
    d.line((110, 0, 110, h), fill=(205, 90, 90), width=3)
    pen = (25, 30, 80)
    y = 112
    while y < h - 140:
        cx = 130
        end = rng.randint(420, 950)
        while cx < end:
            px, py = cx, y
            for _s in range(rng.randint(4, 12)):
                nx, ny = px + rng.randint(5, 15), y + rng.randint(-15, 8)
                d.line((px, py, nx, ny), fill=pen, width=3)
                px, py = nx, ny
            cx = px + rng.randint(14, 26)
        y += 48
        if rng.random() < 0.2:
            y += 48
    return im.filter(ImageFilter.GaussianBlur(0.6))


def chart_frame(rng, title, w=1448, h=1024):
    """A landscape print: a title, then the plot's rectangle; hairline axes, recessive."""
    im = sheet(w, h, rng)
    d = ImageDraw.Draw(im)
    d.text((70, 48), title, font=font("arialbd.ttf", 46), fill=INK)
    d.text((70, 104), "Internal: not for circulation", font=font("arial.ttf", 24), fill=MUTED)
    return im, d, (130, 170, w - 70, h - 110)


def y_axis(d, box, steps=5):
    f = font("arial.ttf", 22)
    x0, y0, x1, y1 = box
    for i in range(steps + 1):
        y = y1 - (y1 - y0) * i / steps
        d.line((x0, y, x1, y), fill=GRID, width=2)
        d.text((x0 - 56, y - 12), f"{i * 20}", font=f, fill=MUTED)


def legend(d, names, x, y):
    f = font("arial.ttf", 24)
    for i, n in enumerate(names):
        d.rectangle((x, y + 2, x + 22, y + 24), fill=SERIES[i])
        d.text((x + 32, y), n, font=f, fill=INK)
        x += 54 + int(d.textlength(n, font=f))


def chart_bars(rng):
    im, d, box = chart_frame(rng, "Occupancy by district")
    y_axis(d, box)
    x0, y0, x1, y1 = box
    n = 8
    gw = (x1 - x0) / n
    f = font("arial.ttf", 22)
    for i in range(n):
        for s in range(2):
            v = rng.uniform(0.25, 0.95)
            bx = x0 + gw * i + gw * 0.16 + s * gw * 0.34
            d.rounded_rectangle((bx, y1 - (y1 - y0) * v, bx + gw * 0.3, y1), radius=4, fill=SERIES[s])
        d.text((x0 + gw * i + gw * 0.3, y1 + 10), f"D{i + 1}", font=f, fill=MUTED)
    legend(d, ("This year", "Last year"), 760, 110)
    return im


def chart_lines(rng):
    im, d, box = chart_frame(rng, "Tenancy, trailing twelve months")
    y_axis(d, box)
    x0, y0, x1, y1 = box
    names = ("Towers", "Podiums", "Annexes")
    for s, n in enumerate(names):
        v = rng.uniform(0.3, 0.7)
        pts = []
        for i in range(13):
            v = min(max(v + rng.uniform(-0.09, 0.1), 0.06), 0.95)
            pts.append((x0 + (x1 - x0) * i / 12, y1 - (y1 - y0) * v))
        d.line(pts, fill=SERIES[s], width=5, joint="curve")
        px, py = pts[-1]
        d.ellipse((px - 7, py - 7, px + 7, py + 7), fill=SERIES[s])
    legend(d, names, 760, 110)
    return im


def chart_gantt(rng):
    im, d, box = chart_frame(rng, "Refit programme")
    x0, y0, x1, y1 = box
    f = font("arial.ttf", 24)
    x0 += 150
    for i in range(13):
        x = x0 + (x1 - x0) * i / 12
        d.line((x, y0, x, y1), fill=GRID, width=2)
    rows = 10
    rh = (y1 - y0) / rows
    start = 0
    for r in range(rows):
        y = y0 + rh * r
        d.text((70, y + rh * 0.25), f"Phase {r + 1}", font=f, fill=INK)
        start = min(start + rng.uniform(0, 2.2), 10)
        dur = rng.uniform(1.5, 4.5)
        d.rounded_rectangle((x0 + (x1 - x0) * start / 12, y + rh * 0.2, min(x0 + (x1 - x0) * (start + dur) / 12, x1), y + rh * 0.8),
                            radius=4, fill=SERIES[r % 3])
    return im


def chart_org(rng):
    im, d, _box = chart_frame(rng, "Reporting lines")
    f = font("arial.ttf", 22)
    w = 1448
    def node(cx, cy, t, fill=None):
        d.rounded_rectangle((cx - 90, cy - 36, cx + 90, cy + 36), radius=6, outline=INK, width=3, fill=fill)
        d.text((cx - d.textlength(t, font=f) / 2, cy - 12), t, font=f, fill=(255, 255, 255) if fill else INK)
    top = (w // 2, 230)
    node(*top, "Director", INK)
    mids = [w * (i + 1) / 5 for i in range(4)]
    for m in mids:
        d.line((top[0], top[1] + 36, m, 430 - 36), fill=MUTED, width=3)
        node(m, 430, rng.choice(("Finance", "Tenancy", "Works", "Legal", "Security")))
        for k in range(rng.randint(2, 3)):
            cy = 590 + k * 120
            d.line((m, cy - 120 + 36, m, cy - 36), fill=MUTED, width=3)
            node(m, cy, f"Team {rng.randint(1, 40)}")
    return im


def chart_pie(rng):
    im, d, box = chart_frame(rng, "Spend by category")
    cx, cy, r = 480, 590, 340
    a = -90.0
    shares = [rng.uniform(0.2, 0.5) for _ in range(4)]
    total = sum(shares)
    f = font("arial.ttf", 30)
    names = ("Rent", "Power", "Staff", "Other")
    for i, s in enumerate(shares):
        b = a + 360 * s / total
        d.pieslice((cx - r, cy - r, cx + r, cy + r), a, b, fill=SERIES[i], outline=PAPER, width=5)
        y = 330 + i * 110
        d.rectangle((960, y, 1000, y + 40), fill=SERIES[i])
        d.text((1020, y), f"{names[i]}  {100 * s / total:.0f}%", font=f, fill=INK)
        a = b
    return im


def chart_map(rng):
    """A floor plan with coloured-in zones."""
    im, d, box = chart_frame(rng, "Floor plan: fire routes")
    x0, y0, x1, y1 = box
    x0 -= 60
    d.rectangle((x0, y0, x1, y1), outline=INK, width=5)
    cols, rows = 7, 4
    cw, rh = (x1 - x0) / cols, (y1 - y0) / rows
    for r in range(rows):
        for c in range(cols):
            if rng.random() < 0.3:
                d.rectangle((x0 + c * cw + 6, y0 + r * rh + 6, x0 + (c + 1) * cw - 6, y0 + (r + 1) * rh - 6), fill=rng.choice((SERIES[0], SERIES[2], (200, 200, 200))))
            d.rectangle((x0 + c * cw, y0 + r * rh, x0 + (c + 1) * cw, y0 + (r + 1) * rh), outline=MUTED, width=2)
    # the route, in the warning hue
    y = y0 + rh * (rng.randint(1, 3))
    d.line((x0, y, x1, y), fill=SERIES[1], width=12)
    for c in range(1, cols):
        x = x0 + c * cw
        d.polygon(((x, y - 22), (x + 34, y), (x, y + 22)), fill=SERIES[1])
    return im


def screen(rng, kind):
    """What a screen shows: dim phosphor on black, a little scanline. Only its brightness reaches the
    facade (a screen's colour there is the engine's), so a still and a bake agree on how much is lit."""
    w, h = 1024, 730
    im = Image.new("RGB", (w, h), (2, 5, 3))
    d = ImageDraw.Draw(im)
    f = font("consola.ttf", 22)
    fg = (95, 235, 130)
    dim = (40, 120, 70)
    if kind == 0:  # code
        y = 20
        indent = 0
        while y < h - 26:
            indent = max(0, min(indent + rng.choice((-1, 0, 0, 1)), 4))
            x = 24 + indent * 28
            for _ in range(rng.randint(1, 7)):
                t = rng.choice(("int", "if", "return", "for", "while", "x", "y", "n", "table", "index", "next", "= 0;", "++i", "(p)", "{", "}", "NULL", "size"))
                d.text((x, y), t, font=f, fill=fg if rng.random() < 0.7 else dim)
                x += d.textlength(t + " ", font=f)
                if x > w - 90:
                    break
            y += 27
    elif kind == 1:  # a ledger
        y = 20
        while y < h - 26:
            d.text((24, y), f"{rng.randint(1000, 9999)}  {rng.choice(WORDS)[:8].upper():8}  {rng.uniform(0, 99999):10,.2f}  {rng.choice(('OK', 'OK', 'OK', 'HOLD'))}", font=f, fill=fg if rng.random() < 0.8 else dim)
            y += 27
    elif kind == 2:  # a trace over a grid
        for x in range(0, w, 64):
            d.line((x, 0, x, h), fill=(8, 34, 18))
        for y in range(0, h, 64):
            d.line((0, y, w, y), fill=(8, 34, 18))
        pts = [(x, h * 0.5 + math.sin(x * 0.012) * 150 + rng.uniform(-40, 40)) for x in range(0, w, 8)]
        d.line(pts, fill=fg, width=3)
    else:  # a menu
        d.rectangle((24, 24, w - 24, 80), fill=dim)
        d.text((40, 38), "MAIN MENU", font=f, fill=(0, 0, 0))
        for i in range(12):
            d.text((60, 110 + i * 46), f"{i + 1:>2}  {rng.choice(WORDS).upper()} {rng.choice(WORDS).upper()}", font=f, fill=fg if i != 3 else dim)
    for y in range(0, h, 3):  # scanlines
        d.line((0, y, w, y), fill=(0, 0, 0))
    return im.filter(ImageFilter.GaussianBlur(0.7))


def main():
    dest = "assets_src/interiors/textures"
    if "--dest" in sys.argv:
        dest = sys.argv[sys.argv.index("--dest") + 1]
    os.makedirs(dest, exist_ok=True)
    rng = random.Random(0x0FF1CE)
    for i, f in enumerate((paper_memo, paper_table, paper_form, paper_notes)):
        f(rng).save(os.path.join(dest, f"paper_{i}.png"), optimize=True)
    for i, f in enumerate((chart_bars, chart_lines, chart_gantt, chart_org, chart_pie, chart_map)):
        f(rng).save(os.path.join(dest, f"chart_{i}.png"), optimize=True)
    for i in range(4):
        screen(rng, i).save(os.path.join(dest, f"screen_{i}.png"), optimize=True)
    print("written to", dest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
