#!/usr/bin/env python3
"""relief_time: the parallax depth roadmap's views (PD0), shot and timed with relief on, off and in its debug view.

    python tools/relief_time.py LABEL [--modes on,off,debug] [--views V1,V2] [--light noon|night] [--rounds N]

The views (each a --cam and the forcing it needs): a Victorian terrace (brick, slate) at 3 and 10 m, over its roofs
and from the air (city_test --home's views); the city's slice street at 8 and 25 m (city_test --slice's). Each shot
lands in build/relief/LABEL/<light>_<view>_<mode>.png with its log; then a table of the frame's GPU passes that
the relief touches (city, kit, proxies) and the whole, the best of its rounds, per view and mode, and the difference
on - off where both were taken.
"""
import os
import re
import subprocess
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
LIGHTS = {
    "noon": ["--utc", "2026-12-23T20:00", "--rain", "0"],
    "night": ["--utc", "2026-12-24T02:50", "--rain", "0.8"],
}
VIC = ["--home-arch", "terrace", "--home-style", "victorian"]
VIEWS = {
    "house_03m": (["8194.51", "7475.56", "103.4", "23.3", "-102.95"], VIC),
    "house_10m": (["8187.70", "7473.94", "103.4", "6.9", "-102.94"], VIC),
    "house_roofs": (["8180.90", "7478.09", "83.4", "-21.8", "-82.72"], VIC),
    "house_air": (["8071.63", "7490.78", "83.4", "-31.0", "-14.72"], VIC),
    "slice_08m": (["339.93", "1176.29", "-127.1", "-4.0", "-19.50"], []),
    "slice_25m": (["358.35", "1178.04", "-97.1", "-1.3", "-19.48"], []),
}
PASSES = ("city", "kit", "proxies")


def shoot(folder, light, view, mode, rnd):
    cam, extra = VIEWS[view]
    stem = os.path.join(folder, f"{light}_{view}_{mode}" + (f"_{rnd}" if rnd else ""))
    args = [os.path.join(BUILD, "NeonCity.exe"), "--cam", *cam, "--time", "30", *LIGHTS[light], *extra,
            "--relief", mode, "--shot", stem + ".bmp"]
    with open(stem + ".log", "w") as f:   # never pipe the game's output (it blocks on a full pipe)
        subprocess.run(args, cwd=BUILD, stdout=f, stderr=subprocess.STDOUT)
    if os.path.exists(stem + ".bmp"):
        Image.open(stem + ".bmp").convert("RGB").save(stem + ".png")
        os.remove(stem + ".bmp")
    times = {}
    for line in open(stem + ".log", errors="replace"):
        if line.startswith("GPU (last 64 frames)"):
            for name, v in re.findall(r"([a-z][a-z ]*?) ([0-9.]+)(?:,|$| =)", line[len("GPU (last 64 frames) "):]):
                times[name.strip()] = float(v)
            m = re.search(r"= ([0-9.]+) ms", line)
            if m:
                times["all"] = float(m.group(1))
    return times


def main():
    args = sys.argv[1:]
    label, modes, views, light, rounds = "now", ["on", "off", "debug"], list(VIEWS), "noon", 1
    while args:
        a = args.pop(0)
        if a == "--modes": modes = args.pop(0).split(",")
        elif a == "--views": views = args.pop(0).split(",")
        elif a == "--light": light = args.pop(0)
        elif a == "--rounds": rounds = int(args.pop(0))
        else: label = a
    folder = os.path.join(BUILD, "relief", label)
    os.makedirs(folder, exist_ok=True)
    best = {}
    for view in views:
        for mode in modes:
            for r in range(rounds if mode != "debug" else 1):
                t = shoot(folder, light, view, mode, r if rounds > 1 and mode != "debug" else 0)
                if mode == "debug" or not t:
                    continue
                cur = best.setdefault((view, mode), t)
                for k, v in t.items():
                    cur[k] = min(cur.get(k, v), v)
            print(f"  {view} {mode}: done", flush=True)
    cols = PASSES + ("all",)
    print(f"\n{label} ({light}): GPU ms, the best of {rounds} round(s)")
    print(f"  {'view':12s} {'mode':5s} " + " ".join(f"{c:>8s}" for c in cols))
    for view in views:
        for mode in modes:
            t = best.get((view, mode))
            if t:
                print(f"  {view:12s} {mode:5s} " + " ".join(f"{t.get(c, 0.0):8.2f}" for c in cols))
        on, off = best.get((view, "on")), best.get((view, "off"))
        if on and off:
            print(f"  {view:12s} {'on-off':5s} " + " ".join(f"{on.get(c, 0.0) - off.get(c, 0.0):+8.2f}" for c in cols))


if __name__ == "__main__":
    main()
