#!/usr/bin/env python3
"""The office furniture's source models (docs/interior-impostors.md, section 19).

The supplied GLBs are centred in a box about 1.9 units across, with each texture variant a file of
its own. This writes them to the baker's contract, in assets_src/interiors: metres, y up, standing on
y = 0, centred on x and z, front towards +z, textures embedded, one file a model. Variants of a model
become materials of one mesh (KHR_materials_variants). The shapes are the supplied ones: nothing is
simplified or repaired (leaks are the capture's to bound, BF_LEAKY in shaders/interior/capture.comp).

  python tools/interior_models.py pack  <source dir> [--metal <dir>] [--rug <dir>] [--only a,b] [--dest assets_src/interiors]
  python tools/interior_models.py books <dir of chosen covers, one per colour> [--dest assets_src/interiors]
  python tools/interior_models.py check [--dest assets_src/interiors]

The computer and the lamp gain one extra primitive each, a flat panel for what lights up, wearing a
material named "screen" or "diffuser": the baker turns it into an emitter when the fitting is on.
"""
import io
import json
import os
import struct
import sys

import numpy as np
from PIL import Image

Image.MAX_IMAGE_PIXELS = None

ALBEDO_SIZE = 4096   # the supplied 8192s are far past what a bake (3 cm a texel at best) or a still can show
DATA_SIZE = 2048

# name, the supplied files (one per variant), metres to a supplied unit, whether its mesh is watertight
MODELS = [
    ("chair", ["chair 0", "chair 1", "chair 2"], 1.0),
    ("desk", ["desk 0", "desk 1", "desk 2"], 1.89),     # 0.74 m to the top, 1.8 wide; each desk stretches to fit its plan
    ("computer", ["computer"], 0.28),                  # 0.53 wide, with its keyboard
    ("lamp", ["lamp"], 0.17),                          # a banker's lamp, 0.32 tall
    ("partition", ["office partition"], 0.84),         # 1.6 wide; each stretches to its run and height
    ("filing_cabinet", ["filing cabinet"], 0.7),
    ("blackboard", ["blackboard"], 1.0),
    ("intray", ["intray"], 0.36),                      # 34 cm wide, two tiers
    ("phone", ["phone"], 0.115),                       # 22 cm deep
    ("fountain_pen", ["fountain pen"], 0.074),         # 14 cm long
    ("armchair", ["armchair"], 0.369),                 # a club chair, 0.70 wide and tall
    ("printer", ["printer"], 0.315),                   # a dot-matrix on its stand, 0.60 wide, 0.30 tall
    ("side_table", ["sidet table"], 0.263),            # 0.50 wide; each stretches to its plan's height
    ("conference_table", ["conference table"], 1.343), # 0.76 to the top; each stretches to its room
]


def read_glb(path):
    data = open(path, "rb").read()
    length = struct.unpack_from("<I", data, 12)[0]
    j = json.loads(data[20:20 + length])
    blob = data[20 + length + 8:]

    def accessor(i):
        a = j["accessors"][i]
        bv = j["bufferViews"][a["bufferView"]]
        comp = {5126: "f4", 5125: "u4", 5123: "u2", 5121: "u1"}[a["componentType"]]
        n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[a["type"]]
        off = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
        arr = np.frombuffer(blob, dtype=comp, count=a["count"] * n, offset=off)
        return arr.reshape(-1, n) if n > 1 else arr

    prims = []
    for mesh in j["meshes"]:
        for pr in mesh["primitives"]:
            prims.append({"P": accessor(pr["attributes"]["POSITION"]).astype(np.float64),
                          "N": accessor(pr["attributes"]["NORMAL"]).astype(np.float64),
                          "T": accessor(pr["attributes"]["TEXCOORD_0"]).astype(np.float64),
                          "F": accessor(pr["indices"]).reshape(-1, 3).astype(np.int64),
                          "material": pr.get("material")})
    m = dict(prims[0])
    m["prims"] = prims
    m["json"] = j
    m["images"] = []
    for im in j.get("images", []):
        bv = j["bufferViews"][im["bufferView"]]
        o = bv.get("byteOffset", 0)
        m["images"].append(blob[o:o + bv["byteLength"]])  # base colour, metallic-roughness, normal
    return m


