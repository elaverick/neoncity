# Earth source data

The planet's large-scale shape comes from a real elevation model. This folder holds the
editable project (`earth.json`, `height/*.tif` tiles, optional `hints/`, and the rivers and lakes in
`water/*.gpkg`) that `tools/earth/earth.py` writes from the sources below. The `.tif` tiles and the
`.gpkg` water tiles are stored with Git LFS (`.gitattributes`); the
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
| `py stage data/earth out.r16` | the whole grid as raw little-endian int16 plus `out.r16.txt`, the input of `tools/earth_bake.c`; with any `hints/` tile, the hints too as `out.r16.hints` (uint8 × 3 a cell, 700 MB), and with `water/` the rivers' and lakes' lines as `out.r16.water` (240 MB), both named in the sidecar |
| `py cities <ucdb.gpkg> <list.csv> [--sha256 HEX]` | adds each listed city's GHSL urban centre (id, name, area, population) to the city list, in place ("City sizes", below) |
| `py water <HydroRIVERS_v10.shp> <HydroLAKES_polys_v10.shp> data/earth [--min-discharge 5] [--min-lake 1] [--tolerance 100]` | the major rivers and the lakes as the project's `water/` tiles ("Rivers and lakes", below; about 5 minutes) |

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
`--size 256` with the big waters only (5.9 MB), for ctest and the engine's tests:

```
build/earth_bake build/earth.r16 tests/data/earth_test.bake --size 256 --water-min-discharge 1000 --water-min-lake 500 --water-tolerance 2000
```

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

