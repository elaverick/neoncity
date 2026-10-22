# Earth source data

The planet's large-scale shape comes from a real elevation model. This folder will hold the
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
