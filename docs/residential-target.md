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
| Zone | suburb (HM1): 1.9 km inside the built edge by the edge measure, in a ring 3.4 km deep there |
| Fringe | 0.30 |
| Affluence | 0.45 (middling); density 0.37 |
| Region | na (Los Angeles) |
| Check | `city_test --home [ARCHETYPE] [--style STYLE]`: the block is residential with a street frontage, and the five views have lines of sight |

Since HM1 the block is a suburb's: `--home` lists its 16 detached plots (14-18 m frontages, 36-42 m deep). Since HM2
it lists each row's house and the first one's decisions; `--home ARCHETYPE --style STYLE` forces both on every suburb
street (`city_home_force`; a style of another region is noted, and HM3 draws them). If a change to the
zoning moves the block out of the suburbs, the check fails: pick another with `city_test --home find` and update the
constants and this table.

## Where the suburbs go (HM1)

The user's answers (2026-11-16): most people live at high density; the houses are the outer ring and a few pockets.

| What | Where | Measured (`city_test`, `city_test --home ring`) |
|---|---|---|
| The ring | `ZONE_SUBURB` from the open land at the edge in to `plan_suburb_depth`: 2.6-4.6 km of the edge measure, a slow field round the edge. The towns are built whole, so the last blocks stand ~1 km of the measure in: 2-4 km of houses on the ground | houses reach 3.1 km in on average (0-5 over 24 rays); 54% of the land 0.5-2.5 km in |
| Open land | the outermost `plan_open_depth` (1.0-1.6 km of the measure): two thirds fields, the rest parks and houses thinning out | |
| Through the ring | industrial estates and parks in patches (slow fields), green buffers on the boulevards; the ring's inner 400 m mixes houses and estates | ~10% each |
| Centres in the ring | town centres keep their high street (170 m), ringed by estates to 350 m, not a core; a third of the neighbourhood centres keep a parade | |
| Inner pockets | three neighbourhoods in ten hold a disc of houses 110-170 m in radius, set off the site, if the density there is 0.34 or less | 54 pockets within 5 km of LA's centre, the largest 6 blocks (0.08 km2), 3.5% of the inner residential land |
| The estates | everything else residential inside the ring, as before (`fill_estate`) | |