def jpeg(im, quality, subsampling):
    b = io.BytesIO()
    im.convert("RGB").save(b, "JPEG", quality=quality, subsampling=subsampling, optimize=True)
    return b.getvalue()


def encode_set(albedo, mr, normal):
    """Base colour, metallic-roughness and normal, as PIL images, for a file."""
    a = albedo if albedo.size[0] <= ALBEDO_SIZE else albedo.resize((ALBEDO_SIZE, ALBEDO_SIZE), Image.LANCZOS)
    r = mr if mr.size[0] <= DATA_SIZE else mr.resize((DATA_SIZE, DATA_SIZE), Image.LANCZOS)
    n = normal if normal.size[0] <= DATA_SIZE else normal.resize((DATA_SIZE, DATA_SIZE), Image.LANCZOS)
    return [jpeg(a, 92, 1), jpeg(r, 92, 0), jpeg(n, 95, 0)]


def open_set(mesh):
    return [Image.open(io.BytesIO(b)).convert("RGB") for b in mesh["images"]]


# ----------------------------------------------- one mesh, several texture layouts ---

def fill_gutters(img, written, passes=12):
    """Bleeds written texels into the unwritten, so mips and bilinear don't pick up black at a seam."""
    img = img.copy()
    written = written.copy()
    for _ in range(passes):
        n = np.zeros_like(img, dtype=np.float32)
        c = np.zeros(written.shape, dtype=np.float32)
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            w = np.roll(written, (dy, dx), (0, 1))
            n += np.roll(img, (dy, dx), (0, 1)).astype(np.float32) * w[..., None]
            c += w
        grow = (~written) & (c > 0)
        img[grow] = (n[grow] / c[grow][:, None]).astype(np.uint8)
        written |= grow
    return img


def tangent_frames(P, T, F):
    """Each triangle's world tangent and image-up, unit length, from its UVs."""
    p = P[F]
    uv = T[F]
    e1, e2 = p[:, 1] - p[:, 0], p[:, 2] - p[:, 0]
    d1, d2 = uv[:, 1] - uv[:, 0], uv[:, 2] - uv[:, 0]
    det = d1[:, 0] * d2[:, 1] - d1[:, 1] * d2[:, 0]
    det = np.where(np.abs(det) < 1e-12, 1e-12, det)
    t = (e1 * d2[:, 1, None] - e2 * d1[:, 1, None]) / det[:, None]
    b = -(e2 * d1[:, 0, None] - e1 * d2[:, 0, None]) / det[:, None]   # image up is against +v
    t /= np.maximum(np.linalg.norm(t, axis=1, keepdims=True), 1e-12)
    b /= np.maximum(np.linalg.norm(b, axis=1, keepdims=True), 1e-12)
    return t, b