Last (format version 3) comes the water: the rivers' lines and the lakes' rings as runs of points
(directions in f32, each river point with its width), and per face 1024 × 1024 buckets (9.8 km at a
face's centre) listing every segment within 2 km (plus a river's half width) of them, with whether each
bucket's centre lies in a lake. The engine measures the exact distance to the lines near a point
(`terrain_earth_water` in `src/terrain/terrain.h`: the signed distance to the water's edge, the nearest
river's centre line, width and direction downstream, and whether the point is in a lake), about 2 µs a
query. On the full project it is 13.0 million points and 27.9 million bucket entries: it adds 351 MB to
the bake (670 MB in all) and about 6 s. The `--water-*` options thin it (the test bake's).
`terrain_test --water-map out.ppm [px] [lon0 lat0 lon1 lat1]` draws it, of the world or of a region.

## The city list

`largest_metropolitan_areas_227.csv` (UTF-8; added 2026-10-27) lists the 227 metropolitan areas the
settlements stand at, a header first, a name in double quotes where it holds a comma:
`rank,name,latitude,longitude` as the list came, then `ghsl_id,ghsl_name,area_km2,population,ghsl_km`,
the GHSL urban centre (below) each row was matched to by `earth.py cities`: its id, name, area (km²) and
population in 2025, and how far the row's point lies outside it (0 inside). Two rows are spelling
duplicates at the same place (Ho Chi Minh City, Urumqi), and several share one centre (Guangzhou,
Shenzhen, Dongguan and Foshan are one urban centre of 6,454 km²); the merging absorbs both. The engine
reads it at run
time (`src/plan/plan_world.c`, `PLAN_CITY_LIST`) from beside the executable, where every build copies
it (the `city_list` target), or failing that from the source tree. The row at the bake's origin (Los Angeles) is
the megacity. Each city is built from 60 km across (the list's smallest area, and a row without one) to 150 km
(its largest), with the log of its GHSL area; cities whose footprints would come near merge, their areas summed.

### City sizes: the GHSL Urban Centre Database

| | |
|---|---|
| Dataset | GHS Urban Centre Database 2025 (GHS-UCDB R2024A), V1.2 |
| File | `GHS_UCDB_GLOBE_R2024A_V1_2.zip`, and from it `GHS_UCDB_GLOBE_R2024A.gpkg` |
| URL | https://jeodpp.jrc.ec.europa.eu/ftp/jrc-opendata/GHSL/GHS_UCDB_GLOBE_R2024A/GHS_UCDB_GLOBE_R2024A/V1-2/GHS_UCDB_GLOBE_R2024A_V1_2.zip |
| Server last-modified | 2026-05-19 14:26:51 GMT |
| Size | zip 276,662,440 bytes; gpkg 285,081,600 bytes |
| SHA-256 | zip `12dc8d366a6057b832a070ba3f7b7b7c601bad191da0417a666851f35f10db2e`; gpkg `56e60b7cbfc776b23493d5684aa5d0d7d6940fc3dd9a3881fa52599f20fd80fc` |
| Format | GeoPackage, 11,422 urban centres, polygons in ESRI:54009 (Mollweide); the table read is `GHSL_UCDB_THEME_GENERAL_CHARACTERISTICS_GLOBE_R2024A` (`ID_UC_G0`, `GC_UCN_MAI_2025`, `GC_UCA_KM2_2025`, `GC_POP_TOT_2025`) |
| Licence | CC BY 4.0 (© European Union, 1995-2026) |
| DOI | https://doi.org/10.2905/1a338be6-7eaf-480c-9664-3a8ade88cbcd |
| Downloaded | 2026-10-28 |

The JRC publishes no checksums; those above were computed on the download. The source is not in the
repository; it sits with its hashes beside it at `C:\Users\Edward Laverick\Source\Data\GHSL\`. Treat it as
untrusted download data, as the elevation source.

Citation: Mari Rivero, I., Melchiorri, M., Florio, P., Schiavina, M., Goch, K., Politis, P., Uhl, J. H.,
Pesaresi, M., Maffenini, L., Sulis, P., Crippa, M., Guizzardi, D., Pisoni, E., Belis, C., Jacome Felix
Oom, D., Branco, A., Mwaniki, D., Kochulem, E., Githira, D., Carioli, A., Ehrlich, D., Tommasi, P.,
Kemper, T., Dijkstra, L. (2024): GHS-UCDB R2024A - GHS Urban Centre Database 2025. European Commission,
Joint Research Centre (JRC). doi:10.2905/1a338be6-7eaf-480c-9664-3a8ade88cbcd. Changes: only the
centres' areas and populations are used, matched to the city list's points.

`py cities <dir>/GHS_UCDB_GLOBE_R2024A.gpkg data/earth/largest_metropolitan_areas_227.csv` checks the
GeoPackage against its `.sha256` sidecar and rewrites the list's GHSL columns (about a second). It matches
each row to the urban centre of at least 300,000 people that holds its point, else to the nearest such
centre within 30 km. On R2024A V1.2, 223 rows lie inside a centre and 3 are matched to the nearest
(Tianjin, 13.7 km; Harbin, 3.9 km; Yantai, 5.0 km, to Fushan). Perm is unmatched: the database has no
centre for it, so its columns are empty. The largest centre is Dhaka's (6,611 km²), then Guangzhou's
(6,454) and Tokyo's (5,165); Los Angeles's is 4,498 km²; the smallest is Ufa's (123).

## Rivers and lakes

The major rivers and the lakes come from HydroSHEDS, as lines, not as a raster: at 60" a cell is 1.85 km
and most rivers are tens of metres wide, so the project keeps the rivers' lines with a width each and the
lakes' shores as polygons, and the engine measures the distance to them (decided with the user,
2026-10-28, in place of masks in the hints).

| | |
|---|---|
| Rivers | HydroRIVERS v1.0, `HydroRIVERS_v10_shp.zip`: https://data.hydrosheds.org/file/HydroRIVERS/HydroRIVERS_v10_shp.zip (544,388,154 bytes; SHA-256 `0cf9e363e4b48ede535787f9e2eecbcff43eb53acad4b9cbf048c9e12453e5b5`). 8,477,883 river reaches as lines from the 15" HydroSHEDS flow directions, each with its long-term mean natural discharge (`DIS_AV_CMS`, WaterGAP 1971-2000), drawn downstream |
| Lakes | HydroLAKES v1.0, `HydroLAKES_polys_v10_shp.zip`: https://data.hydrosheds.org/file/hydrolakes/HydroLAKES_polys_v10_shp.zip (820,295,132 bytes; SHA-256 `9ef63498569dd7ccd0b8d8852da026e009a75f42d293f00e483f3b9fcfe8e1f9`). 1,427,688 lakes and reservoirs of 10 ha and more as shoreline polygons, the Caspian among them |
| Format | ESRI shapefiles in WGS 84 latitude and longitude; `earth.py` reads them itself (no GDAL vector driver needed) |
| Licence | HydroLAKES: CC BY 4.0. HydroRIVERS: the HydroSHEDS licence agreement, free for non-commercial and commercial use with attribution |
| Product pages | https://www.hydrosheds.org/products/hydrorivers, https://www.hydrosheds.org/products/hydrolakes |
| Downloaded | 2026-10-28 |

HydroSHEDS publishes no checksums; those above were computed on the downloads. The sources are not in the
repository; the zips and the shapefiles extracted from them sit at
`C:\Users\Edward Laverick\Source\Data\HydroSHEDS\`, with `SHA256SUMS` for the zips and a `.sha256`
sidecar beside each `.shp`, `.shx` and `.dbf`, which `earth.py water` checks. Treat them as untrusted
download data, as the elevation source.

Citations: Lehner, B., Grill G. (2013): Global river hydrography and network routing: baseline data and
new approaches to study the world's large river systems. Hydrological Processes, 27(15): 2171-2186. Data
is available at www.hydrosheds.org. Messager, M.L., Lehner, B., Grill, G., Nedeva, I., Schmitt, O. (2016):
Estimating the volume and age of water stored in global lakes using a geo-statistical approach. Nature
Communications: 13603. doi: 10.1038/ncomms13603. Data is available at www.hydrosheds.org. Changes: only
the rivers carrying at least 5 m³/s and the lakes of at least 1 km², the rivers smoothed, both simplified
to 100 m.

`py water` keeps the river reaches carrying at least 5 m³/s (the user's threshold: about 16 m wide;
1,170,717 reaches, 3.9 million km) and the lakes of at least 1 km² (185,181). A river's width is
7·√(discharge) m (16 m at 5 m³/s, about 700 m for the Mississippi at Memphis, 3 km for the lower
Amazon), written as a column the editor may change. A river's line (its points on the 15" cells'
centres) is smoothed by two rounds of corner cutting, its ends kept so the reaches still meet, and both
are simplified (Douglas-Peucker) to 100 m: the rivers are good to about 500 m, the shores to 50-125 m.
Islands and bays under 200 m across go (26,790 rings). The result is 5.0 million river points and 8.0
million shore points in 53 GeoPackages (350 MB).

The water tiles are GeoPackages (`water/N30_W120.gpkg`, by the tile of each feature's first point), each
with two tables in EPSG:4326: `rivers` (LineString; `hyriv_id`, `next_down`, `discharge` m³/s, `width` m)
and `lakes` (MultiPolygon; `hylak_id`, `name`, `area_km2`, `lake_type` 1 lake, 2 reservoir, 3 regulated
lake, `elevation` m). Open them in QGIS and edit them there: add, move or delete a river or a lake, or
change a width. The tile is only a convention: `stage` reads every feature of every file, so a new river
can go in whichever file is open. The lines through the lakes are HydroRIVERS' own flow paths; they are
under water anyway.
