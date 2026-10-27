# Earth source data

The planet's large-scale shape comes from a real elevation model. This folder holds the
editable project (`earth.json`, `height/*.tif` tiles, optional `hints/`) that `tools/earth/earth.py`
writes from the source below. The `.tif` tiles are stored with Git LFS (`.gitattributes`); the
engine's bake (`earth.bake`) is a build output and is never committed.

## Source

| | |
|---|---|
| Dataset | ETOPO 2022 Global Relief Model, v1, 60 arc-second, ice-surface elevation ("surface") |
| File | `ETOPO_2022_v1_60s_N90W180_surface.tif` |
| URL | https://www.ngdc.noaa.gov/mgg/global/relief/ETOPO2022/data/60s/60s_surface_elev_gtif/ETOPO_2022_v1_60s_N90W180_surface.tif |
| Server last-modified | 2022-10-04 21:52:12 GMT |
| Size | 465,969,062 bytes |
| SHA-256 | `9d27d4b8ea8e76977e2988bca667d7c8fa68b927355feffcddd6b4875a7fd08e` |
| Format | GeoTIFF, EPSG:4326, 21600 × 10800, signed metres, one global file (north-west corner 90°N 180°W) |
| DOI | https://doi.org/10.25921/fd45-gt74 |
| Product page | https://www.ncei.noaa.gov/products/etopo-global-relief-model |
| Downloaded | 2026-10-22 |

NOAA publishes no checksum alongside the file; the SHA-256 above was computed on the download,
whose size matches the server's `Content-Length`.

Citation: NOAA National Centers for Environmental Information. 2022: ETOPO 2022 15 Arc-Second
Global Relief Model. NOAA National Centers for Environmental Information.
https://doi.org/10.25921/fd45-gt74. Accessed 2026-10-22.

## Where the source lives

The source file is not in the repository. It sits outside it, with its hash beside it:

```
C:\Users\Edward Laverick\Source\Data\ETOPO2022\ETOPO_2022_v1_60s_N90W180_surface.tif
C:\Users\Edward Laverick\Source\Data\ETOPO2022\ETOPO_2022_v1_60s_N90W180_surface.tif.sha256
```

Treat it as untrusted download data: keep it in its own folder and run the importer with `python -I`.
To fetch it again, download the URL above and check the hash: `sha256sum -c *.sha256`.

## Decisions recorded with the download (2026-10-22)

- **Storage:** the editable tiles go in this folder through Git LFS. The GitHub account shows the
  free 10 GiB allowance with the LFS budget at $0, so over quota LFS blocks rather than charges.
- **Origin:** the megacity sits at **34.0522°N, 118.2440°W** (central Los Angeles), heading 0°
  (north). Note for E4/E6: the Pacific is about 25 km south-west of this point, so the city's
  fringe approaches the coast; the sea-level margin keeps settlements and roads off the sea floor.

## The tool

`tools/earth/earth.py` (Python 3.11+, `tools/earth/requirements.txt`: numpy, rasterio) builds and
edits the project. Set it up once with a virtual environment, which `.gitignore` leaves out:

```
python -m venv tools/earth/.venv
tools/earth/.venv/Scripts/python -m pip install -r tools/earth/requirements.txt
```

Then, with `py` standing for `tools/earth/.venv/Scripts/python -I tools/earth/earth.py`:

| Command | What it does |
|---|---|
| `py import <source.tif> data/earth [--origin LAT LON [HEADING]]` | writes the 72 tiles, `earth.json` and `earth.vrt`; checks the source against its `.sha256` sidecar (30 s) |
| `py check data/earth` | validates every tile and the manifest, prints the minimum, maximum and highest slope |
| `py extract data/earth <lon0> <lat0> <lon1> <lat1> out.tif` | one GeoTIFF of a region to edit in QGIS, Photoshop or Gaea (`--layer hints` for the hint layer) |
| `py insert data/earth edited.tif [--feather N]` | writes the edit back into the tiles it covers; nodata pixels keep their values; only changed tiles are rewritten |
| `py preview data/earth out.png [--region …] [--width W] [--exaggerate X]` | a hillshade with the sea floor tinted blue |
| `py stage data/earth out.r16` | the whole grid as raw little-endian int16 plus `out.r16.txt`, the input of `tools/earth_bake.c`; with any `hints/` tile, the hints too as `out.r16.hints` (uint8 × 3 a cell, 700 MB), named in the sidecar |