def transfer(target, source, source_images):
    """source's textures, redrawn on target's texture layout: both are one shape, triangle for
    triangle, with their UVs laid out differently. Normals are turned from one layout's tangent frame
    to the other's, triangle by triangle."""
    if not np.allclose(target["P"][target["F"]], source["P"][source["F"]], atol=1e-5):
        raise SystemExit("variants differ in shape: their textures can't share a mesh")
    tuv, suv = target["T"][target["F"]], source["T"][source["F"]]
    tt, tb = tangent_frames(target["P"], target["T"], target["F"])
    st, sb = tangent_frames(source["P"], source["T"], source["F"])
    # the source's tangent frame seen in the target's
    m00, m01 = (st * tt).sum(1), (sb * tt).sum(1)
    m10, m11 = (st * tb).sum(1), (sb * tb).sum(1)
    sizes = (ALBEDO_SIZE, DATA_SIZE, DATA_SIZE)
    out = []
    for k, (im, R) in enumerate(zip(source_images, sizes)):
        src = np.asarray(im)
        sh, sw = src.shape[:2]
        dst = np.zeros((R, R, 3), np.uint8)
        wrote = np.zeros((R, R), bool)
        for i in range(len(tuv)):
            a, b, c = tuv[i] * R - 0.5
            x0, x1 = int(np.floor(min(a[0], b[0], c[0]))) - 1, int(np.ceil(max(a[0], b[0], c[0]))) + 1
            y0, y1 = int(np.floor(min(a[1], b[1], c[1]))) - 1, int(np.ceil(max(a[1], b[1], c[1]))) + 1
            x0, y0, x1, y1 = max(x0, 0), max(y0, 0), min(x1, R - 1), min(y1, R - 1)
            if x1 < x0 or y1 < y0:
                continue
            gy, gx = np.mgrid[y0:y1 + 1, x0:x1 + 1]
            d = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
            if abs(d) < 1e-9:
                continue
            w1 = ((gx - a[0]) * (c[1] - a[1]) - (gy - a[1]) * (c[0] - a[0])) / d
            w2 = ((b[0] - a[0]) * (gy - a[1]) - (b[1] - a[1]) * (gx - a[0])) / d
            w0 = 1 - w1 - w2
            pad = 1.0 / max(abs(d) ** 0.5, 1.0)   # a texel's margin, so neighbours meet
            mask = (w0 >= -pad) & (w1 >= -pad) & (w2 >= -pad)
            if not mask.any():
                continue
            s = suv[i]
            su = w0 * s[0, 0] + w1 * s[1, 0] + w2 * s[2, 0]
            sv = w0 * s[0, 1] + w1 * s[1, 1] + w2 * s[2, 1]
            sx = np.clip(su[mask] * sw, 0, sw - 1).astype(int)
            sy = np.clip(sv[mask] * sh, 0, sh - 1).astype(int)
            texels = src[sy, sx]
            if k == 2:
                n = texels.astype(np.float32) / 127.5 - 1.0
                nx = m00[i] * n[:, 0] + m01[i] * n[:, 1]
                ny = m10[i] * n[:, 0] + m11[i] * n[:, 1]
                texels = np.stack([np.clip((nx + 1) * 127.5, 0, 255), np.clip((ny + 1) * 127.5, 0, 255), texels[:, 2]], 1).astype(np.uint8)
            dst[gy[mask], gx[mask]] = texels
            wrote[gy[mask], gx[mask]] = True
        out.append(Image.fromarray(fill_gutters(dst, wrote)))
    return out


# ------------------------------------------------------------- emitter panels ---

def rounded_rect(centre, u, v, normal, hu, hv, radius):
    """A flat rounded rectangle: a fan round its centre, uv 0..1 across it with v = 0 at the image's top
    (+v axis), wound to face normal."""
    ring = []
    for cu, cv, a0 in ((hu - radius, hv - radius, 0.0), (-hu + radius, hv - radius, 90.0),
                       (-hu + radius, -hv + radius, 180.0), (hu - radius, -hv + radius, 270.0)):
        for s in range(7):
            a = np.radians(a0 + s * 15.0)
            ring.append((cu + radius * np.cos(a), cv + radius * np.sin(a)))
    ring = np.array(ring)
    pts = np.vstack([[0.0, 0.0], ring])
    P = centre + pts[:, 0, None] * u + pts[:, 1, None] * v
    T = np.stack([pts[:, 0] / (2 * hu) + 0.5, 0.5 - pts[:, 1] / (2 * hv)], 1)
    N = np.tile(normal, (len(P), 1))
    n = len(ring)
    F = np.array([(0, 1 + i, 1 + (i + 1) % n) for i in range(n)], dtype=np.int64)
    if np.dot(np.cross(u, v), normal) < 0:
        F = F[:, [0, 2, 1]]
    return {"P": P, "N": N, "T": T, "F": F}


def computer_screen(lift):
    """The CRT's face, measured from its glass (centre, plane, extents in supplied units), just proud of it."""
    centre = np.array([-0.32, 0.16, 0.061])
    n = np.array([0.008, 0.276, 0.961])
    n /= np.linalg.norm(n)
    u = np.array([1.0, 0.0, 0.0])
    u -= n * (u @ n)
    u /= np.linalg.norm(u)
    v = np.cross(n, u)
    return rounded_rect(centre + n * 0.03, u, v, n, 0.33, 0.235, 0.06)


def lamp_diffuser(lift):
    """A panel in the shade's mouth (its rim runs from z 0.92, y 0.51 to z 0.11, y 0.395 in supplied units), facing down it."""
    a, b = np.array([0.0, 0.51, 0.92]), np.array([0.0, 0.395, 0.11])
    along = (a - b) / np.linalg.norm(a - b)
    n = np.array([0.0, -along[2], along[1]])
    if n[1] > 0:
        n = -n
    centre = (a + b) * 0.5 - n * 0.04
    u = np.array([1.0, 0.0, 0.0])
    return rounded_rect(centre, u, along, n, 0.6, 0.3, 0.08)


