# Residential buildings: target sheet

The target for the houses of the residential roadmap (HM0-HM8; the blocks follow in HM9-HM10), from
`docs/procedural-building-generation.md` §13 ("§13" below). Every later step is judged against this sheet, the
signed-off references and the office interior impostors (the quality bar), not against §13's browser-prototype
cell. Where the code departs from §13, the departure and its reason are written here.

**References**: `docs/reference/residential/` (54 photographs from Wikimedia Commons, credits in `CREDITS.md`),
the mood board the user signed off on 2026-11-16. Mood, proportion and construction are taken from them; no single
real house or street is reproduced.

## Regions

Each city's houses take the palette of the region its centre lies in. Styles change the treatment (walls, roofs,
openings, the character layer) and the archetype weights; the grammar's rules are the same everywhere. North
America and Britain are built to final quality first (HM3-HM5); the other regions follow as kit and grammar breadth
(HM5b, sized at HM5's sign-off). `city_test --home regions` lists every city's region.

| Code | Region | Where (the city's centre) | Cities | Styles | Archetypes |
|---|---|---|---|---|---|
| na | North America | west of 30° W, north of 24° N | 17 (Los Angeles, home) | craftsman, spanish, ranch, tract; rowhouse and tripledecker in the north-east | detached most, rowhouse terraces in the north-east, no semis |
| uk | Britain and NW Europe | 45° N and north, 30° W to 60° E; Australia and New Zealand | 42 | victorian, georgian, interwar, postwar, modern | §13.9's weights: terraces, semis, detached |
| seu | Southern Europe | 36° to 45° N | 15 | villa, townhouse | terraced townhouses, detached villas |
| eas | East Asia | east of 92° E, 22° N and north | 33 | japan, china, korea | detached (japan), low-rise rows and courtyards (china), walk-up villas (korea) |
| sea | South-East Asia | east of 92° E, south of 22° N | 14 | shophouse, linkhouse | terraces most |
| sas | South Asia | 60° to 92° E, 5° to 37° N | 26 | rcc, bungalow | attached self-built rows, detached bungalows |
| mena | Middle East and North Africa | 12° to 36° N, west of 60° E | 8 | villa, courtyard | walled detached, courtyard rows |
| ssa | Sub-Saharan Africa | south of 12° N, west of 60° E | 14 | block, walled | walled detached |
| lat | Latin America | west of 30° W, south of 24° N | 8 | selfbuilt, colonial | attached self-built rows, single-storey colonial rows |

Gaps on the board (no usable photographs): Korea, Sub-Saharan Africa; the Middle East has one. They are filled
before HM5b builds them.

## Upkeep

The user's verdict on the board: **houses don't have to be run down, and none are derelict.** Upkeep is a spread
over a street, pulled by `plan_affluence`:

| Upkeep | Share (middling affluence) | What it shows |
|---|---|---|
| Kept | ~60% (more where wealthy) | sound paint and render, clean roofs, tended gardens |
| Tired | ~30% | faded paint, a stained render, an overgrown hedge, a mismatched replacement window or door |
| Run-down | ~10% (more where poor) | dishes, security grilles, a patched roof, render patches, junk in the garden, at most a boarded window |
| Derelict | none | no abandoned or roofless houses |

Graffiti follows the city's existing rule (by affluence, on walls, shutters and boarded openings).

## The slice site

| | |
|---|---|
| Where | the residential block holding (8196.7, 7477.6), home city frame (`HOME_SLICE_X/Z`, `tests/city_test_home.c`) |
| Lot | 131 x 99 m (12,411 m²), a local street along its longest side |
| Fringe | 0.30 (the fringe's inner half, where HM1 puts the suburbs) |
| Affluence | 0.45 (middling); density 0.37 |
| Region | na (Los Angeles) |
| Check | `city_test --home [ARCHETYPE] [--style STYLE]`: the block is residential with a street frontage, and the five views have lines of sight |

Today the block holds `fill_estate`'s slabs and points; HM1-HM3 put houses on it, forced by archetype and style
through `--home` once the generator exists. If HM1's re-banding moves the block out of residential, the check fails:
pick another with `city_test --home find` and update the constants and this table.

## Plots (§13.5, kept as written)

| Rule | Value | Notes |
|---|---|---|
| Order from the street | boundary, front garden, house, rear garden | doors face the street |
| Front garden | 3 m at least | bays and porches stand inside it |
| Rear garden | 8 m at least | |
| Terrace unit | 4.2 m wide at least (4.2-6 m) | a row at most 48 m long |
| Semi pair | 7-10 m a half; 1.2 m clear on the open side | **na has no semis** (the region's archetypes) |
| Detached | 10-18 m frontage; 2.0 m clear each side, 4 m between two houses | |
| Garage wing | 1 m from the plot boundary, set back from the front wall | lean-to or flat roof |
| Plot depth | 25-45 m | from the street frame's frontage (`street_frame`, `frame_rect`), not §13's 60 m cell |

## Massing and parts

| Part | Size | Notes |
|---|---|---|
| Storey | 2.7-3.0 m floor to floor | ground floor 0.15-0.45 m above the garden (a step or a stoop; rowhouse stoops to 1.5 m) |
| Storeys | 1 (ranch, bungalows) to 3 (rowhouses, Georgian); 2 most | |
| Main volume | a rectangle, depth 7-10 m (terraces 8-11, ranch 9-12) | |
| Roof | gable, hip or lean-to spanning the short side; pitch 25-50° (§13.5's 25° floor); span at most 12 m | ranch 18-25°, a departure: real ranches are shallower than §13's floor; they are hips over short spans so they drain |
| Intersecting roofs | a wing's, outrigger's or rear extension's roof meets the main one: valleys, a lower ridge running in, a lean-to against a wall | HM3's hard geometry |
| Eaves | overhang 0.2-0.6 m (craftsman 0.6-0.9, georgian 0 behind a parapet) | soffit, fascia; rafter tails or brackets by style |
| Chimneys | stack 0.6-1.2 m wide, 0.45-0.6 m deep, 0.9-1.5 m over the ridge; a pot a flue | on a ridge, a gable end or a party wall, never mid-slope (§13.4) |
| Porch | depth 1.2-2.0 m; craftsman verandas to 2.5 m | over every front door |
| Bay window | projection 0.6-0.9 m, 1.8-2.7 m wide; one or two storeys | inside the front garden's 3 m |
| Window bay | 2.7 m (§13.7) | near: real reveals, glass set back 0.08-0.15 m, sills, frames, glazing bars |
| Door | 0.9-1.0 m wide (§13.5's 1 m clear with its frame), 2.0-2.1 m high | |
| Dormers | 1.2-2.4 m wide, gabled, hipped or flat | |
| Boundaries | walls 0.6-1.2 m, hedges 0.8-1.8 m, fences 1.2-1.8 m, gates 0.9-1.2 m (drive gates 3 m) | na: mostly no front boundary (open lawns) |

## Materials

Designed here, fetched in HM3 (walls, roofs) and HM4 (the kit), NB3's way: ambientCG sets packed by
`tools/pbr_pack.c` into `assets/materials/*.json`, "own" sets showing the photograph's colour, "grain" sets under
the material's. The candidates, to be confirmed against the sets when they are fetched:

| Surface | Kind | Regions |
|---|---|---|
| Brick, red and brown, stretcher and Flemish bond | own, 2-3 sets | uk, na rowhouses, seu, eas (china grey brick) |
| Render and stucco: smooth, roughcast, pebbledash | grain under the house's colour | everywhere |
| Painted timber siding (clapboard, shingle) | grain under paint | na craftsman and tripledecker, eas japan (as panel siding) |
| Stone: sandstone, brownstone, limestone dressing | own | uk georgian, na rowhouse, mena |
| Concrete block, bare and painted | own and grain | sas, lat, ssa |
| Roof: slate | own | uk |
| Roof: clay tile (plain, pantile, Spanish barrel) | own | na spanish, seu, uk, lat |
| Roof: concrete tile, asphalt shingle | own | uk postwar and modern, na tract and ranch |
| Roof: corrugated metal, flat concrete with a parapet | own; the vendors' corrugated iron | sas, lat, ssa, mena, eas |
| Timber (porch posts, fences), painted metal (railings, grilles), glass | the kit's existing materials | everywhere |

No one set over 40% of a house's visible surface (NB3's rule): a wall set and a roof set at most share most of it.

## Lights

| Light | Kind | Notes |
|---|---|---|
| Porch and entrance lamps | warm (2700-3000 K) bulkhead or lantern by the door, one a house | in the light table with ray-traced shadows; most on at night |
| Windows | lit at night by §13.7's share (~30-45% at 19:00, ~10% at 03:00) | a cheap room behind the glass: a curtain or blind and a lit back wall, not the office impostors |
| Street | the streets' existing lamps | |
| By day | the windows dark interiors, the porch lamps off | |

## Budgets

A suburb is vast and repetitive: a 110 m chunk holds 30-50 houses and tens of thousands are in view from a rise.
That decides the shape of the budgets: the character is **instanced**, the merged shells are small, and the far
view is proxies.

| What | Budget | Why |
|---|---|---|
| Near shell (walls, roofs, reveals, merged; a near layer within ~150 m) | **1,200 triangles a house** | 40 houses a chunk = 48k triangles, 5.8 MB; ~12 chunks near = ~70 MB of the 504 MB pool |
| Character kit (eaves parts, porches, bays, chimney pots, gutters, frames: instances of shared modules) | **150 instances a house** within 60 m; the module library under 20 MB over its levels, indexed | NB2's instancing; detail without the pool |
| Far shell (base layer, to the 1.9 km view distance) | **60 triangles a house** (walls, a roof prism, chimney boxes) | ~900 chunks in view: 40 x 60 x 3 x 40 B = 0.29 MB a chunk |
| A street of 40 houses | 48k merged triangles, 6,000 instances | the slice's street, HM5 |
| The pool at the worst suburban view | the residential layers under 150 MB; the whole pool under 90% | measured at HM8 |
| Proxies (beyond the view distance) | **one prism a row, pair or detached house's block, not a box a house** | the proxies' pool holds 768k boxes and is already too small (TODO "Code structure"); a ring of suburbs 3 km deep round a 100 km city holds ~1M houses |
| Frame cost | **+0.5 ms on the ground, +1.0 ms from the air** at the worst suburban views (the 2.5 ms budget's share) | the vendors' gate settled at ~1 ms for a street of bars; houses are the whole view, so the per-house cost must be lower |
| Generation | **30 ms a chunk of 40 houses** | no per-house occlusion bake: the kit's modules carry theirs (baked once); a house's shell gets cheap per-vertex occlusion |
| Collision | walls, porches' posts and steps, boundaries with their gates open | boxes from the plan |

## Captures and the comparison board

`city_test --home` prints the views (`view NAME --cam X Z YAW PITCH EYE_Y`): the slice's frontage from the street at
3, 10, 30 and 100 m (looking in at a house front 6 m back, turned off the front if a line of sight needs it) and one
from the air (150 m out, 90 m up).

```
python tools/home_capture.py LABEL [--only noon|dusk|night] [--home ARCHETYPE --style STYLE]
python tools/home_board.py LABEL [--region na|uk|...]
```

take every view in three lights (noon, dusk at 17:05, night in rain at 18:50; the city keeps Los Angeles' time) into
`build/home/LABEL/`, then lay one board per light: the five views along the top and the region's references below.
Each HM step captures under its own label (`hm0` is the block as `fill_estate` builds it today, the baseline).

## The judgement

At each sign-off (HM4, HM5, HM6, the HM8 gate) the user judges the board against the references and the office
impostors. What fails the slice:
- a house that reads as a box with a texture (no eaves shadow, no reveals, no roofscape), within 30 m;
- a street of identical houses (a row is built as one, but owners change doors, porches and windows);
- anything through a roof plane or another part; a chimney mid-slope; a door with no path or porch;
- a suburb that reads as a postcard: no dishes, cables, bins or wear at all;
- or derelict: run-down is the minority, never abandoned.
