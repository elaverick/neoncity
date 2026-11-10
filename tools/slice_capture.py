"""Capture the noodle bar slice (roadmap NB0, docs/noodle-bar-target.md).

Runs `city_test --slice` for the views (the slice at 3, 8, 25 and 80 m, and an
office impostor tower's curtain wall at the same distances), then takes each
view with NeonCity.exe --shot in three lights: night in rain, dusk, noon.
The shots land in build/slice/<label>/<light>_<view>.png, then
tools/slice_board.py lays them out beside the references.

    python tools/slice_capture.py [LABEL] [--only night|dusk|noon] [--views slice|office|all]

LABEL (default "now") names the set, so a milestone's captures sit beside the
last ones (build/slice/nb4, build/slice/nb5, ...). Each shot's log is kept
beside it (a shot may run to the streaming timeout: that is normal). Run from
the repository's root; NeonCity.exe runs from build/ (its assets sit there).
"""
import os
import subprocess
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")

# The lights. The city keeps Los Angeles' time (UTC-8 in December): night at
# 18:50 (the sky's capture instant), dusk at 17:05, noon at 12:00.
LIGHTS = {
    "night": ["--utc", "2026-12-24T02:50", "--rain", "0.8"],
    "dusk": ["--utc", "2026-12-24T01:05", "--rain", "0"],
    "noon": ["--utc", "2026-12-23T20:00", "--rain", "0"],
}
SHADER_TIME = "30"   # the shaders' clock held (flames, signs, steam)


def views():
    """The view lines city_test --slice prints: name -> the --cam arguments."""
    exe = os.path.join(BUILD, "city_test.exe")
    out = subprocess.run([exe, "--slice"], cwd=BUILD, capture_output=True, text=True)
    found = {}
    for line in out.stdout.splitlines():
        if line.startswith("view ") and "--cam" in line:
            name, cam = line[5:].split(" --cam ")
            found[name] = cam.split()[:5]   # X Z YAW PITCH EYE_Y, then a note
    if out.returncode != 0:
        print(out.stdout)
        print("city_test --slice failed; taking the views it gave")
    return found


def shoot(label, light, name, cam):
    folder = os.path.join(BUILD, "slice", label)
    os.makedirs(folder, exist_ok=True)
    bmp = os.path.join(folder, f"{light}_{name}.bmp")
    log = os.path.join(folder, f"{light}_{name}.log")
    args = [os.path.join(BUILD, "NeonCity.exe"), "--cam", *cam, "--time", SHADER_TIME, *LIGHTS[light], "--shot", bmp]
    if name.startswith("office"):
        args[1:1] = ["--impostors", "1"]   # every floor behind its glass drawn as an impostor
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
    label, only, which = "now", None, "all"
    while args:
        a = args.pop(0)
        if a == "--only":
            only = args.pop(0)
        elif a == "--views":
            which = args.pop(0)
        else:
            label = a
    v = views()
    print(f"{len(v)} views: {', '.join(v)}")
    for light in LIGHTS:
        if only and light != only:
            continue
        for name, cam in v.items():
            if which != "all" and not name.startswith(which):
                continue
            shoot(label, light, name, cam)


if __name__ == "__main__":
    main()