The grid is fixed at EPSG:4326, 60", pixel-is-area, 21600 × 10800 from 90°N 180°W. A tile is named
by its south-west corner: `height/N30_W120.tif` spans 30°N..60°N, 120°W..90°W. Elevations are whole
metres (EGM2008), the sea floor negative, no nodata. An edited region must come back on the same grid
(the tool refuses anything else), and an unedited `extract` → `insert` leaves the tiles byte-identical.
`earth.vrt` opens the tiles as one layer in QGIS. The optional `hints/` tiles are 8-bit, three bands:
R landform override (0 none, 1 plains, 2 hills, 3 mountains, 4 plateau), G detail roughness scale,
B no-settlement mask; `extract --layer hints` gives zeros where no hint tile exists yet. Edit a region
with `extract --layer hints` → your editor → `insert --layer hints`; the engine reads them from the bake:
the override wins over the landform the relief suggests, G scales the procedural landforms' height
(128 is 1, 0 reads as 128), and B is kept for the settlements (E6).

Imported 2026-10-22: minimum −10752 m (23.2°S 174.7°W), maximum 8157 m (28.0°N 86.9°E), 65.9% of
cells below sea level, and the origin cell at 99 m.

## The bake

The engine reads the project as `earth.bake`, written by `tools/earth_bake.c` (built with the game as
`build/earth_bake`) from the staged grid. It is a build output, never committed:

```
py stage data/earth build/earth.r16
build/earth_bake build/earth.r16 build/earth.bake [--size N] [--threads T]
```

or, in one step when the venv (or a `python3`) is found at configure time, `cmake --build build --target earth`.
The bake holds the same ground resampled onto the six equal-angle cube faces the engine addresses the
planet by (`src/terrain/terrain_cube.c`, which the tool links and nothing else of the engine, so bake and
engine agree on every direction bit for bit): N = 4096 samples a side at mip 0 (2.44 km at a face's
centre), halving per mip down to 8, each face with a 2-sample apron resampled past its edges at the
apron's own directions. Each mip carries per-8×8-block minimum, maximum, steepest grade and the relief
within about 20 km. Every sample is the area-weighted mean of the source over its cell (the cell is split
into sub-cells no larger than a source cell; each sub-cell's latitude-longitude box is averaged); mips are
2×2 means. The file layout is set out at the top of `tools/earth_bake.c`. Baking the same stage at the
same size gives the same bytes whatever the thread count; the full bake is 319 MB and takes about 12 s
on 32 threads. `--info <bake>` prints the header, the per-mip table and a seam check (each apron sample
against the face it lies over); `--dump-face <k|all> <bake> out.png [--mip M]` draws a face, or the six
as a 3 × 2 net, shaded, the sea floor blue. `tests/data/earth_test.bake` is the real project at
`--size 256` (2.8 MB), for ctest and the engine's tests.

After the mips the bake (format version 2) carries a landform grid on the first mip at least 8 km a
sample (mip 2, 1024 a side, at N = 4096; mip 0 in the test bake): per sample, the relief (the range of
the elevation within 20 km), the ridges' strike (the dominant gradients' direction turned a quarter
turn, from the structure tensor of the stage averaged over 15′ cells, within 100 km, as a doubled angle
from east whose length is its coherence) and, when the stage has hints, the override, roughness and
no-settlement mask under it. The engine's landforms follow it (`src/terrain/terrain_land.c`): much
relief makes mountains, some hills, high level ground plateaus, low level ground plains; the mountains'
ridges run along the strike; and where the data already carries the relief the procedural ridges are
scaled down. It adds about 2 s and 25 MB to the full bake. `terrain_test --landform-map out.ppm [px]`
draws the landforms the bake gives the whole planet.

## The city list

`largest_metropolitan_areas_227.csv` (UTF-8; added 2026-10-27) lists the 227 metropolitan areas the
settlements stand at: `rank,name,latitude,longitude`, a header first, a name in double quotes where it
holds a comma. It has no population; the size comes from the rank. Two rows are spelling duplicates
at the same place (Ho Chi Minh City, Urumqi), which the merging absorbs. The engine reads it at run
time (`src/plan/plan_world.c`, `PLAN_CITY_LIST`) from beside the executable, where every build copies
it (the `city_list` target), or failing that from the source tree. Cities within 30 km of each other become one,
ranks 1-50 are colossal, and the row at the bake's origin (Los Angeles) is the megacity.
