#!/usr/bin/env python3
"""earth.py: the Earth elevation project tool.

Turns a global elevation model (ETOPO 2022 60 arc-second, or any GDAL raster) into the editable
project under data/earth/: 72 int16 GeoTIFF tiles of 30 x 30 degrees at 60", a manifest, and a
VRT mosaic that QGIS opens as one layer. Regions can be pulled out for editing and written back,
and the whole project is staged as one raw grid for the engine's bake tool (tools/earth_bake.c).

    earth.py import  <source.tif> <project> [--origin LAT LON [HEADING]] [--sha256 HEX]
    earth.py extract <project> <lon0> <lat0> <lon1> <lat1> <out.tif> [--layer height|hints]
    earth.py insert  <project> <edited.tif> [--feather N] [--layer height|hints] [--rewrite]
    earth.py check   <project>
    earth.py preview <project> <out.png> [--region lon0 lat0 lon1 lat1] [--width W]
    earth.py stage   <project> <out.r16>

The grid is fixed: EPSG:4326, 60" cells, pixel-is-area, 21600 x 10800, the north-west corner at
90N 180W. Tiles are named by their south-west corner (height/N30_W120.tif spans 30N..60N,
120W..90W). Elevations are whole metres above the source's datum (EGM2008 for ETOPO 2022), with
the sea floor as negative values and no nodata. The optional hints layer holds the same tiles at
8 bits, three bands: R landform override (0 none, 1 plains, 2 hills, 3 mountains, 4 plateau),
G detail roughness scale, B no-settlement mask.

Run it with the source treated as untrusted data: `python -I tools/earth/earth.py ...`.
"""

import argparse
import hashlib
import json
import math
import os
import sys

import numpy as np
import rasterio
from rasterio.crs import CRS
from rasterio.enums import Resampling
from rasterio.transform import Affine
from rasterio.vrt import WarpedVRT
from rasterio.windows import Window

FORMAT = 1
CELL_ARCSEC = 60
CELL = CELL_ARCSEC / 3600.0          # degrees per cell
TILE_DEG = 30
TILE_PX = int(round(TILE_DEG / CELL))  # 1800
TILES_X = 360 // TILE_DEG            # 12
TILES_Y = 180 // TILE_DEG            # 6
WIDTH = TILES_X * TILE_PX            # 21600
HEIGHT = TILES_Y * TILE_PX           # 10800
LON0, LAT0 = -180.0, 90.0
CRS4326 = CRS.from_epsg(4326)
METRES_PER_DEG = 1852.0 * 60.0       # along a meridian
DEFAULT_ORIGIN = {"lat": 34.0522, "lon": -118.2440, "heading_deg": 0.0}
DEFAULT_DATUM = "EGM2008 geoid"

LAYERS = {
    "height": {"dtype": "int16", "count": 1, "predictor": 2},
    "hints": {"dtype": "uint8", "count": 3, "predictor": 2},
}

MANIFEST = "earth.json"
VRT = "earth.vrt"


def fail(msg):
    sys.exit("earth.py: " + msg)


def global_transform():
    return Affine(CELL, 0.0, LON0, 0.0, -CELL, LAT0)


def tile_name(ti, tj):
    west = LON0 + TILE_DEG * ti
    south = LAT0 - TILE_DEG * (tj + 1)
    return "%s%02d_%s%03d" % ("N" if south >= 0 else "S", abs(int(south)),
                              "E" if west >= 0 else "W", abs(int(west)))


def tile_transform(ti, tj):
    return Affine(CELL, 0.0, LON0 + TILE_DEG * ti, 0.0, -CELL, LAT0 - TILE_DEG * tj)


def tile_profile(layer, width=TILE_PX, height=TILE_PX):
    spec = LAYERS[layer]
    return dict(driver="GTiff", dtype=spec["dtype"], count=spec["count"], width=width,
                height=height, crs=CRS4326, compress="deflate", predictor=spec["predictor"],
                tiled=True, blockxsize=256, blockysize=256, interleave="pixel", bigtiff="NO",
                nodata=None)