The plots (`src/city/plots.c`, `city_block_plots`): each street side of a suburb block is a strip one plot deep (back
to back with the far side's where the block is shallow), laid with one archetype by the region's weights:

| Region | Detached / semi / terrace (a street side's chance) |
|---|---|
| na | 0.90 / 0 / 0.10; the north-east's old cities (east of 82° W, north of 37° N) 0.45 / 0 / 0.55 |
| uk | 0.31 / 0.31 / 0.38 (§13.9's suburb weights over its houses) |
| seu | 0.40 / 0 / 0.60 |
| eas, mena | 0.60 / 0 / 0.40 |
| sea | 0.25 / 0.05 / 0.70 |
| sas | 0.35 / 0 / 0.65 |
| ssa | 0.90 / 0 / 0.10 |
| lat | 0.15 / 0 / 0.85 |

Local streets are laid before busier ones, the longest first; a later strip keeps clear of the earlier ones' plots, so
a corner plot fronts one street and the side street's first plot starts behind its rear garden. What a row leaves at
the end of a strip takes a detached house if it fits. A row's units share its style, storeys and front garden; three rows
in four take the street's style and storeys (a street was mostly built at once). Since HM2 the storeys follow the
style (`home_style_storeys`: a ranch one, a triple-decker three, a Victorian terrace two or three). The plot's planned height (`city_home_height`)
stands in the skyline check until HM3 builds the houses; until then their plots are lawns.

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

## The graph and rules (HM2)

`src/gen/home.h` (`HGraph`), `src/gen/building_residential.c` (the styles, the grammar, the garden, the driver),
`src/gen/building_residential_rules.c` (the repairs and rules). A graph is one row of plots: a detached house, a semi
pair or a terrace. §13.2's relationships are fields: `shares_party_wall_with` the `HParty` list, `attached_to`
`HVol.parent`, `supported_by` the ground (`y0 = 0`; a canopy hangs on its wall), `opens_on` `HDoor.path`. A row's
choices (roof, pitch, eaves, depth, porch kind, bays, outriggers, garage, front boundary) come from its style's
weights (`HStyleDef`, 26 styles); owners vary a unit's porch and boundary one time in seven, and add rear extensions
unit by unit. Semis are mirrored about their party wall; handed terraces (Victorian, Georgian, rowhouse) pair their
doors and outriggers on alternate party walls.

| §13.5 / §13.4 / §13.8 | Checked as | Tag |
|---|---|---|
| Pitched roof: span at most 12 m, the shorter side, 25° at least | every gable and hip; the style's least pitch | `span`, `span side`, `pitch` |
| Terrace row at most 48 m; units 4.2 m wide | the row's main; each unit | `row`, `unit width` |
| Detached: 2 m each side | every volume and stack but a garage wing | `side clearance` |
| Semi: 1.2 m on the open side | the same | `open side` |
| Front garden 3 m; rear garden 8 m | the main's front; each unit's rearmost volume | `front garden`, `rear garden` |
| Garage wing 1 m from the boundary, set back, lean-to or flat | | `garage`, `garage clearance` |
| Front door 1 m wide, on the street side, a path crossing no building | the volumes' outlines (a canted bay's trapezoid), not a bounding box (§13.8) | `door width`, `door side`, `door wall`, `path`, `path crosses` |
| A porch or canopy over every door (§13.8); a gate where its path meets a boundary | | `porch`, `gate` |
| Site fit: nothing leaves the plot, nothing overlaps unless attached | every volume, stack and garden element | `site`, `overlap`, `attached` |
| A rear extension lower than its main; a lean-to rising to its parent's wall (§13.4) | and every attached part under its main's top; a lean-to or flat part under its eaves | `extension`, `lean`, `height` |
| A flat roof has a parapet (§13.4) | habitable volumes (a main's 0.7 m, an extension's 0.3 m upstand) | `parapet` |
| Chimneys on the ridge, a gable end or a party wall, never mid-slope (§13.4) | a hip's ridge runs between its hips; a gable stack within 0.8 m of the wall | `chimney mid-slope`, `chimney height` |
| One shared roof over a pair or row, its party walls through it | | `shared roof`, `party wall` |
| Boundaries cross no building; sheds and trees clear of buildings, paths and drives; cars on drives (§13.8) | | `boundary`, `garden clear`, `car` |

Repairs, each logged with its tag: §13.10's span over 12 m made flat (`simplify`, its stacks removed) and a row
too long cut (`row`); and the grammar's own: a rear garden won back by shortening, then removing, the unit's
extension or outrigger, then a shallower main, then the house nearer the street (`rear garden`); a porch or bay
shallower, a porch made a canopy, a bay removed (`projection`); a lean-to's pitch or eaves lowered under its main's
eaves, else made flat (`lean`, `height`); a gabled porch or outrigger whose ridge would clear the main's made flat
(`ridge`); no room for a garage or a bay (`garage`, `bay`); a stack moved inside its gable or removed when its roof
turned (`chimney`). Level 1 drops the garage, outriggers and extensions; level 2 is the plain form (a gable or flat
main, a canopy, a path); then the simplest valid house at the least clearances; a plot even that cannot take is left
a garden (`HGraph.empty`; never, on HM1's plots).

Departures, written here as the code has them:
- **A gabled porch faces the street** whatever its proportions: it is exempt from "spans the shorter side".
- **Bays and porches keep 1 m from the street boundary** (`PROJ_MIN`): §13.5 gives the 3 m garden but no figure for
  what stands in it; a metre leaves the boundary, its gate and the path's start.
- **The 4 m between two detached houses is between the houses.** A garage wing keeps its own metre to the boundary
  (§13.5's access rule), so beside a neighbour's 2 m clearance it stands 3 m off. Checked across rows by `city_test`.
- **A ranch's roof pitches from 18°** (the target's departure above); every other style keeps §13.5's 25°.

Measured (2026-11-16): `home_lab` builds 10,000 rows an archetype over every style: all accepted at level 0 (no
rejects), none empty, invalid or different twice; levels 1, 2 and the simplest built on their own on every fourth
seed, all valid; each of 12 rules broken on purpose is caught by its tag every time. 0.012 ms a detached house,
0.012 a semi half, 0.018 a terrace unit (40 houses a chunk: under 1 ms of the 30 ms budget). Repairs a house:
detached 0.51, semi 0.46, terrace 0.57, mostly lean-tos under one-storey eaves and rear gardens won back.
`city_test` builds 8,060 houses on the home city's suburb plots (the ring and the pockets): all valid, none
overlapping across rows, no two detached houses under 4 m apart.

```
build/home_lab 10000 out    # stats, the rules broken on purpose, out/home_detached.png, home_semi.png, home_terrace.png
build/home_lab 30 "" 12     # seed 12's decision log for each archetype
```

The plan sheets draw a row of each style top-down, the street along the bottom: plots in green, roofs by kind with
their ridges and hips (a lean-to's high side, a parapet's line), bays blue, porches yellow, party walls black, stacks
dark red, doors red, paths and drives, walls, hedges and fences, sheds, cars and the trees' places as rings.

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
