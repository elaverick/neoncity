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
    earth.py stage   <project> <out.r16>     (and <out.r16>.hints, .water and .runoff, if the project has them)
    earth.py cities  <GHS_UCDB_GLOBE_R2024A.gpkg> <list.csv> [--sha256 HEX]
    earth.py water   <HydroRIVERS_v10.shp> <HydroLAKES_polys_v10.shp> <project> [--min-discharge M3S]
                     [--min-lake KM2] [--tolerance M]
    earth.py runoff  <HydroRIVERS_v10.shp> <project>

The grid is fixed: EPSG:4326, 60" cells, pixel-is-area, 21600 x 10800, the north-west corner at
90N 180W. Tiles are named by their south-west corner (height/N30_W120.tif spans 30N..60N,
120W..90W). Elevations are whole metres above the source's datum (EGM2008 for ETOPO 2022), with
the sea floor as negative values and no nodata. The optional hints layer holds the same tiles at
8 bits, three bands: R landform override (0 none, 1 plains, 2 hills, 3 mountains, 4 plateau),
G detail roughness scale, B no-settlement mask. The optional water layer (water/*.gpkg) holds the
major rivers as lines with a width and the lakes as polygons, from HydroSHEDS; the optional runoff
(runoff/runoff.tif, 0.25 degrees, L/s/km2, from HydroRIVERS) sets how much water the derived
streams (tools/earth_streams.c) carry.

Run it with the source treated as untrusted data: `python -I tools/earth/earth.py ...`.
"""

import argparse
import csv
import hashlib
import json
import math
import os
import sqlite3
import struct
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
    # The hints, if any tile exists: the same grid at 8 bits, three bands interleaved per cell.
    hints = None
    folder = os.path.join(project.root, "hints")
    if os.path.isdir(folder) and any(n.endswith(".tif") for n in os.listdir(folder)):
        hints = args.out + ".hints"
        hh = hashlib.sha256()
        with open(hints, "wb") as f:
            for tj in range(TILES_Y):
                strip = project.read_region("hints", 0, tj * TILE_PX, WIDTH, TILE_PX)
                b = np.ascontiguousarray(np.moveaxis(strip, 0, -1), dtype="u1").tobytes()
                hh.update(b)
                f.write(b)
    # The water, if the project has any: the rivers' lines and the lakes' rings.
    water = args.out + ".water"
    wh = stage_water(project, water)
    if wh is None and os.path.exists(water):
        os.remove(water)
    # The runoff, if the project has it (for earth_streams; the bake ignores it).
    runoff = args.out + ".runoff"
    rh = stage_runoff(project, runoff)
    if rh is None and os.path.exists(runoff):
        os.remove(runoff)
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
    if hints:
        lines += ["hints_file %s" % os.path.basename(hints), "hints_sha256 %s" % hh.hexdigest()]
    if wh:
        lines += ["water_file %s" % os.path.basename(water), "water_sha256 %s" % wh]
    if rh:
        lines += ["runoff_file %s" % os.path.basename(runoff), "runoff_sha256 %s" % rh]
    with open(args.out + ".txt", "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote %s (%d bytes)%s and %s.txt" % (args.out, WIDTH * HEIGHT * 2,
                                             ", %s (%d bytes)" % (hints, WIDTH * HEIGHT * 3) if hints else "", args.out))


# ----------------------------------------------------------------------------- cities
# The city list's sizes from the GHSL Urban Centre Database (a GeoPackage: SQLite, its polygons in
# ESRI:54009, Mollweide on the WGS 84 sphere). Each row is matched to the urban centre of at least
# CITY_MIN_PEOPLE holding its coordinates, else to the nearest such within CITY_NEAR_KM (a listed
# city's point may fall in a small suburb beside its own centre), and gains that centre's id, name,
# area and population (2025 epoch); a row with none gets empty columns (Perm: the database has no
# centre for it). Several rows can share a centre (Guangzhou, Shenzhen, Foshan, Dongguan). The
# engine sizes the megacities from the area.

UCDB_TABLE = "GHSL_UCDB_THEME_GENERAL_CHARACTERISTICS_GLOBE_R2024A"
MOLL_R = 6378137.0
CITY_NEAR_KM = 30.0
CITY_MIN_PEOPLE = 300000
CITY_COLUMNS = ["rank", "name", "latitude", "longitude", "ghsl_id", "ghsl_name", "area_km2", "population", "ghsl_km"]


def moll_forward(lat, lon):
    phi, lam = math.radians(lat), math.radians(lon)
    if abs(abs(phi) - math.pi / 2) < 1e-12:
        th = math.copysign(math.pi / 2, phi)
    else:
        th = phi
        for _ in range(50):   # 2 th + sin 2 th = pi sin phi, by Newton
            d = (2 * th + math.sin(2 * th) - math.pi * math.sin(phi)) / (2 + 2 * math.cos(2 * th))
            th -= d
            if abs(d) < 1e-14:
                break
    return 2 * math.sqrt(2) / math.pi * MOLL_R * lam * math.cos(th), math.sqrt(2) * MOLL_R * math.sin(th)


def moll_inverse(x, y):
    th = np.arcsin(np.clip(y / (math.sqrt(2) * MOLL_R), -1, 1))
    lat = np.degrees(np.arcsin(np.clip((2 * th + np.sin(2 * th)) / math.pi, -1, 1)))
    lon = np.degrees(math.pi * x / (2 * math.sqrt(2) * MOLL_R * np.cos(th)))
    return lat, lon


def gpkg_rings(blob):
    """The rings of a GeoPackage (multi)polygon blob, or the lines of a (multi)linestring blob,
    each an (n, 2) array of x, y (any z or m dropped)."""
    if blob[:2] != b"GP":
        fail("a geometry isn't a GeoPackage blob")
    flags = blob[3]
    env = (0, 32, 48, 48, 64)[(flags >> 1) & 7]
    wkb, rings = memoryview(blob)[8 + env:], []

    def points(off, bo, dims):
        n = struct.unpack_from(bo + "I", wkb, off)[0]
        off += 4
        rings.append(np.frombuffer(wkb, dtype=bo + "f8", count=dims * n, offset=off).reshape(n, dims)[:, :2])
        return off + 8 * dims * n

    def geom(off):
        bo = "<" if wkb[off] == 1 else ">"
        code = struct.unpack_from(bo + "I", wkb, off + 1)[0]
        kind, dims = code % 1000, (2, 3, 3, 4)[min(code // 1000, 3)]
        off += 5
        if kind == 2:
            return points(off, bo, dims)
        if kind == 3:
            nr = struct.unpack_from(bo + "I", wkb, off)[0]
            off += 4
            for _ in range(nr):
                off = points(off, bo, dims)
            return off
        if kind in (5, 6):
            ng = struct.unpack_from(bo + "I", wkb, off)[0]
            off += 4
            for _ in range(ng):
                off = geom(off)
            return off
        fail("a geometry is neither a line nor a polygon (WKB type %d)" % code)

    geom(0)
    return rings


def inside_rings(rings, x, y):
    """Even-odd: whether (x, y) lies inside the rings (holes count against)."""
    inside = False
    for r in rings:
        x0, y0, x1, y1 = r[:-1, 0], r[:-1, 1], r[1:, 0], r[1:, 1]
        cross = (y0 > y) != (y1 > y)
        xs = x0 + (y - y0) * (x1 - x0) / np.where(y1 != y0, y1 - y0, 1.0)
        inside ^= bool(np.count_nonzero(cross & (x < xs)) & 1)
    return inside


def sphere_km(lat0, lon0, lat1, lon1):
    p0, p1, dl = np.radians(lat0), np.radians(lat1), np.radians(np.asarray(lon1) - lon0)
    c = np.sin(p0) * np.sin(p1) + np.cos(p0) * np.cos(p1) * np.cos(dl)
    return 6371.0088 * np.arccos(np.clip(c, -1, 1))


def ring_km(rings, lat, lon):
    """The distance from (lat, lon) to the nearest point of the rings' edges (km), each edge
    sampled every ~100 m in Mollweide and measured over the sphere."""
    best = 1e300
    for r in rings:
        seg = np.hypot(np.diff(r[:, 0]), np.diff(r[:, 1]))
        steps = np.maximum(1, np.ceil(seg / 100.0)).astype(int)
        t = np.concatenate([np.arange(s) / s for s in steps])
        k = np.repeat(np.arange(len(seg)), steps)
        px, py = r[k, 0] + t * (r[k + 1, 0] - r[k, 0]), r[k, 1] + t * (r[k + 1, 1] - r[k, 1])
        plat, plon = moll_inverse(px, py)
        best = min(best, float(np.min(sphere_km(lat, lon, plat, plon))))
    return best


def cmd_cities(args):
    src = args.ucdb
    if not os.path.isfile(src):
        fail("no such database " + src)
    print("hashing source ...", flush=True)
    digest = sha256_file(src)
    expected = args.sha256
    sidecar = src + ".sha256"
    if expected is None and os.path.isfile(sidecar):
        with open(sidecar, "r", encoding="utf-8") as f:
            expected = f.read().split()[0]
    if expected is not None and expected.lower() != digest:
        fail("source SHA-256 %s does not match the expected %s" % (digest, expected))
    print("source sha256 " + digest)
    sys.stdout.reconfigure(errors="replace")   # the centres' names, on a console that can't show them

    with open(args.list, "r", encoding="utf-8", newline="") as f:
        rows = list(csv.reader(f))
    if not rows or [c.strip() for c in rows[0][:4]] != CITY_COLUMNS[:4]:
        fail(args.list + " doesn't begin rank,name,latitude,longitude")
    cities = [(int(r[0]), r[1], float(r[2]), float(r[3])) for r in rows[1:] if r]

    db = sqlite3.connect("file:" + src.replace("\\", "/") + "?mode=ro", uri=True)
    srs = db.execute("select srs_id from gpkg_geometry_columns where table_name = ?", (UCDB_TABLE,)).fetchone()
    if not srs or srs[0] != 54009:
        fail("the database has no %s in ESRI:54009" % UCDB_TABLE)
    centres = db.execute('select ID_UC_G0, GC_UCN_MAI_2025, GC_UCA_KM2_2025, GC_POP_TOT_2025, geom from "%s"' % UCDB_TABLE).fetchall()
    print("%d urban centres" % len(centres), flush=True)
    # each centre's box in Mollweide, and its middle's latitude and longitude
    boxes, mids = [], []
    for c in centres:
        flags = c[4][3]
        if (flags >> 1) & 7 == 0:
            fail("an urban centre's geometry has no envelope")
        x0, x1, y0, y1 = struct.unpack_from("<dddd" if flags & 1 else ">dddd", c[4], 8)
        boxes.append((x0, x1, y0, y1))
        mids.append(moll_inverse(np.array(0.5 * (x0 + x1)), np.array(0.5 * (y0 + y1))))
    boxes = np.array(boxes)
    mlat = np.array([float(m[0]) for m in mids])
    mlon = np.array([float(m[1]) for m in mids])

    out, far, unmatched = [], 0, 0
    for (rank, name, lat, lon), row in zip(cities, rows[1:]):
        x, y = moll_forward(lat, lon)
        match, km = None, 0.0
        for k in np.nonzero((boxes[:, 0] <= x) & (x <= boxes[:, 1]) & (boxes[:, 2] <= y) & (y <= boxes[:, 3]))[0]:
            if centres[k][3] >= CITY_MIN_PEOPLE and inside_rings(gpkg_rings(centres[k][4]), x, y):
                match = k
                break
        if match is None:
            # the nearest edge among the big enough centres whose middle lies within reach of it
            near = np.nonzero(sphere_km(lat, lon, mlat, mlon) < CITY_NEAR_KM + 150.0)[0]
            for k in near:
                if centres[k][3] < CITY_MIN_PEOPLE:
                    continue
                d = ring_km(gpkg_rings(centres[k][4]), lat, lon)
                if d < CITY_NEAR_KM and (match is None or d < km):
                    match, km = k, d
        if match is None:
            unmatched += 1
            print("  %-28s no centre of %d people within %g km: no size" % (name, CITY_MIN_PEOPLE, CITY_NEAR_KM))
            out.append(row[:4] + [""] * 5)
            continue
        c = centres[match]
        if km > 0.0:
            far += 1
            print("  %-28s outside any centre big enough: %.1f km from %s" % (name, km, c[1]))
        out.append(row[:4] + [str(int(c[0])), c[1] or "", "%.1f" % float(c[2]), "%d" % round(float(c[3])), "%.1f" % km])

    with open(args.list + ".tmp", "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(CITY_COLUMNS)
        w.writerows(out)
    os.replace(args.list + ".tmp", args.list)
    sized = [r for r in out if r[4]]
    shared = len(sized) - len({r[4] for r in sized})
    print("%d cities matched (%d by the nearest centre), %d not; %d share a centre with another row"
          % (len(sized), far, unmatched, shared))
    for r in sorted(sized, key=lambda r: -float(r[6]))[:5] + sorted(sized, key=lambda r: float(r[6]))[:5]:
        print("  %-28s %-24s %9s km2 %11s people" % (r[1], r[5], r[6], r[7]))


# ----------------------------------------------------------------------------- water
# The major rivers and the lakes, from HydroSHEDS: HydroRIVERS v1.0 (river reaches as lines, each
# with its long-term mean discharge) and HydroLAKES v1.0 (lake shorelines as polygons), shapefiles in
# WGS 84 latitude and longitude. They stay lines in the project, not a raster: at 60" a cell is
# 1.85 km and most rivers are tens of metres wide, so the engine measures the distance to the line.
# The project keeps them as GeoPackages in water/, one per 30-degree tile by the feature's first
# point (a convention only: stage reads every feature of every file, so an edit may add a river to
# whichever file is open). A river's lines are smoothed (Chaikin's corner cutting, its ends kept, so
# the reaches still meet) and both are simplified (Douglas-Peucker) to WATER_TOLERANCE_M.

WATER = "water"
WATER_MIN_DISCHARGE = 5.0      # m3/s: the rivers kept (user, 2026-10-28), about 16 m wide
WATER_MIN_LAKE = 1.0           # km2: the lakes kept
WATER_WIDTH_K = 7.0            # a river's width (m) is about this times the root of its discharge
WATER_SMOOTH = 2               # rounds of corner cutting on a river (its points step 15" cells)
WATER_TOLERANCE_M = 100.0      # the simplification's tolerance (the rivers are good to ~500 m, the shores to 50-125 m)
WATER_MAGIC = b"NCWATER1"
EARTH_R_M = 6371008.8
GPKG_TIME = "2026-10-28T00:00:00.000Z"   # fixed, so the same import writes the same bytes
RIVER_COLUMNS = ["hyriv_id", "next_down", "discharge", "width"]
LAKE_COLUMNS = ["hylak_id", "name", "area_km2", "lake_type", "elevation"]


def checked_source(path):
    """Hashes a source file and checks it against its .sha256 sidecar, if there is one."""
    if not os.path.isfile(path):
        fail("no such source " + path)
    digest = sha256_file(path)
    sidecar = path + ".sha256"
    if os.path.isfile(sidecar):
        with open(sidecar, "r", encoding="utf-8") as f:
            expected = f.read().split()[0].lower()
        if expected != digest:
            fail("%s: SHA-256 %s does not match the sidecar's %s" % (path, digest, expected))
    return digest


def dbf_columns(path, names):
    """Columns of a dBASE table, each a numpy array of its fields' raw bytes."""
    mm = np.memmap(path, dtype="u1", mode="r")
    count = int(mm[4:8].view("<u4")[0])
    header, record = int(mm[8:10].view("<u2")[0]), int(mm[10:12].view("<u2")[0])
    fields, off, p = {}, 1, 32
    while mm[p] != 0x0D:
        fields[bytes(mm[p:p + 11]).split(b"\0")[0].decode("ascii")] = (off, int(mm[p + 16]))
        off += int(mm[p + 16])
        p += 32
    if off != record:
        fail("%s: the fields don't fill a record" % path)
    recs = np.ndarray((count, record), dtype="u1", buffer=mm, offset=header)
    out = {}
    for name in names:
        if name not in fields:
            fail("%s has no column %s" % (path, name))
        o, n = fields[name]
        out[name] = np.ascontiguousarray(recs[:, o:o + n]).view("S%d" % n).ravel()
    return out


def dbf_text(b):
    b = b.strip()
    try:
        return b.decode("utf-8")
    except UnicodeDecodeError:
        return b.decode("latin-1")


class Shapefile:
    """The parts of a polyline or polygon shapefile's records, read through its index (.shx)."""

    def __init__(self, path):
        self.shp = np.memmap(path, dtype="u1", mode="r")
        index = np.fromfile(os.path.splitext(path)[0] + ".shx", dtype=">i4", offset=100).reshape(-1, 2)
        self.offsets = index[:, 0].astype(np.int64) * 2

    def __len__(self):
        return len(self.offsets)

    def parts(self, k):
        o = int(self.offsets[k]) + 8
        kind = struct.unpack_from("<i", self.shp, o)[0]
        if kind == 0:
            return []
        if kind not in (3, 5):
            fail("a shape is neither a polyline nor a polygon (type %d)" % kind)
        nparts, npts = struct.unpack_from("<ii", self.shp, o + 36)
        starts = list(np.frombuffer(self.shp, "<i4", nparts, o + 44)) + [npts]
        pts = np.frombuffer(self.shp, "<f8", 2 * npts, o + 44 + 4 * nparts).reshape(npts, 2)
        return [pts[starts[i]:starts[i + 1]] for i in range(nparts)]


def unwrapped(ll):
    """Longitudes and latitudes (n, 2) with the longitudes made continuous across the antimeridian."""
    out = np.array(ll, dtype=np.float64)
    out[:, 0] = np.degrees(np.unwrap(np.radians(out[:, 0])))
    return out


def plane_m(ll):
    """Unwrapped longitudes and latitudes to metres east and north on a plane about the middle."""
    lat = math.radians(0.5 * (ll[:, 1].min() + ll[:, 1].max()))
    k = EARTH_R_M * math.pi / 180.0
    return np.stack([(ll[:, 0] - ll[0, 0]) * k * math.cos(lat), (ll[:, 1] - ll[0, 1]) * k], axis=1)


def simplified(xy, tol):
    """Douglas-Peucker: the indices of the points kept (the ends always)."""
    n = len(xy)
    keep = np.zeros(n, dtype=bool)
    keep[0] = keep[-1] = True
    stack = [(0, n - 1)]
    while stack:
        i, j = stack.pop()
        if j <= i + 1:
            continue
        d = xy[j] - xy[i]
        p = xy[i + 1:j] - xy[i]
        dd = float(d @ d)
        e = p - np.clip(p @ d / dd, 0.0, 1.0)[:, None] * d if dd > 0.0 else p
        e2 = np.einsum("ij,ij->i", e, e)
        k = int(np.argmax(e2))
        if e2[k] > tol * tol:
            k += i + 1
            keep[k] = True
            stack += [(i, k), (k, j)]
    return np.nonzero(keep)[0]


def smoothed(ll, rounds):
    """Chaikin's corner cutting, the two ends kept."""
    for _ in range(rounds):
        if len(ll) < 3:
            break
        a, b = ll[:-1], ll[1:]
        cut = np.empty((2 * len(a), 2))
        cut[0::2], cut[1::2] = 0.75 * a + 0.25 * b, 0.25 * a + 0.75 * b
        ll = np.concatenate([ll[:1], cut[1:-1], ll[-1:]])
    return ll


def wrapped(ll):
    out = ll.copy()
    out[:, 0] = (out[:, 0] + 180.0) % 360.0 - 180.0
    return out


def ring_area(xy):
    return 0.5 * float(np.sum(xy[:-1, 0] * xy[1:, 1] - xy[1:, 0] * xy[:-1, 1]))


def tile_of(lon, lat):
    ti = min(TILES_X - 1, max(0, int((lon - LON0) // TILE_DEG)))
    tj = min(TILES_Y - 1, max(0, int((LAT0 - lat) // TILE_DEG)))
    return ti, tj


def wkb_points(ll):
    return struct.pack("<I", len(ll)) + np.ascontiguousarray(ll, dtype="<f8").tobytes()


def gpkg_blob(wkb, ll):
    """A GeoPackage geometry: the header (little-endian, EPSG:4326, an x/y envelope) and the WKB."""
    return (b"GP\0\x03" + struct.pack("<i4d", 4326, ll[:, 0].min(), ll[:, 0].max(), ll[:, 1].min(), ll[:, 1].max())
            + wkb)


def line_blob(ll):
    return gpkg_blob(b"\x01" + struct.pack("<I", 2) + wkb_points(ll), ll)


def multipolygon_blob(polygons):
    """polygons: a list of polygons, each a list of rings (the outer first)."""
    wkb = b"\x01" + struct.pack("<II", 6, len(polygons))
    for rings in polygons:
        wkb += b"\x01" + struct.pack("<II", 3, len(rings)) + b"".join(wkb_points(r) for r in rings)
    return gpkg_blob(wkb, np.concatenate([r for rings in polygons for r in rings]))


def write_gpkg(path, rivers, lakes):
    """A GeoPackage (1.3, written with sqlite3) of two feature tables in EPSG:4326, no spatial index."""
    tmp = path + ".tmp"
    if os.path.exists(tmp):
        os.remove(tmp)
    db = sqlite3.connect(tmp)
    db.execute("PRAGMA application_id = 1196444487")   # 'GPKG'
    db.execute("PRAGMA user_version = 10300")
    db.executescript("""
        CREATE TABLE gpkg_spatial_ref_sys (srs_name TEXT NOT NULL, srs_id INTEGER PRIMARY KEY,
          organization TEXT NOT NULL, organization_coordsys_id INTEGER NOT NULL, definition TEXT NOT NULL,
          description TEXT);
        CREATE TABLE gpkg_contents (table_name TEXT NOT NULL PRIMARY KEY, data_type TEXT NOT NULL,
          identifier TEXT UNIQUE, description TEXT DEFAULT '', last_change DATETIME NOT NULL,
          min_x DOUBLE, min_y DOUBLE, max_x DOUBLE, max_y DOUBLE, srs_id INTEGER,
          CONSTRAINT fk_gc_r_srs_id FOREIGN KEY (srs_id) REFERENCES gpkg_spatial_ref_sys(srs_id));
        CREATE TABLE gpkg_geometry_columns (table_name TEXT NOT NULL, column_name TEXT NOT NULL,
          geometry_type_name TEXT NOT NULL, srs_id INTEGER NOT NULL, z TINYINT NOT NULL, m TINYINT NOT NULL,
          CONSTRAINT pk_geom_cols PRIMARY KEY (table_name, column_name),
          CONSTRAINT uk_gc_table_name UNIQUE (table_name),
          CONSTRAINT fk_gc_tn FOREIGN KEY (table_name) REFERENCES gpkg_contents(table_name),
          CONSTRAINT fk_gc_srs FOREIGN KEY (srs_id) REFERENCES gpkg_spatial_ref_sys (srs_id));
        CREATE TABLE rivers (fid INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, geom LINESTRING,
          hyriv_id INTEGER, next_down INTEGER, discharge REAL, width REAL);
        CREATE TABLE lakes (fid INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, geom MULTIPOLYGON,
          hylak_id INTEGER, name TEXT, area_km2 REAL, lake_type INTEGER, elevation REAL);
    """)
    db.executemany("INSERT INTO gpkg_spatial_ref_sys VALUES (?, ?, ?, ?, ?, ?)", [
        ("Undefined cartesian SRS", -1, "NONE", -1, "undefined", "undefined cartesian coordinate reference system"),
        ("Undefined geographic SRS", 0, "NONE", 0, "undefined", "undefined geographic coordinate reference system"),
        ("WGS 84 geodetic", 4326, "EPSG", 4326, CRS4326.to_wkt(),
         "longitude/latitude coordinates in decimal degrees on the WGS 84 spheroid")])
    for table, rows, kind, what in (("rivers", rivers, "LINESTRING", "rivers from HydroRIVERS v1.0, width in m"),
                                    ("lakes", lakes, "MULTIPOLYGON", "lakes from HydroLAKES v1.0")):
        box = [None] * 4
        if rows:
            env = np.array([struct.unpack_from("<4d", r[0], 8) for r in rows])
            box = [float(env[:, 0].min()), float(env[:, 2].min()), float(env[:, 1].max()), float(env[:, 3].max())]
        db.execute("INSERT INTO gpkg_contents VALUES (?, 'features', ?, ?, ?, ?, ?, ?, ?, 4326)",
                   (table, table, what, GPKG_TIME, *box))
        db.execute("INSERT INTO gpkg_geometry_columns VALUES (?, 'geom', ?, 4326, 0, 0)", (table, kind))
    db.executemany("INSERT INTO rivers (geom, %s) VALUES (?, ?, ?, ?, ?)" % ", ".join(RIVER_COLUMNS), rivers)
    db.executemany("INSERT INTO lakes (geom, %s) VALUES (?, ?, ?, ?, ?, ?)" % ", ".join(LAKE_COLUMNS), lakes)
    db.commit()
    db.close()
    os.replace(tmp, path)


def cmd_water(args):
    project = Project(args.project)
    rivers_shp, lakes_shp = args.rivers, args.lakes
    sources = {}
    print("hashing sources ...", flush=True)
    for shp in (rivers_shp, lakes_shp):
        stem = os.path.splitext(shp)[0]
        sources[os.path.basename(stem)] = {ext: checked_source(stem + ext) for ext in (".shp", ".shx", ".dbf")}
    tiles = {}   # (ti, tj) -> ([river rows], [lake rows])

    def bucket(lon, lat):
        return tiles.setdefault(tile_of(lon, lat), ([], []))

    cols = dbf_columns(os.path.splitext(rivers_shp)[0] + ".dbf", ["HYRIV_ID", "NEXT_DOWN", "DIS_AV_CMS"])
    q = cols["DIS_AV_CMS"].astype(np.float64)
    keep = np.nonzero(q >= args.min_discharge)[0]
    shp = Shapefile(rivers_shp)
    if len(shp) != len(q):
        fail(rivers_shp + ": the shapes and the table differ in length")
    print("%d of %d river reaches carry at least %g m3/s" % (len(keep), len(q), args.min_discharge), flush=True)
    points_in = points_out = 0
    for n, k in enumerate(keep):
        for part in shp.parts(k):
            if len(part) < 2:
                continue
            ll = smoothed(unwrapped(part), WATER_SMOOTH)
            ll = wrapped(ll[simplified(plane_m(ll), args.tolerance)])
            points_in, points_out = points_in + len(part), points_out + len(ll)
            bucket(ll[0, 0], ll[0, 1])[0].append((line_blob(ll), int(cols["HYRIV_ID"][k]), int(cols["NEXT_DOWN"][k]),
                                                  float(q[k]), round(WATER_WIDTH_K * math.sqrt(q[k]), 1)))
        if n % 200000 == 0:
            print("  rivers %d / %d" % (n, len(keep)), flush=True)
    print("rivers: %d points in, %d out" % (points_in, points_out), flush=True)

    cols = dbf_columns(os.path.splitext(lakes_shp)[0] + ".dbf", ["Hylak_id", "Lake_name", "Lake_area", "Lake_type", "Elevation"])
    area = cols["Lake_area"].astype(np.float64)
    keep = np.nonzero(area >= args.min_lake)[0]
    shp = Shapefile(lakes_shp)
    if len(shp) != len(area):
        fail(lakes_shp + ": the shapes and the table differ in length")
    print("%d of %d lakes cover at least %g km2" % (len(keep), len(area), args.min_lake), flush=True)
    points_in = points_out = dropped = 0
    for n, k in enumerate(keep):
        outers, holes = [], []
        for part in shp.parts(k):
            ll = unwrapped(part)
            xy = plane_m(ll)
            # a ring whose span is within the tolerance would collapse: islands and bays that small go
            if len(ll) < 4 or np.ptp(xy[:, 0]) < 2 * args.tolerance and np.ptp(xy[:, 1]) < 2 * args.tolerance:
                dropped += 1
                continue
            idx = simplified(xy, args.tolerance)
            if len(idx) < 4:
                dropped += 1
                continue
            r = ll[idx]
            points_in, points_out = points_in + len(part), points_out + len(r)
            # shapefiles wind outer rings clockwise, holes anticlockwise; WKB the other way round
            (outers if ring_area(xy[idx]) < 0 else holes).append(wrapped(r[::-1]))
        if not outers:
            dropped += 1
            continue
        polygons = [[o] for o in outers]
        for h in holes:   # each hole to the outer ring whose box holds its first point
            for p in polygons:
                lo, hi = p[0].min(axis=0), p[0].max(axis=0)
                if np.all(h[0] >= lo) and np.all(h[0] <= hi):
                    p.append(h)
                    break
        first = polygons[0][0][0]
        bucket(first[0], first[1])[1].append((multipolygon_blob(polygons), int(cols["Hylak_id"][k]),
                                              dbf_text(cols["Lake_name"][k]), float(area[k]),
                                              int(cols["Lake_type"][k]), float(cols["Elevation"][k])))
        if n % 50000 == 0:
            print("  lakes %d / %d" % (n, len(keep)), flush=True)
    print("lakes: %d points in, %d out; %d rings dropped as too small" % (points_in, points_out, dropped), flush=True)

    folder = os.path.join(project.root, WATER)
    os.makedirs(folder, exist_ok=True)
    for name in os.listdir(folder):
        if name.endswith(".gpkg"):
            os.remove(os.path.join(folder, name))
    total = 0
    for (ti, tj), (r, l) in sorted(tiles.items()):
        path = os.path.join(folder, tile_name(ti, tj) + ".gpkg")
        write_gpkg(path, r, l)
        total += os.path.getsize(path)
    project.manifest["water"] = {
        "folder": WATER, "min_discharge_m3s": args.min_discharge, "min_lake_km2": args.min_lake,
        "width": "%g * sqrt(discharge) m" % WATER_WIDTH_K, "smooth_rounds": WATER_SMOOTH,
        "tolerance_m": args.tolerance, "sources": sources}
    project.save()
    print("wrote %d water tiles, %.1f MB" % (len(tiles), total / 1e6))


# ----------------------------------------------------------------------------- runoff
# How much water the land sheds, from HydroRIVERS: each reach's mean discharge less its tributaries'
# is what its own catchment (CATCH_SKM) adds, never less than 0 (a reach that loses water to
# evaporation adds nothing). Summed over the reaches whose box centres fall in each cell of a
# RUNOFF_CELL grid (those draining at most RUNOFF_UPLAND; capped at RUNOFF_MAX), the runoff is the cell's added discharge over its catchment, in litres a second
# per km2; a cell without reaches borrows its neighbours' mean, RUNOFF_FILL rounds out from the data,
# and is NaN beyond (the sea, Antarctica: the engine's streams need runoff, so none rise there).
# The engine's bake doesn't read it; tools/earth_streams.c does, through stage.

RUNOFF = "runoff"
RUNOFF_FILE = "runoff/runoff.tif"
RUNOFF_CELL = 0.25             # degrees
RUNOFF_FILL = 2                # rounds of borrowing from the neighbours
RUNOFF_UPLAND = 1000.0         # km2: only reaches draining at most this (a big river's gain is its rounding)
RUNOFF_MAX = 300.0             # L/s/km2: about 9.5 m of rain a year, the wettest real catchments'
RUNOFF_MAGIC = b"NCRUNOF1"


def shape_boxes(shp):
    """Each record's shape type and bounding box (xmin, ymin, xmax, ymax), read without walking
    the records one by one."""
    o = shp.offsets + 8
    kind = shp.shp[(o[:, None] + np.arange(4)).ravel()].reshape(-1, 4).copy().view("<i4").ravel()
    box = shp.shp[(o[:, None] + 4 + np.arange(32)).ravel()].reshape(-1, 32).copy().view("<f8")
    return kind, box


def cmd_runoff(args):
    project = Project(args.project)
    stem = os.path.splitext(args.rivers)[0]
    print("hashing sources ...", flush=True)
    sources = {ext: checked_source(stem + ext) for ext in (".shp", ".shx", ".dbf")}
    cols = dbf_columns(stem + ".dbf", ["HYRIV_ID", "NEXT_DOWN", "CATCH_SKM", "UPLAND_SKM", "DIS_AV_CMS"])
    rid = cols["HYRIV_ID"].astype(np.int64)
    nxt = cols["NEXT_DOWN"].astype(np.int64)
    catch = cols["CATCH_SKM"].astype(np.float64)
    q = cols["DIS_AV_CMS"].astype(np.float64)
    shp = Shapefile(args.rivers)
    if len(shp) != len(q):
        fail(args.rivers + ": the shapes and the table differ in length")
    kind, box = shape_boxes(shp)
    order = np.argsort(rid)
    down = nxt > 0
    at = np.searchsorted(rid, nxt[down], sorter=order)
    at = np.clip(at, 0, len(rid) - 1)
    found = rid[order[at]] == nxt[down]
    inflow = np.zeros(len(q))
    np.add.at(inflow, order[at[found]], q[down][found])
    added = np.maximum(0.0, q - inflow)
    ok = (kind == 3) & (catch > 0.0) & (cols["UPLAND_SKM"].astype(np.float64) <= RUNOFF_UPLAND)
    lon = 0.5 * (box[:, 0] + box[:, 2])
    lat = 0.5 * (box[:, 1] + box[:, 3])
    W, H = int(round(360.0 / RUNOFF_CELL)), int(round(180.0 / RUNOFF_CELL))
    cx = np.clip(((lon + 180.0) / RUNOFF_CELL).astype(np.int64), 0, W - 1)
    cy = np.clip(((90.0 - lat) / RUNOFF_CELL).astype(np.int64), 0, H - 1)
    cell = (cy * W + cx)[ok]
    qsum = np.bincount(cell, weights=added[ok], minlength=W * H)
    asum = np.bincount(cell, weights=catch[ok], minlength=W * H)
    r = np.full(W * H, np.nan)
    have = asum > 0.0
    r[have] = np.minimum(RUNOFF_MAX, 1000.0 * qsum[have] / asum[have])
    r = r.reshape(H, W)
    print("%d reaches, %d of them unmatched downstream; %d cells hold reaches" % (len(q), int((~found).sum()), int(have.sum())))
    for _ in range(RUNOFF_FILL):
        v = np.nan_to_num(r)
        n = (~np.isnan(r)).astype(np.float64)
        s, c = np.zeros_like(v), np.zeros_like(v)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dy or dx:
                    sv = np.roll(v, dx, axis=1)
                    sn = np.roll(n, dx, axis=1)
                    if dy:   # rolled along the longitudes, never over the poles
                        sv = np.roll(sv, dy, axis=0)
                        sn = np.roll(sn, dy, axis=0)
                        edge = 0 if dy > 0 else -1
                        sv[edge], sn[edge] = 0.0, 0.0
                    s += sv
                    c += sn
        fill = np.isnan(r) & (c > 0)
        r[fill] = s[fill] / c[fill]
    r = r.astype(np.float32)
    valid = r[~np.isnan(r)]
    print("runoff over %d cells: median %.1f, 90th percentile %.1f, max %.0f L/s/km2; %d cells (%.1f%%) under 1"
          % (valid.size, np.median(valid), np.percentile(valid, 90), valid.max(), int((valid < 1.0).sum()),
             100.0 * (valid < 1.0).mean()))
    path = os.path.join(project.root, RUNOFF_FILE)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    profile = dict(driver="GTiff", dtype="float32", count=1, width=W, height=H, crs=CRS4326,
                   transform=Affine(RUNOFF_CELL, 0.0, LON0, 0.0, -RUNOFF_CELL, LAT0), nodata=float("nan"),
                   compress="deflate", predictor=3, tiled=True, blockxsize=256, blockysize=256)
    with rasterio.open(path, "w", **profile) as ds:
        ds.write(r[None])
        ds.update_tags(unit="L/s/km2", source="HydroRIVERS v1.0 (Lehner & Grill 2013)")
    project.manifest["runoff"] = {
        "file": RUNOFF_FILE, "cell_deg": RUNOFF_CELL, "unit": "L/s/km2",
        "method": "per cell: sum over reaches of max(0, DIS_AV_CMS less the tributaries') / sum of CATCH_SKM; "
                  "reaches draining at most %g km2; at most %g; %d rounds of neighbour means into empty cells" % (RUNOFF_UPLAND, RUNOFF_MAX, RUNOFF_FILL),
        "sources": {os.path.basename(stem): sources}}
    project.save()
    print("wrote %s (%d x %d, %.1f MB)" % (path, W, H, os.path.getsize(path) / 1e6))


def stage_runoff(project, out):
    """The runoff as earth_streams reads it, little-endian: "NCRUNOF1", u32 version 1, u32 width,
    u32 height, f64 west longitude, f64 north latitude, f64 cell (degrees), then f32 L/s/km2 rows
    north to south (NaN: no data). Returns its SHA-256, or None if the project has no runoff."""
    spec = project.manifest.get("runoff")
    if not spec:
        return None
    path = os.path.join(project.root, spec["file"])
    if not os.path.isfile(path):
        return None
    with rasterio.open(path) as ds:
        r = ds.read(1).astype("<f4")
        t = ds.transform
    h = hashlib.sha256()
    with open(out, "wb") as f:
        for b in (RUNOFF_MAGIC + struct.pack("<IIIddd", 1, r.shape[1], r.shape[0], t.c, t.f, t.a), r.tobytes()):
            h.update(b)
            f.write(b)
    return h.hexdigest()


def read_water(project):
    """Every river line and lake ring of the project's water tiles, in file and row order: a list
    of (kind, id, width, size, lon/lat array); kind 1 is a river (size its discharge, m3/s), kind 2 a
    lake's ring (size its area, km2), closed."""
    folder = os.path.join(project.root, WATER)
    out = []
    if not os.path.isdir(folder):
        return out
    for name in sorted(n for n in os.listdir(folder) if n.endswith(".gpkg")):
        path = os.path.join(folder, name).replace("\\", "/")
        db = sqlite3.connect("file:" + path + "?mode=ro", uri=True)
        for geom, rid, q, w in db.execute("SELECT geom, hyriv_id, discharge, width FROM rivers ORDER BY fid"):
            for line in gpkg_rings(geom):
                if len(line) >= 2:
                    out.append((1, int(rid or 0), float(w or WATER_WIDTH_K * math.sqrt(q or 0.0)), float(q or 0.0), line))
        for geom, lid, a in db.execute("SELECT geom, hylak_id, area_km2 FROM lakes ORDER BY fid"):
            for ring in gpkg_rings(geom):
                if len(ring) >= 4:
                    if not np.array_equal(ring[0], ring[-1]):
                        ring = np.concatenate([ring, ring[:1]])
                    out.append((2, int(lid or 0), 0.0, float(a or 0.0), ring))
        db.close()
    return out


def stage_water(project, out):
    """The water as the bake reads it, little-endian: "NCWATER1", u32 version 1, u32 count, then per
    line u8 kind (1 river, 2 lake ring), u8 0, u16 0, u32 id, f32 width (m), f32 size (m3/s or km2),
    u32 points, and the points as f64 longitude, latitude. Returns its SHA-256, or None if the
    project has no water."""
    lines = read_water(project)
    if not lines:
        return None
    h = hashlib.sha256()
    with open(out, "wb") as f:
        def put(b):
            h.update(b)
            f.write(b)
        put(WATER_MAGIC + struct.pack("<II", 1, len(lines)))
        for kind, wid, width, size, ll in lines:
            put(struct.pack("<BBHIffI", kind, 0, 0, wid, width, size, len(ll)))
            put(np.ascontiguousarray(ll, dtype="<f8").tobytes())
    rivers = sum(1 for l in lines if l[0] == 1)
    print("water: %d river lines, %d lake rings, %d points" % (rivers, len(lines) - rivers, sum(len(l[4]) for l in lines)))
    return h.hexdigest()


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

    s = sub.add_parser("cities", help="add each listed city's GHSL urban centre (area, population) to the list")
    s.add_argument("ucdb", help="GHS_UCDB_GLOBE_R2024A.gpkg")
    s.add_argument("list", help="the city list CSV, rewritten in place")
    s.add_argument("--sha256", help="expected source checksum (default: the .sha256 sidecar if present)")
    s.set_defaults(fn=cmd_cities)

    s = sub.add_parser("water", help="the major rivers and the lakes from HydroSHEDS, as the project's water tiles")
    s.add_argument("rivers", help="HydroRIVERS_v10.shp (its .shx and .dbf beside it)")
    s.add_argument("lakes", help="HydroLAKES_polys_v10.shp (its .shx and .dbf beside it)")
    s.add_argument("project")
    s.add_argument("--min-discharge", type=float, default=WATER_MIN_DISCHARGE, metavar="M3S",
                   help="keep river reaches carrying at least this, default %(default)s m3/s")
    s.add_argument("--min-lake", type=float, default=WATER_MIN_LAKE, metavar="KM2",
                   help="keep lakes at least this large, default %(default)s km2")
    s.add_argument("--tolerance", type=float, default=WATER_TOLERANCE_M, metavar="M",
                   help="simplify the lines to within this, default %(default)s m")
    s.set_defaults(fn=cmd_water)

    s = sub.add_parser("runoff", help="the land's runoff from HydroRIVERS, for the derived streams")
    s.add_argument("rivers", help="HydroRIVERS_v10.shp (its .shx and .dbf beside it)")
    s.add_argument("project")
    s.set_defaults(fn=cmd_runoff)

    args = p.parse_args(argv)
    if args.cmd == "import":
        if len(args.origin) == 2:
            args.origin.append(0.0)
        if len(args.origin) != 3:
            p.error("--origin takes LAT LON [HEADING]")
    args.fn(args)


if __name__ == "__main__":
    main()