def write_geotiff(path, arr, transform, layer, tags=None):
    """Writes arr (bands, rows, cols) as a GeoTIFF on the project's grid. The writer is shared by
    import, insert and extract so that an unedited round trip is byte-identical."""
    if arr.ndim == 2:
        arr = arr[None]
    profile = tile_profile(layer, arr.shape[2], arr.shape[1])
    profile["transform"] = transform
    tmp = path + ".tmp"
    with rasterio.open(tmp, "w", **profile) as ds:
        ds.write(arr.astype(profile["dtype"]))
        ds.update_tags(AREA_OR_POINT="Area")
        if tags:
            ds.update_tags(**tags)
    os.replace(tmp, path)


def sha256_file(path, chunk=1 << 22):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def to_int16(arr):
    return np.clip(np.rint(arr), -32768, 32767).astype(np.int16)


# ----------------------------------------------------------------------------- the project

class Project:
    def __init__(self, root, manifest=None):
        self.root = root
        if manifest is None:
            path = os.path.join(root, MANIFEST)
            if not os.path.isfile(path):
                fail("no %s in %s (run import first)" % (MANIFEST, root))
            with open(path, "r", encoding="utf-8") as f:
                manifest = json.load(f)
        self.manifest = manifest
        if manifest.get("format") != FORMAT:
            fail("manifest format %r, expected %d" % (manifest.get("format"), FORMAT))

    def save(self):
        path = os.path.join(self.root, MANIFEST)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(self.manifest, f, indent=2)
            f.write("\n")

    def tile_path(self, layer, ti, tj):
        return os.path.join(self.root, layer, tile_name(ti, tj) + ".tif")

    def layer_tags(self, layer):
        if layer == "height":
            return {"UNIT": "m", "DATUM": self.manifest.get("datum", DEFAULT_DATUM)}
        return {"BANDS": "landform roughness nosettle"}

    def read_tile(self, layer, ti, tj, window=None):
        """One tile (bands, rows, cols). A missing hints tile reads as zeros."""
        spec = LAYERS[layer]
        path = self.tile_path(layer, ti, tj)
        if not os.path.isfile(path):
            if layer == "hints":
                h = TILE_PX if window is None else int(window.height)
                w = TILE_PX if window is None else int(window.width)
                return np.zeros((spec["count"], h, w), dtype=spec["dtype"])
            fail("missing tile " + path)
        with rasterio.open(path) as ds:
            return ds.read(window=window).astype(spec["dtype"])

    def read_region(self, layer, col0, row0, cols, rows):
        """A region of the mosaic in grid pixels, assembled from the tiles it crosses."""
        spec = LAYERS[layer]
        out = np.zeros((spec["count"], rows, cols), dtype=spec["dtype"])
        for ti, tj, tw, ow in self._tiles_over(col0, row0, cols, rows):
            out[:, ow.row_off:ow.row_off + ow.height, ow.col_off:ow.col_off + ow.width] = \
                self.read_tile(layer, ti, tj, tw)
        return out

    def write_region(self, layer, col0, row0, arr, mask=None, feather=0, rewrite=False):
        """Writes arr (bands, rows, cols) into the tiles it covers. Where mask is False the old
        value is kept, and the border of the region is feathered over `feather` pixels. Returns the
        tiles written (unchanged tiles are left alone unless rewrite is set)."""
        spec = LAYERS[layer]
        rows, cols = arr.shape[1], arr.shape[2]
        weight = feather_weights(rows, cols, feather)
        if mask is not None:
            weight = weight * mask.astype(np.float64)
        written = []
        for ti, tj, tw, ow in self._tiles_over(col0, row0, cols, rows):
            old = self.read_tile(layer, ti, tj)
            r0, r1 = int(tw.row_off), int(tw.row_off + tw.height)
            c0, c1 = int(tw.col_off), int(tw.col_off + tw.width)
            o0, o1 = int(ow.row_off), int(ow.row_off + ow.height)
            p0, p1 = int(ow.col_off), int(ow.col_off + ow.width)
            w = weight[o0:o1, p0:p1]
            new_part = arr[:, o0:o1, p0:p1].astype(np.float64)
            old_part = old[:, r0:r1, c0:c1].astype(np.float64)
            blended = old_part + w * (new_part - old_part)
            new = old.copy()
            if spec["dtype"] == "int16":
                new[:, r0:r1, c0:c1] = to_int16(blended)
            else:
                new[:, r0:r1, c0:c1] = np.clip(np.rint(blended), 0, 255).astype(np.uint8)
            if rewrite or not np.array_equal(new, old):
                os.makedirs(os.path.join(self.root, layer), exist_ok=True)
                write_geotiff(self.tile_path(layer, ti, tj), new, tile_transform(ti, tj), layer,
                              self.layer_tags(layer))
                written.append(tile_name(ti, tj))
        return written

    @staticmethod
    def _tiles_over(col0, row0, cols, rows):
        """Yields (ti, tj, window in the tile, window in the region) for the tiles a region crosses."""
        for tj in range(row0 // TILE_PX, (row0 + rows - 1) // TILE_PX + 1):
            for ti in range(col0 // TILE_PX, (col0 + cols - 1) // TILE_PX + 1):
                tc0, tr0 = ti * TILE_PX, tj * TILE_PX
                c0, c1 = max(col0, tc0), min(col0 + cols, tc0 + TILE_PX)
                r0, r1 = max(row0, tr0), min(row0 + rows, tr0 + TILE_PX)
                yield (ti, tj, Window(c0 - tc0, r0 - tr0, c1 - c0, r1 - r0),
                       Window(c0 - col0, r0 - row0, c1 - c0, r1 - r0))


def feather_weights(rows, cols, n):
    """1 inside, ramping to ~0 over n pixels at the region's border (all 1 when n is 0)."""
    if n <= 0:
        return np.ones((rows, cols), dtype=np.float64)
    i = np.arange(rows, dtype=np.float64)
    j = np.arange(cols, dtype=np.float64)
    wr = np.minimum(np.minimum(i + 1, rows - i) / (n + 1), 1.0)
    wc = np.minimum(np.minimum(j + 1, cols - j) / (n + 1), 1.0)
    return wr[:, None] * wc[None, :]


def region_to_pixels(lon0, lat0, lon1, lat1):
    """A lon/lat box (any corner order), snapped outwards to the grid. Returns (col0, row0, cols, rows)."""
    west, east = min(lon0, lon1), max(lon0, lon1)
    south, north = min(lat0, lat1), max(lat0, lat1)
    if not (-180 <= west < east <= 180) or not (-90 <= south < north <= 90):
        fail("region out of range: lon %g..%g lat %g..%g" % (west, east, south, north))
    eps = 1e-9
    col0 = max(0, int(math.floor((west - LON0) / CELL + eps)))
    col1 = min(WIDTH, int(math.ceil((east - LON0) / CELL - eps)))
    row0 = max(0, int(math.floor((LAT0 - north) / CELL + eps)))
    row1 = min(HEIGHT, int(math.ceil((LAT0 - south) / CELL - eps)))
    return col0, row0, col1 - col0, row1 - row0


def pixels_transform(col0, row0):
    return Affine(CELL, 0.0, LON0 + col0 * CELL, 0.0, -CELL, LAT0 - row0 * CELL)


def write_vrt(project):
    srs = CRS4326.to_wkt()
    lines = ['<VRTDataset rasterXSize="%d" rasterYSize="%d">' % (WIDTH, HEIGHT),
             '  <SRS dataAxisToSRSAxisMapping="2,1">%s</SRS>' % srs,
             '  <GeoTransform>%r, %r, 0.0, %r, 0.0, %r</GeoTransform>' % (LON0, CELL, LAT0, -CELL),
             '  <VRTRasterBand dataType="Int16" band="1">',
             '    <Description>elevation (m)</Description>']
    for tj in range(TILES_Y):
        for ti in range(TILES_X):
            lines += ['    <SimpleSource>',
                      '      <SourceFilename relativeToVRT="1">height/%s.tif</SourceFilename>' % tile_name(ti, tj),
                      '      <SourceBand>1</SourceBand>',
                      '      <SourceProperties RasterXSize="%d" RasterYSize="%d" DataType="Int16" '
                      'BlockXSize="256" BlockYSize="256" />' % (TILE_PX, TILE_PX),
                      '      <SrcRect xOff="0" yOff="0" xSize="%d" ySize="%d" />' % (TILE_PX, TILE_PX),
                      '      <DstRect xOff="%d" yOff="%d" xSize="%d" ySize="%d" />'
                      % (ti * TILE_PX, tj * TILE_PX, TILE_PX, TILE_PX),
                      '    </SimpleSource>']
    lines += ['  </VRTRasterBand>', '</VRTDataset>', '']
    with open(os.path.join(project.root, VRT), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


# ----------------------------------------------------------------------------- import

def cmd_import(args):
    src_path = args.source
    if not os.path.isfile(src_path):
        fail("no such source " + src_path)
    print("hashing source ...", flush=True)
    digest = sha256_file(src_path)
    expected = args.sha256
    sidecar = src_path + ".sha256"
    if expected is None and os.path.isfile(sidecar):
        with open(sidecar, "r", encoding="utf-8") as f:
            expected = f.read().split()[0]
    if expected is not None and expected.lower() != digest:
        fail("source SHA-256 %s does not match the expected %s" % (digest, expected))
    print("source sha256 " + digest)

    os.makedirs(os.path.join(args.project, "height"), exist_ok=True)
    manifest = {
        "format": FORMAT,
        "crs": "EPSG:4326",
        "pixel": "area",
        "cell_arcsec": CELL_ARCSEC,
        "tile_deg": TILE_DEG,
        "tile_px": TILE_PX,
        "width": WIDTH,
        "height": HEIGHT,
        "lon0": LON0,
        "lat0": LAT0,
        "dtype": "int16",
        "unit": "m",
        "datum": args.datum,
        "source": {
            "file": os.path.basename(src_path),
            "bytes": os.path.getsize(src_path),
            "sha256": digest,
        },
        "origin": {"lat": args.origin[0], "lon": args.origin[1], "heading_deg": args.origin[2]},
        "layers": {"height": "height/<SW corner>.tif, int16 metres",
                   "hints": "optional hints/<SW corner>.tif, uint8 x3: landform, roughness, nosettle"},
        "tiles": [tile_name(ti, tj) for tj in range(TILES_Y) for ti in range(TILES_X)],
    }
    project = Project(args.project, manifest)

    with rasterio.open(src_path) as src:
        same_grid = (src.width == WIDTH and src.height == HEIGHT and src.crs is not None
                     and src.crs.is_geographic
                     and src.transform.almost_equals(global_transform(), precision=1e-9))
        if same_grid:
            print("source is on the project grid; reading it directly")
            reader = src
        else:
            print("source is not on the project grid; warping to it (area average)")
            reader = WarpedVRT(src, crs=CRS4326, transform=global_transform(), width=WIDTH,
                               height=HEIGHT, resampling=Resampling.average)
        nodata = reader.nodata
        bad = 0
        lo, hi = None, None
        for tj in range(TILES_Y):
            for ti in range(TILES_X):
                win = Window(ti * TILE_PX, tj * TILE_PX, TILE_PX, TILE_PX)
                data = reader.read(1, window=win).astype(np.float64)
                if nodata is not None:
                    miss = data == nodata
                else:
                    miss = ~np.isfinite(data)
                if miss.any():
                    bad += int(miss.sum())
                    if args.fill is None:
                        fail("%d nodata pixels in tile %s; pass --fill METRES to fill them"
                             % (int(miss.sum()), tile_name(ti, tj)))
                    data[miss] = args.fill
                tile = to_int16(data)
                lo = tile.min() if lo is None else min(lo, tile.min())
                hi = tile.max() if hi is None else max(hi, tile.max())
                write_geotiff(project.tile_path("height", ti, tj), tile, tile_transform(ti, tj),
                              "height", project.layer_tags("height"))
            print("  tile row %d/%d written" % (tj + 1, TILES_Y), flush=True)
        if not same_grid:
            reader.close()
    project.save()
    write_vrt(project)
    print("wrote %d tiles to %s (min %d m, max %d m%s)" % (
        TILES_X * TILES_Y, os.path.join(args.project, "height"), lo, hi,
        ", %d nodata pixels filled" % bad if bad else ""))


# ----------------------------------------------------------------------------- extract / insert

def cmd_extract(args):
    project = Project(args.project)
    col0, row0, cols, rows = region_to_pixels(args.lon0, args.lat0, args.lon1, args.lat1)
    arr = project.read_region(args.layer, col0, row0, cols, rows)
    write_geotiff(args.out, arr, pixels_transform(col0, row0), args.layer,
                  project.layer_tags(args.layer))
    t = pixels_transform(col0, row0)
    print("wrote %s: %d x %d px, lon %.6g..%.6g lat %.6g..%.6g" % (
        args.out, cols, rows, t.c, t.c + cols * CELL, t.f - rows * CELL, t.f))


def grid_position(ds):
    """The dataset's top-left corner as integer grid pixels, or a reason it does not fit the grid."""
    t = ds.transform
    if ds.crs is None or not ds.crs.is_geographic:
        return None, "CRS is %s, not geographic (EPSG:4326)" % (ds.crs.to_string() if ds.crs else "missing")
    if abs(t.a - CELL) > 1e-9 or abs(t.e + CELL) > 1e-9 or t.b != 0 or t.d != 0:
        return None, "resolution is %g x %g deg, not %g (60 arc-seconds)" % (t.a, -t.e, CELL)
    fc = (t.c - LON0) / CELL
    fr = (LAT0 - t.f) / CELL
    if abs(fc - round(fc)) > 1e-6 or abs(fr - round(fr)) > 1e-6:
        return None, "pixel edges are not aligned to the project grid"
    c0, r0 = int(round(fc)), int(round(fr))
    if c0 < 0 or r0 < 0 or c0 + ds.width > WIDTH or r0 + ds.height > HEIGHT:
        return None, "the raster reaches outside the global grid"
    return (c0, r0), None


def cmd_insert(args):
    project = Project(args.project)
    spec = LAYERS[args.layer]
    with rasterio.open(args.edited) as ds:
        pos, why = grid_position(ds)
        if pos is None:
            fail("%s: %s" % (args.edited, why))
        if ds.count != spec["count"]:
            fail("%s has %d bands; the %s layer takes %d" % (args.edited, ds.count, args.layer, spec["count"]))
        if ds.crs.to_epsg() not in (4326, 4979, 9518):
            print("note: CRS is %s; treating it as EPSG:4326" % ds.crs.to_string())
        data = ds.read().astype(np.float64)
        mask = np.ones(data.shape[1:], dtype=bool)
        if ds.nodata is not None:
            mask &= ~np.any(data == ds.nodata, axis=0)
        mask &= np.all(np.isfinite(data), axis=0)
        if not mask.all():
            print("%d nodata pixels keep their current values" % int((~mask).sum()))
    col0, row0 = pos
    written = project.write_region(args.layer, col0, row0, data, mask=mask, feather=args.feather,
                                   rewrite=args.rewrite)
    if written:
        print("updated %d tile(s): %s" % (len(written), " ".join(written)))
    else:
        print("no tile changed")


# ----------------------------------------------------------------------------- check

def cmd_check(args):
    project = Project(args.project)
    m = project.manifest
    problems = []

    def problem(msg):
        problems.append(msg)
        print("PROBLEM: " + msg)

    for key, want in (("width", WIDTH), ("height", HEIGHT), ("tile_px", TILE_PX), ("tile_deg", TILE_DEG),
                      ("cell_arcsec", CELL_ARCSEC), ("lon0", LON0), ("lat0", LAT0), ("dtype", "int16")):
        if m.get(key) != want:
            problem("manifest %s is %r, expected %r" % (key, m.get(key), want))
    names = [tile_name(ti, tj) for tj in range(TILES_Y) for ti in range(TILES_X)]
    if m.get("tiles") != names:
        problem("manifest tile list does not match the 12 x 6 grid")
    origin = m.get("origin", {})
    if not all(isinstance(origin.get(k), (int, float)) for k in ("lat", "lon", "heading_deg")):
        problem("manifest origin needs numeric lat, lon and heading_deg")

    for layer in ("height", "hints"):
        folder = os.path.join(project.root, layer)
        if not os.path.isdir(folder):
            if layer == "height":
                problem("no height/ folder")
            continue
        present = sorted(f for f in os.listdir(folder) if f.lower().endswith(".tif"))
        extra = sorted(set(present) - set(n + ".tif" for n in names))
        if extra:
            problem("%s/ has files not on the grid: %s" % (layer, " ".join(extra)))
        spec = LAYERS[layer]
        for tj in range(TILES_Y):
            for ti in range(TILES_X):
                name = tile_name(ti, tj)
                path = project.tile_path(layer, ti, tj)
                if not os.path.isfile(path):
                    if layer == "height":
                        problem("missing tile height/%s.tif" % name)
                    continue
                with rasterio.open(path) as ds:
                    if (ds.width, ds.height) != (TILE_PX, TILE_PX):
                        problem("%s/%s is %d x %d px" % (layer, name, ds.width, ds.height))
                    if ds.count != spec["count"] or ds.dtypes[0] != spec["dtype"]:
                        problem("%s/%s is %d x %s, expected %d x %s" % (
                            layer, name, ds.count, ds.dtypes[0], spec["count"], spec["dtype"]))
                    if ds.crs is None or ds.crs.to_epsg() != 4326:
                        problem("%s/%s CRS is %s" % (layer, name, ds.crs.to_string() if ds.crs else "missing"))
                    if not ds.transform.almost_equals(tile_transform(ti, tj), precision=1e-9):
                        problem("%s/%s georeferencing is off" % (layer, name))
                    if ds.nodata is not None:
                        problem("%s/%s has a nodata value (%r)" % (layer, name, ds.nodata))
                    if ds.profile.get("compress", "").lower() != "deflate":
                        print("note: %s/%s is not DEFLATE compressed" % (layer, name))

    vrt = os.path.join(project.root, VRT)
    if not os.path.isfile(vrt):
        problem("no " + VRT)
    elif not problems:
        with rasterio.open(vrt) as ds:
            if (ds.width, ds.height) != (WIDTH, HEIGHT) or ds.dtypes[0] != "int16":
                problem("%s is %d x %d %s" % (VRT, ds.width, ds.height, ds.dtypes[0]))

    if problems:
        print("%d problem(s)" % len(problems))
        sys.exit(1)

    # Statistics over the whole mosaic, strip by strip, with the seams between strips included.
    lo = (None, None)
    hi = (None, None)
    steep = (0.0, None)
    steep85 = (0.0, None)
    below = 0
    prev_row = None
    for tj in range(TILES_Y):
        strip = project.read_region("height", 0, tj * TILE_PX, WIDTH, TILE_PX)[0]
        r0 = tj * TILE_PX
        i = int(np.argmin(strip))
        j = int(np.argmax(strip))
        if lo[0] is None or strip.flat[i] < lo[0]:
            lo = (int(strip.flat[i]), (r0 + i // WIDTH, i % WIDTH))
        if hi[0] is None or strip.flat[j] > hi[0]:
            hi = (int(strip.flat[j]), (r0 + j // WIDTH, j % WIDTH))
        below += int((strip < 0).sum())
        block = strip if prev_row is None else np.vstack([prev_row[None], strip])
        b0 = r0 if prev_row is None else r0 - 1
        rows = np.arange(block.shape[0]) + b0
        lat = LAT0 - (rows + 0.5) * CELL
        dy = METRES_PER_DEG * CELL
        dx = dy * np.maximum(np.cos(np.radians(lat)), 1e-12)
        ns = np.abs(np.diff(block.astype(np.float64), axis=0)) / dy
        ew = np.abs(np.diff(np.concatenate([block, block[:, :1]], axis=1).astype(np.float64), axis=1)) / dx[:, None]
        for grad, roff in ((ns, 0), (ew, 0)):
            k = int(np.argmax(grad))
            v = float(grad.flat[k])
            where = (b0 + k // grad.shape[1], k % grad.shape[1])
            if v > steep[0]:
                steep = (v, where)
            keep = np.abs(lat) <= 85.0
            if grad is ew:
                sel = grad[keep]
                base = rows[keep]
            else:
                sel = grad[keep[:-1]]
                base = rows[:-1][keep[:-1]]
            if sel.size:
                k = int(np.argmax(sel))
                v = float(sel.flat[k])
                if v > steep85[0]:
                    steep85 = (v, (int(base[k // sel.shape[1]]), k % sel.shape[1]))
        prev_row = strip[-1].copy()

    def place(rc):
        r, c = rc
        return "%.3f%s %.3f%s" % (abs(LAT0 - (r + 0.5) * CELL), "N" if LAT0 - (r + 0.5) * CELL >= 0 else "S",
                                  abs(LON0 + (c + 0.5) * CELL), "E" if LON0 + (c + 0.5) * CELL >= 0 else "W")

    print("ok: %d tiles, %d x %d px at %d\", int16 metres, origin %.4f %.4f heading %g" % (
        TILES_X * TILES_Y, WIDTH, HEIGHT, CELL_ARCSEC, origin["lat"], origin["lon"], origin["heading_deg"]))
    print("minimum %d m at %s" % (lo[0], place(lo[1])))
    print("maximum %d m at %s" % (hi[0], place(hi[1])))
    print("below sea level: %.1f%% of cells" % (100.0 * below / (WIDTH * HEIGHT)))
    print("highest slope %.3f (%.1f%%) at %s" % (steep[0], 100 * steep[0], place(steep[1])))
    print("highest slope between 85S and 85N %.3f (%.1f%%) at %s" % (
        steep85[0], 100 * steep85[0], place(steep85[1])))


# ----------------------------------------------------------------------------- preview

def hillshade(z, dx, dy, azimuth_deg=315.0, altitude_deg=45.0, exaggerate=1.0):
    gy, gx = np.gradient(z * exaggerate, dy, axis=0), np.gradient(z * exaggerate, axis=1) / dx[:, None]
    slope = np.arctan(np.hypot(gx, gy))
    aspect = np.arctan2(-gx, gy)
    zen = np.radians(90.0 - altitude_deg)
    az = np.radians(azimuth_deg)
    shade = np.cos(zen) * np.cos(slope) + np.sin(zen) * np.sin(slope) * np.cos(az - aspect)
    return np.clip(shade, 0.0, 1.0)


def colour_ramp(z, stops):
    """Linear ramp through (value, (r, g, b)) stops, as float arrays 0..255."""
    vals = np.array([s[0] for s in stops], dtype=np.float64)
    cols = np.array([s[1] for s in stops], dtype=np.float64)
    out = np.empty(z.shape + (3,), dtype=np.float64)
    for k in range(3):
        out[..., k] = np.interp(z, vals, cols[:, k])
    return out


def cmd_preview(args):
    project = Project(args.project)
    if args.region:
        col0, row0, cols, rows = region_to_pixels(*args.region)
    else:
        col0, row0, cols, rows = 0, 0, WIDTH, HEIGHT
    f = max(1, int(math.ceil(cols / float(args.width))))
    cols -= cols % f
    rows -= rows % f
    if cols == 0 or rows == 0:
        fail("region too small for that width")
    z = project.read_region("height", col0, row0, cols, rows)[0].astype(np.float64)
    if f > 1:
        z = z.reshape(rows // f, f, cols // f, f).mean(axis=(1, 3))
    lat = LAT0 - (row0 + (np.arange(z.shape[0]) + 0.5) * f) * CELL
    dy = METRES_PER_DEG * CELL * f
    dx = dy * np.maximum(np.cos(np.radians(lat)), 0.05)
    shade = hillshade(z, dx, dy, exaggerate=args.exaggerate)
    land = colour_ramp(z, [(0, (86, 128, 72)), (300, (132, 150, 84)), (900, (160, 136, 90)),
                           (1800, (150, 118, 92)), (3000, (176, 168, 160)), (4500, (236, 236, 236)),
                           (9000, (255, 255, 255))])
    sea = colour_ramp(z, [(-11000, (12, 24, 56)), (-6000, (20, 44, 92)), (-3000, (34, 70, 128)),
                          (-1000, (56, 104, 160)), (-200, (84, 138, 184)), (0, (120, 170, 200))])
    rgb = np.where((z < 0)[..., None], sea, land)
    rgb = rgb * (0.35 + 0.65 * shade)[..., None]
    img = np.clip(np.rint(rgb), 0, 255).astype(np.uint8).transpose(2, 0, 1)
    t = Affine(CELL * f, 0.0, LON0 + col0 * CELL, 0.0, -CELL * f, LAT0 - row0 * CELL)
    with rasterio.open(args.out, "w", driver="PNG", dtype="uint8", count=3, width=img.shape[2],
                       height=img.shape[1], crs=CRS4326, transform=t) as ds:
        ds.write(img)
    for side in (".aux.xml",):
        if os.path.isfile(args.out + side):
            os.remove(args.out + side)
    print("wrote %s: %d x %d px (1 px = %d cells)" % (args.out, img.shape[2], img.shape[1], f))


# ----------------------------------------------------------------------------- stage

def cmd_stage(args):
    project = Project(args.project)
    m = project.manifest
    h = hashlib.sha256()
    with open(args.out, "wb") as f:
        for tj in range(TILES_Y):
            strip = project.read_region("height", 0, tj * TILE_PX, WIDTH, TILE_PX)[0]
            b = np.ascontiguousarray(strip, dtype="<i2").tobytes()
            h.update(b)
            f.write(b)
    origin = m["origin"]
    lines = [
        "format earth-stage-1",
        "width %d" % WIDTH,
        "height %d" % HEIGHT,
        "dtype int16",
        "byte_order little",
        "row_order north_to_south",
        "pixel area",
        "crs EPSG:4326",
        "lon0 %r" % LON0,
        "lat0 %r" % LAT0,
        "cell_deg %r" % CELL,
        "cell_arcsec %d" % CELL_ARCSEC,
        "unit m",
        "datum %s" % m.get("datum", DEFAULT_DATUM),
        "origin_lat %r" % origin["lat"],
        "origin_lon %r" % origin["lon"],
        "origin_heading_deg %r" % origin["heading_deg"],
        "source_file %s" % m["source"]["file"],
        "source_sha256 %s" % m["source"]["sha256"],
        "stage_sha256 %s" % h.hexdigest(),
    ]
    with open(args.out + ".txt", "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote %s (%d bytes) and %s.txt" % (args.out, WIDTH * HEIGHT * 2, args.out))


# ----------------------------------------------------------------------------- main

def main(argv=None):
    p = argparse.ArgumentParser(prog="earth.py", description=__doc__.split("\n\n")[1],
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("import", help="source raster to project tiles, manifest and VRT")
    s.add_argument("source")
    s.add_argument("project")
    s.add_argument("--origin", nargs="+", type=float, metavar="DEG",
                   default=[DEFAULT_ORIGIN["lat"], DEFAULT_ORIGIN["lon"], DEFAULT_ORIGIN["heading_deg"]],
                   help="megacity LAT LON [HEADING], default %(default)s")
    s.add_argument("--sha256", help="expected source checksum (default: the .sha256 sidecar if present)")
    s.add_argument("--fill", type=float, help="metres to put where the source has nodata")
    s.add_argument("--datum", default=DEFAULT_DATUM, help="vertical datum recorded in the manifest")
    s.set_defaults(fn=cmd_import)

    s = sub.add_parser("extract", help="one GeoTIFF of a region for editing")
    s.add_argument("project")
    s.add_argument("lon0", type=float)
    s.add_argument("lat0", type=float)
    s.add_argument("lon1", type=float)
    s.add_argument("lat1", type=float)
    s.add_argument("out")
    s.add_argument("--layer", choices=sorted(LAYERS), default="height")
    s.set_defaults(fn=cmd_extract)

    s = sub.add_parser("insert", help="write an edited region back into the tiles it covers")
    s.add_argument("project")
    s.add_argument("edited")
    s.add_argument("--layer", choices=sorted(LAYERS), default="height")
    s.add_argument("--feather", type=int, default=0, metavar="N", help="blend over N px at the border")
    s.add_argument("--rewrite", action="store_true", help="rewrite touched tiles even if unchanged")
    s.set_defaults(fn=cmd_insert)

    s = sub.add_parser("check", help="validate the project and print its statistics")
    s.add_argument("project")
    s.set_defaults(fn=cmd_check)

    s = sub.add_parser("preview", help="hillshade PNG with the sea floor tinted")
    s.add_argument("project")
    s.add_argument("out")
    s.add_argument("--region", nargs=4, type=float, metavar=("LON0", "LAT0", "LON1", "LAT1"))
    s.add_argument("--width", type=int, default=2160, help="about this many pixels across")
    s.add_argument("--exaggerate", type=float, default=1.0, help="vertical exaggeration for the shading")
    s.set_defaults(fn=cmd_preview)

    s = sub.add_parser("stage", help="the whole project as one raw int16 grid for earth_bake")
    s.add_argument("project")
    s.add_argument("out")
    s.set_defaults(fn=cmd_stage)

    args = p.parse_args(argv)
    if args.cmd == "import":
        if len(args.origin) == 2:
            args.origin.append(0.0)
        if len(args.origin) != 3:
            p.error("--origin takes LAT LON [HEADING]")
    args.fn(args)


if __name__ == "__main__":
    main()