# ------------------------------------------------------------------- the writer ---

def write_glb(path, name, body, sets, extra):
    """body: the mesh. sets: [(variant name, [colour, metal-roughness, normal bytes])]. extra: [(material name, mesh)]."""
    blob = bytearray()
    views, accessors = [], []

    def add(data, target=None):
        while len(blob) % 4:
            blob.append(0)
        view = {"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)}
        if target:
            view["target"] = target
        blob.extend(data)
        views.append(view)
        return len(views) - 1

    def acc(arr, comp, kind, target, bounds=False):
        a = {"bufferView": add(arr.tobytes(), target), "componentType": comp, "count": len(arr), "type": kind}
        if bounds:
            a["min"] = arr.min(0).astype(float).tolist()
            a["max"] = arr.max(0).astype(float).tolist()
        accessors.append(a)
        return len(accessors) - 1

    def prim(mesh, material):
        return {"attributes": {"POSITION": acc(mesh["P"].astype(np.float32), 5126, "VEC3", 34962, True),
                               "NORMAL": acc(mesh["N"].astype(np.float32), 5126, "VEC3", 34962),
                               "TEXCOORD_0": acc(mesh["T"].astype(np.float32), 5126, "VEC2", 34962)},
                "indices": acc(mesh["F"].astype(np.uint32).ravel(), 5125, "SCALAR", 34963),
                "material": material, "mode": 4}

    images, textures, materials = [], [], []
    for vname, maps in sets:
        ids = []
        for b in maps:
            images.append({"bufferView": add(b), "mimeType": "image/jpeg"})
            textures.append({"source": len(images) - 1})
            ids.append(len(textures) - 1)
        materials.append({"name": vname,
                          "pbrMetallicRoughness": {"baseColorTexture": {"index": ids[0]},
                                                   "metallicRoughnessTexture": {"index": ids[1]},
                                                   "metallicFactor": 1.0, "roughnessFactor": 1.0},
                          "normalTexture": {"index": ids[2]}, "doubleSided": True})
    prims = [prim(body, 0)]
    if len(sets) > 1:
        prims[0]["extensions"] = {"KHR_materials_variants": {"mappings": [{"material": i, "variants": [i]} for i in range(len(sets))]}}
    for mname, mesh in extra:
        materials.append({"name": mname, "pbrMetallicRoughness": {"baseColorFactor": [0.02, 0.02, 0.02, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.2}})
        prims.append(prim(mesh, len(materials) - 1))
    j = {"asset": {"version": "2.0", "generator": "NeonCity tools/interior_models.py"},
         "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "name": name}],
         "meshes": [{"name": name, "primitives": prims}],
         "materials": materials, "textures": textures, "images": images,
         "samplers": [], "accessors": accessors, "bufferViews": views}
    del j["samplers"]
    if len(sets) > 1:
        j["extensionsUsed"] = ["KHR_materials_variants"]
        j["extensions"] = {"KHR_materials_variants": {"variants": [{"name": s[0]} for s in sets]}}
    while len(blob) % 4:
        blob.append(0)
    j["buffers"] = [{"byteLength": len(blob)}]
    js = json.dumps(j, separators=(",", ":")).encode()
    js += b" " * (-len(js) % 4)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(blob)))
        f.write(struct.pack("<II", len(js), 0x4E4F534A))
        f.write(js)
        f.write(struct.pack("<II", len(blob), 0x004E4942))
        f.write(blob)
    return os.path.getsize(path)


def to_metres(mesh, scale):
    """Scaled, centred on x and z, standing on y = 0. Returns the shift, for the panels that go with it."""
    P = mesh["P"] * scale
    mn, mx = P.min(0), P.max(0)
    shift = np.array([-(mn[0] + mx[0]) * 0.5, -mn[1], -(mn[2] + mx[2]) * 0.5])
    out = dict(mesh)
    out["P"] = P + shift
    return out, shift


