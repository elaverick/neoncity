"""Capture the residential slice (roadmap HM0, docs/residential-target.md).

Runs `city_test --home [ARCHETYPE] [--style STYLE]` for the views (the slice
block's frontage at 3, 10, 30 and 100 m, from the air, and over the roofs), then takes each
view with NeonCity.exe --shot in three lights: noon, dusk, night in rain.
The shots land in build/home/<label>/<light>_<view>.png, then
tools/home_board.py lays them out beside the references.

    python tools/home_capture.py [LABEL] [--only noon|dusk|night] [--home ARGS...]

LABEL (default "now") names the set, so a milestone's captures sit beside the
last ones (build/home/hm0, build/home/hm3, ...). Everything after --home is
passed to city_test --home (an archetype, --style STYLE) and to the game as
--home-arch and --home-style, so a style of another region can be captured in
Los Angeles. Each shot's log is
kept beside it (a shot may run to the streaming timeout: that is normal). Run
from the repository's root; NeonCity.exe runs from build/ (its assets sit there).
"""
import os
import subprocess
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")

# The lights, as tools/slice_capture.py has them (Los Angeles' time, UTC-8 in December).
LIGHTS = {
    "noon": ["--utc", "2026-12-23T20:00", "--rain", "0"],
    "dusk": ["--utc", "2026-12-24T01:05", "--rain", "0"],
    "night": ["--utc", "2026-12-24T02:50", "--rain", "0.8"],
}
SHADER_TIME = "30"


def views(home_args):
    """The view lines city_test --home prints: name -> the --cam arguments."""
    exe = os.path.join(BUILD, "city_test.exe")
    out = subprocess.run([exe, "--home", *home_args], cwd=BUILD, capture_output=True, text=True)
    found = {}
    for line in out.stdout.splitlines():
        if line.startswith("view ") and "--cam" in line:
            name, cam = line[5:].split(" --cam ")
            found[name] = cam.split()[:5]   # X Z YAW PITCH EYE_Y, then a note
    if out.returncode != 0:
        print(out.stdout)
        print("city_test --home failed; taking the views it gave")
    return found


def game_args(home_args):
    """city_test --home's forcing as the game's flags: an archetype, --style STYLE."""
    out, a = [], list(home_args)
    while a:
        x = a.pop(0)
        if x == "--style" and a:
            out += ["--home-style", a.pop(0)]
        else:
            out += ["--home-arch", x]
    return out


GAME_ARGS = []


def shoot(label, light, name, cam):
    folder = os.path.join(BUILD, "home", label)
    os.makedirs(folder, exist_ok=True)
    bmp = os.path.join(folder, f"{light}_{name}.bmp")
    log = os.path.join(folder, f"{light}_{name}.log")
    args = [os.path.join(BUILD, "NeonCity.exe"), "--cam", *cam, "--time", SHADER_TIME, *LIGHTS[light], *GAME_ARGS, "--shot", bmp]
    with open(log, "w") as f:   # never pipe the game's output (it blocks on a full pipe)
        subprocess.run(args, cwd=BUILD, stdout=f, stderr=subprocess.STDOUT)
    if os.path.exists(bmp):
        Image.open(bmp).convert("RGB").save(bmp[:-4] + ".png")
        os.remove(bmp)
        print(f"  {light} {name}: ok")
    else:
        print(f"  {light} {name}: no shot (see {log})")


def main():
    args = sys.argv[1:]
    label, only, home_args = "now", None, []
    while args:
        a = args.pop(0)
        if a == "--only":
            only = args.pop(0)
        elif a == "--home":
            home_args, args = args, []
        else:
            label = a
    v = views(home_args)
    GAME_ARGS[:] = game_args(home_args)
    print(f"{len(v)} views: {', '.join(v)}")
    for light in LIGHTS:
        if only and light != only:
            continue
        for name, cam in v.items():
            shoot(label, light, name, cam)


if __name__ == "__main__":
    main()