def book_atlas(cover):
    """One book's texture from a generated cover (furniture.c's book mesh reads this layout): the cover
    on the left 5/8, a spine made from the same leather in the next 1/8, with raised bands, and the page
    block's edges on the last 1/4."""
    S = 2048
    w, spine = S * 5 // 8, S // 8
    im = cover.convert("RGB")
    cw, ch = im.size
    m = min(cw, ch) // 20                                    # the generator's margins are its least reliable part
    im = im.crop((m, m, cw - m, ch - m))
    out = Image.new("RGB", (S, S), (222, 212, 188))
    out.paste(im.resize((w, S), Image.LANCZOS), (0, 0))
    # the spine: a strip down the cover's middle, rounded by shading across it, its bands caught by the light
    a = np.asarray(im.crop((im.size[0] * 2 // 5, 0, im.size[0] * 3 // 5, im.size[1])).resize((spine, S), Image.LANCZOS), dtype=np.float32)
    across = np.linspace(-1.0, 1.0, spine)[None, :, None]
    a = a * (1.05 - 0.45 * across ** 2)
    for k in range(1, 6):
        y = S * k // 6
        for r in range(-22, 23):
            a[y + r] *= 1.0 + 0.5 * np.exp(-(r / 9.0) ** 2) - 0.35 * np.exp(-((abs(r) - 18) / 3.0) ** 2)
    out.paste(Image.fromarray(np.clip(a, 0, 255).astype(np.uint8)), (w, 0))
    # the pages: fine lines running along the block, a little uneven, darkening towards the boards
    rng = np.random.default_rng(int(a.mean() * 16))
    pw = S - w - spine
    lines = 0.86 + 0.14 * rng.random((1, pw)) * rng.random((1, pw))
    edge = 1.0 - 0.3 * np.abs(np.linspace(-1.0, 1.0, pw))[None, :] ** 6
    pg = np.repeat(lines * edge, S, axis=0)[..., None] * np.array([232.0, 222.0, 198.0])
    out.paste(Image.fromarray(pg.astype(np.uint8)), (w + spine, 0))
    return out


def books(src, dest):
    tex = os.path.join(dest, "textures")
    os.makedirs(tex, exist_ok=True)
    names = sorted(f for f in os.listdir(src) if f.lower().endswith((".png", ".jpg")))
    for i, f in enumerate(names):
        book_atlas(Image.open(os.path.join(src, f))).save(os.path.join(tex, f"book_{i}.jpg"), quality=92, subsampling=0)
        print(f"book_{i}: {f}")


def pack(src, dest, metal, rug=None, only=None):
    os.makedirs(dest, exist_ok=True)
    total = 0
    for name, files, scale in MODELS:
        if only and name not in only:
            continue
        meshes = [read_glb(os.path.join(src, f + ".glb")) for f in files]
        base = meshes[0]
        sets = []
        for i, m in enumerate(meshes):
            maps = open_set(m)
            if i > 0 and not np.allclose(base["T"][base["F"]], m["T"][m["F"]], atol=1e-4):
                print(f"  {files[i]}: redrawing its textures on {files[0]}'s layout", flush=True)
                maps = transfer(base, m, maps)
            sets.append((f"{name}_{i}", encode_set(*maps)))
        body, shift = to_metres(base, scale)
        extra = []
        for mname, panel in (("screen", computer_screen), ("diffuser", lamp_diffuser)):
            if (mname, name) in (("screen", "computer"), ("diffuser", "lamp")):
                p = panel(0.0)
                p["P"] = p["P"] * scale + shift
                extra.append((mname, p))
        size = write_glb(os.path.join(dest, name + ".glb"), name, body, sets, extra)
        total += size
        ext = body["P"].max(0) - body["P"].min(0)
        print(f"{name}: {len(body['F'])} triangles, {len(sets)} variant(s), {ext[0]:.3f} x {ext[1]:.3f} x {ext[2]:.3f} m, {size / 1e6:.1f} MB", flush=True)
    if metal:
        tex = os.path.join(dest, "textures")
        os.makedirs(tex, exist_ok=True)
        stem = os.path.join(metal, "Metal055A_2K-PNG_")
        S = (2048, 2048)
        Image.open(stem + "Color.png").convert("RGB").resize(S).save(os.path.join(tex, "metal_albedo.jpg"), quality=92)
        rough = Image.open(stem + "Roughness.png").convert("L").resize(S)
        metalness = Image.open(stem + "Metalness.png").convert("L").resize(S)
        Image.merge("RGB", (Image.new("L", S, 255), rough, metalness)).save(os.path.join(tex, "metal_orm.jpg"), quality=92, subsampling=0)
        im = Image.open(stem + "NormalGL.png")
        if im.mode == "I;16":
            im = Image.fromarray((np.asarray(im, dtype=np.float32) / 257.0).astype(np.uint8))
        im.convert("RGB").resize(S).save(os.path.join(tex, "metal_normal.jpg"), quality=95, subsampling=0)
        print("metal: textures written", flush=True)
    if rug:
        tex = os.path.join(dest, "textures")
        os.makedirs(tex, exist_ok=True)
        Image.open(os.path.join(rug, "albedo.png")).convert("RGB").save(os.path.join(tex, "rug_albedo.jpg"), quality=92)
        Image.open(os.path.join(rug, "normal.png")).convert("RGB").save(os.path.join(tex, "rug_normal.jpg"), quality=95, subsampling=0)
        print("rug: textures written", flush=True)
    print(f"{total / 1e6:.0f} MB of models in {dest}")


# -------------------------------------------------------------------- the check ---

def tris_hit(o, d, a, e1, e2):
    p = np.cross(d, e2)
    det = (e1 * p).sum(1)
    with np.errstate(divide="ignore", invalid="ignore"):
        inv = 1.0 / det
        s = o - a
        u = (s * p).sum(1) * inv
        q = np.cross(s, e1)
        v = (q @ d) * inv
        t = (e2 * q).sum(1) * inv
        ok = (np.abs(det) > 1e-14) & (u >= 0) & (v >= 0) & (u + v <= 1) & (t > 0)
    return t[ok], det[ok] > 0


def leak_share(mesh, rays=400, seed=3):
    """The share of rays through the model that capture.comp's plain count would leave stuck inside it
    (those a bake bounds, by BF_LEAKY or BF_OPEN)."""
    P, F = mesh["P"], mesh["F"]
    a = P[F[:, 0]]
    e1, e2 = P[F[:, 1]] - a, P[F[:, 2]] - a
    centre = (P.max(0) + P.min(0)) * 0.5
    radius = np.linalg.norm(P.max(0) - P.min(0)) * 0.5
    rng = np.random.default_rng(seed)
    bad = 0
    for _ in range(rays):
        d = rng.normal(size=3)
        d /= np.linalg.norm(d)
        o = centre - d * radius * 2 + (rng.normal(size=3) - d * 0.0) * radius * 0.25
        t, front = tris_hit(o, d, a, e1, e2)
        inside = 0
        limit = -1e30
        for tt, f in zip(t[np.argsort(t)].tolist(), front[np.argsort(t)].tolist()):
            if tt < limit:
                continue
            limit = tt + max(1e-4, tt * 1e-5)
            if f:
                inside += 1
            elif inside > 0:
                inside -= 1
        bad += inside != 0
    return bad / rays


def check(dest):
    failed = 0
    for name, files, scale in MODELS:
        path = os.path.join(dest, name + ".glb")
        if not os.path.exists(path):
            print(f"{name}: missing")
            failed += 1
            continue
        m = read_glb(path)
        P = m["P"]
        ok = abs(P[:, 1].min()) < 1e-3 and abs(P[:, 0].min() + P[:, 0].max()) < 1e-3 and abs(P[:, 2].min() + P[:, 2].max()) < 1e-3
        share = leak_share(m)
        print(f"{name}: {len(m['F'])} triangles, {len(m['images']) // 3} variant(s), stands on y=0 and centred: {ok}, rays stuck by a plain count: {share * 100:.1f}%")
        failed += not ok
    return 1 if failed else 0


def main():
    args = sys.argv[1:]
    dest = "assets_src/interiors"
    metal = None
    if "--dest" in args:
        i = args.index("--dest")
        dest = args[i + 1]
        del args[i:i + 2]
    if "--metal" in args:
        i = args.index("--metal")
        metal = args[i + 1]
        del args[i:i + 2]
    rug = only = None
    if "--rug" in args:
        i = args.index("--rug")
        rug = args[i + 1]
        del args[i:i + 2]
    if "--only" in args:
        i = args.index("--only")
        only = args[i + 1].split(",")
        del args[i:i + 2]
    if args and args[0] == "check":
        return check(dest)
    if len(args) == 2 and args[0] == "pack":
        pack(args[1], dest, metal, rug, only)
        return 0
    if len(args) == 2 and args[0] == "books":
        books(args[1], dest)
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
