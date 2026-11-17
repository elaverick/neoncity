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
| Where | the residential block holding (8220.6, 7473.5), home city frame (`HOME_SLICE_X/Z`, `tests/city_test_home.c`); HM1's block at (8196.7, 7477.6), cut in two by a lane at HM2b |
| Lot | 131 x 47 m (6,098 m²), a local street along its longest side |
| Zone | suburb (HM1): 1.9 km inside the built edge by the edge measure, in a ring 2.5 km deep there |
| Fringe | 0.31 |
| Affluence | 0.45 (middling); density 0.36 |
| Region | na (Los Angeles) |
| Check | `city_test --home [ARCHETYPE] [--style STYLE]`: the block is residential with a street frontage, and the five views have lines of sight |

Since HM1 the block is a suburb's; since HM2b (HM1's block cut in two by a lane) `--home` lists its 37 plots: 12
detached (9-12 m frontages, 23 m deep, eight parking in their front yards) and two terraces of ten 4.6-4.7 m units
17 m deep. Since HM2 it lists each row's house and the first one's decisions; `--home ARCHETYPE --style STYLE` forces both on every suburb
street (`city_home_force`; a style of another region is noted, and HM3 draws them). If a change to the
zoning moves the block out of the suburbs, the check fails: pick another with `city_test --home find` and update the
constants and this table.

## Where the suburbs go (HM1)

The user's answers (2026-11-16): most people live at high density; the houses are the outer ring and a few pockets.

| What | Where | Measured (`city_test`, `city_test --home ring`) |
|---|---|---|
| The ring | `ZONE_SUBURB` from the open land at the edge in to `plan_suburb_depth`: 2.0-3.2 km of the edge measure (2.6-4.6 until HM2b), a slow field round the edge. The towns are built whole, so the last blocks stand ~1 km of the measure in: 1-2.5 km of houses on the ground | houses reach 1.4 km in on average (0-3 over 24 rays; 3.1 before HM2b); 42% of the land 0.5-2 km in |
| Estates before towers | no core within `PLAN_RING_ESTATES` (1 km of the measure) inside the ring (HM2b), so the skyline still steps down through the estates | tallest per 600 m in from the edge: 61, 135, 140, 174, 241 m (56, 125, 131, 147, 183 before HM2b) |
| Open land | the outermost `plan_open_depth` (1.0-1.6 km of the measure): two thirds fields, the rest parks and houses thinning out | |
| Through the ring | industrial estates and parks in patches (slow fields), green buffers on the boulevards; the ring's inner 400 m mixes houses and estates | ~10% each |
| Centres in the ring | town centres keep their high street (170 m), ringed by estates to 350 m, not a core; a third of the neighbourhood centres keep a parade | |
| Inner pockets | three neighbourhoods in ten hold a disc of houses 110-170 m in radius, set off the site, if the density there is 0.34 or less | 51 pockets within 5 km of LA's centre, the largest 20 of HM2b's thin blocks (0.10 km2), 3.1% of the inner residential land |
| The estates | everything else residential inside the ring, as before (`fill_estate`) | |

The blocks (HM2b): the plan cuts a suburb's leaf across its shorter side into blocks about two plots deep
(`split_suburb`, `SUBURB_LOT` by region: 32 m of lot in East Asia to 50 m in Britain), split by lanes narrower than the
towns' local streets (a 7-8 m carriageway, 2-2.5 m pavements: `PLAN_LANE_*`), so the houses meet back to back rather
than round a wide backland. The plots (`src/city/plots.c`, `city_block_plots`): each street side of a suburb block is
a strip one plot deep (back to back with the far side's where the block is shallow), laid with one archetype by the
region's weights:

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
stands in the skyline check until HM3 builds the houses; until then their plots are open ground.

## Plots (§13.5, loosened for a megacity: HM2b)

The user's verdict on HM2's plots (2026-11-16): LA's 450-800 m² lots with 10-25 m back gardens read as very affluent
for a megacity, where the rich live in the towers' condos. Since HM2b the plots are **somewhere between Tokyo-tight and
British terraced, Europe on the larger side**; the yards are small and mostly paved, with **no lawns anywhere in a
city**. §13.5's order is kept; its garden and clearance figures are the region's (`home_yard`, `HomeYard` in
`src/gen/home.h`), and the rules check each row against its own region's. Towns outside the megacities might take
larger gardens; there are none yet (TODO "Residential buildings").

| Rule | §13.5 | Now (the region's) | Notes |
|---|---|---|---|
| Order from the street | boundary, front garden, house, rear garden | kept | doors face the street |
| Front yard | 3 m at least | 1.0 m (seu, eas, sas, lat) or 1.5 m (na, uk, sea, mena, ssa) at least; planned 1-4 m, or a car pad of 5.2-6.0 m | bays and porches stand in it, 0.4-0.5 m short of the boundary (a canopy's 0.6 m always fits) |
| Rear yard | 8 m at least | eas, sas 2 m; sea, lat 2.5; mena, ssa 3; na 4; seu 4.5; uk 6 | Tokyo's two metres to a British terraced street's yards |
| Detached sides | 2.0 m each side, 4 m between two houses | eas 0.5 m; sea, sas, lat 0.75; the rest 1.0 (twice that between two houses) | Japan's civil code half metre |
| Semi open side | 1.2 m | as the detached sides | **na has no semis** (the region's archetypes) |
| Garage wing | 1 m from the boundary, set back, lean-to or flat | the side clearance, a metre at most | rare: a row that parks in its front yard has none |
| Terrace unit | 4.2 m wide at least (4.2-6 m), a row at most 48 m | kept; 4.5-6.5 m in seu, sea, mena, ssa | |
| Frontage | detached 10-18 m, semi halves 7-10 m | detached 7-10 m (eas, sas, lat), 8-11 (sea), 9-12.5 (na), 9-13 (uk, seu, mena, ssa); semi halves 5.5-8.5 | |
| Plot depth | 25-45 m (§13's 60 m cell) | 13-18 m (eas) to 19-28 m (uk) by archetype | from the street frame's frontage |

| Region | Pads (detached / semi / terrace: a row's chance of parking in its front yard) | Yards: paved / gravel / planted / soil |
|---|---|---|
| na | 0.75 / 0.5 / 0 (driveway aprons) | 45 / 20 / 35 / 0 |
| uk | 0.55 / 0.45 / 0.1 (paved-over front gardens) | 50 / 15 / 35 / 0 |
| seu | 0.5 / 0.4 / 0.1 | 55 / 15 / 30 / 0 |
| eas | 0.5 / 0.3 / 0.15 | 45 / 25 / 30 / 0 |
| sea | 0.6 / 0.6 / 0.55 (link houses' car porches) | 65 / 5 / 25 / 5 |
| sas | 0.3 / 0.2 / 0.1 | 55 / 5 / 15 / 25 |
| mena | 0.5 / 0.4 / 0.1 | 60 / 15 / 15 / 10 |
| ssa | 0.45 / 0.3 / 0.05 | 40 / 10 / 10 / 40 (swept earth) |
| lat | 0.4 / 0.3 / 0.3 (cocheras) | 60 / 5 / 15 / 20 |

A yard's surface (`HSite.surface`, `HY_*`) is drawn per unit, front and back (the back as the front seven times in
ten); soil is likelier where poor, planting (beds and pots on paving) where better off. A car pad is a drive beside
the path, clear of porches and bays, with a car on it seven times in ten and a gap in the front boundary. HM7 draws
the surfaces.

Measured (2026-11-17, LA): **41 houses a hectare of the suburb blocks' lots, plots 200 m² on average** (HM1-HM2: ~13
a hectare, 600-800 m²); the slice block 61 a hectare (134 m²). Seven rows in ten park in front (LA is na: aprons);
yards 48% paved, 21% gravel, 32% planted. The ring is half the land it was and the houses ~3 times as dense, so it
holds about 1.5 times the houses: still a few per cent of the city's people.

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
| Detached: 2 m each side (the region's since HM2b) | every volume and stack but a garage wing | `side clearance` |
| Semi: 1.2 m on the open side (the region's) | the same | `open side` |
| Front garden 3 m; rear garden 8 m (the region's yards) | the main's front; each unit's rearmost volume | `front garden`, `rear garden` |
| Garage wing 1 m from the boundary (the side clearance, a metre at most), set back, lean-to or flat | | `garage`, `garage clearance` |
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
- **Bays and porches keep 0.4-0.5 m from the street boundary** (`HomeYard.proj`; a metre until HM2b): §13.5 gives the
  garden but no figure for what stands in it; the margin leaves the boundary, its gate and the path's start. A porch
  the front yard has no room for is a canopy over the door from the start (not a repair), and a canopy always fits.
- **The gap between two detached houses (4 m, the region's twice-the-side since HM2b) is between the houses.** A
  garage wing keeps its own clearance to the boundary (§13.5's access rule). Checked across rows by `city_test`.
- **Since HM2b the main is no deeper than its plot leaves** between the front yard and the least rear yard, and an
  extension or outrigger no deeper than the rear yard spares (none under 2 m), so tight plots are planned, not
  repaired.
- **A ranch's roof pitches from 18°** (the target's departure above); every other style keeps §13.5's 25°.

Measured at HM2 (2026-11-16, HM1's large plots): `home_lab` builds 10,000 rows an archetype over every style: all accepted at level 0 (no
rejects), none empty, invalid or different twice; levels 1, 2 and the simplest built on their own on every fourth
seed, all valid; each of 12 rules broken on purpose is caught by its tag every time. 0.012 ms a detached house,
0.012 a semi half, 0.018 a terrace unit (40 houses a chunk: under 1 ms of the 30 ms budget). Repairs a house:
detached 0.51, semi 0.46, terrace 0.57, mostly lean-tos under one-storey eaves and rear gardens won back.
`city_test` builds 8,060 houses on the home city's suburb plots (the ring and the pockets): all valid, none
overlapping across rows, no two detached houses under 4 m apart.

Measured at HM2b (2026-11-17, the regions' tight plots): `home_lab` still accepts every row at level 0 with no
rejects, none empty, invalid or different twice, every fallback valid, every rule broken on purpose caught. 0.008 ms
a detached house, 0.007 a semi half, 0.011 a terrace unit. Repairs a house fell (detached 0.31, semi 0.23, terrace
0.29): mains, extensions and porches are sized to the plot before they are proposed. `city_test` builds 12,904 houses
on the home city's suburb plots (the ring's along 8 rays and the pockets'), all valid, none overlapping across rows,
no two detached houses under the region's gap.

```
build/home_lab 10000 out    # stats, the rules broken on purpose, out/home_detached.png, home_semi.png, home_terrace.png
build/home_lab 30 "" 12     # seed 12's decision log for each archetype
```

The plan sheets draw a row of each style top-down, the street along the bottom: plots in green, roofs by kind with
their ridges and hips (a lean-to's high side, a parapet's line), bays blue, porches yellow, party walls black, stacks
dark red, doors red, paths and drives, walls, hedges and fences, sheds, cars and the trees' places as rings.

## The shells (HM3a)

HM3 was split before it started (the roadmap's rule for a milestone past its size): **HM3a** the shells, **HM3b**
the materials (ambientCG sets per covering, "Materials" below) and the proxies' roof prisms ("Budgets"). `src/gen/home_shell.c` builds a graph's shell, `src/city/homes.c` sets it down, and
`shaders/city/house.glsl` shades it (`src/core/house_surf.h`: what a vertex carries).

| Part | How |
|---|---|
| Solids | each volume's **body** (its walls up to its roof's planes, from the ground; an open porch's from its eaves: a pediment and a ceiling), a **slab** for each roof plane (0.2 m deep at the eaves: covering, soffit, fascia, the verges' boards), **parapets** round a flat roof's deck and along a Georgian front, **party walls** through a row's roof, **stacks** and their caps, porch floors, posts, stoops and steps; all convex, as half-spaces |
| Joining | every face clipped against every solid it meets (CSG): a gabled outrigger's or porch's roof runs on into the main's to its ridge and meets it in valleys; a lean-to stops 0.15 m inside its parent's wall; a stack comes up through the tiles; faces looking into another solid go, and of two coplanar faces looking the same way the earlier solid's stays (a face-less plane never wins) |
| Rows | a pair's or row's main is cut into a slice a unit, each from the whole main's roof (a hipped row's hips at its ends); units stand on their own ground (the highest under what they cover), neighbours within 0.25 m at one height, so a row steps where the ground does; party walls rise 0.25 m through the roof where it steps or the style shows them (Victorian, Georgian, rowhouse, townhouse, shophouse, China), as dividing parapets on flat rows |
| Openings | carved out of a body: windows (glass set back 0.10 m, 0.14 m on Victorian, Georgian and rowhouse fronts; reveals the wall's finish), front doors (0.12 m back, panelled by the unit), garage doors, recessed porches (tiled floor, a ceiling); an opening's faces stay inside its host |
| Windows | each wall facing out, each storey: a window in each bay of about 2.7 m (section 13.7), the style's size (a Georgian sash 0.95 x 1.6 m, an inter-war casement 1.5 x 1.2, a ranch's picture window 1.5 x 1.1), sill 0.75-0.85 m over the floor, head at most 2.15 m and 0.25 m under the eaves; none where a porch, bay, wing, stack or post stands within 0.35 m of it or a door is; side walls (square to the street) wider bays and four in ten left blank |
| Finishes | `home_finish`: the row's walls and roof by its style (brick, render, siding, stone, block; clay, slate, concrete tile, shingle, metal, barrel, glazed), owners repainting render and siding (three in ten), a brick front painted over (one in twelve), the trim their own (one in five), the door always; stacks brick in Britain and North America |
| Near / far | the near shell (a layer of its own within 220 m, `HOME_NEAR_DISTANCE`), the far shell in the base layer (bodies with their roof planes, stacks; windows painted in the walls' rhythm), dropped while the near layer is drawn; the far walls cast no ray-traced shadows (they stand over the near shell's set-back windows), the far roofs and stacks do |
| Shading | each finish's palette colour under an authored grain (brick, plaster, stone, concrete; roof tiles; gravel and concrete decks; the kit's paint on trim); slate, shingles, siding's boards and corrugated sheet drawn here until HM3b; past 170 m the palette colour and the lights alone; glass reflects the sky, and by the homes' hours (`cityHours`) a share is lit behind a blind (HM6 gives them rooms) |
| Collision | bodies, enclosed porches, porches' posts; steps and stoops not yet (TODO) |

Measured (2026-11-17): `home_lab` builds both shells of 2,600 rows an archetype: **near 479 triangles a detached
house (most 1,003), 397 a semi half, 340 a terrace unit; far 26, 25, 24 (most 96)**; 16, 12 and 8 windows a unit;
0.16, 0.13, 0.12 ms a unit for both levels (60 houses a chunk: about 8 ms); none with a bad face, none different
twice, no coplanar faces sharing over 20 cm2 (a few soffit corners do, under that). In the game, the slice's
views, two runs each with `--homes off`, `far` and `near` (noon):

| View | No houses | Far shells | Near too | Pool (off / near) |
|---|---|---|---|---|
| From the air (150 m out, 90 m up) | 6.6 ms | 7.1-7.5 ms | 7.5 ms (**+0.9**, budget +1.0) | 22% / 29% |
| Down the street, 30 m | 5.6 ms | 2.9 ms | 3.2 ms (**-2.4**: the houses hide the city behind) | 21% / 29% |

With a flat shader the far shells cost nothing and the near layer 0.3 ms: the rest is shading (lighting about
half), which is why the far path drops the grain past 170 m (the air view was +1.5 ms before it).

**HM3b** (2026-11-17, noon, `--homes off` against `near`, four runs from the air, two elsewhere): the authored sets
cost no more than the drawn courses did. From the air 6.6 ms to 7.33 ms (**+0.75**, budget +1.0; one run of five
read 8.27); down the street at 30 m 6.13 to 3.61 (-2.5); along the ring 82 m up, the houses filling the view to
the horizon, 8.6-9.0 to 9.4-10.1 (about +0.9). The proxies' prisms (`city_home_prisms`, one a row's main) add
0.02 ms to the proxies' pass (17,550 drawn along the ring) and 108,000 records to the pool: the 20 km round Los
Angeles' edge holds 1.75M boxes and prisms in 16,165 pages, so the pool grew to 20,480 (80 MB; it had turned half
the range away at 6,144 even before the houses). Filling the range takes 12.5 s instead of 10.8 (the rows' graphs
are built for their prisms). The prisms match the far shells' place, height, roof form and colours (checked by
drawing them in the shells' place at the slice); they leave out wings, porches and bays.

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
| Bay window | projection 0.6-0.9 m, 1.8-2.7 m wide; one or two storeys | inside the front yard, short of the boundary (no bay where the yard leaves under 0.45 m) |
| Window bay | 2.7 m (§13.7) | near: real reveals, glass set back 0.08-0.15 m, sills, frames, glazing bars |
| Door | 0.9-1.0 m wide (§13.5's 1 m clear with its frame), 2.0-2.1 m high | |
| Dormers | 1.2-2.4 m wide, gabled, hipped or flat | |
| Boundaries | walls 0.6-1.2 m, hedges 0.8-1.8 m, fences 1.2-1.8 m, gates 0.9-1.2 m (drive gates 3 m) | na: mostly no front boundary (open, paved or gravelled fronts and driveway aprons; no lawns in a city since HM2b) |

## Materials

Fetched in HM3b (walls, roofs; the kit's in HM4), NB3's way: ambientCG sets (CC0) packed at 1024 by
`tools/pbr_pack.c` into `assets/materials/*.json`, every one a grain under the palette's colour (the shells'
colours stay the style's).

**The setting over the references** (the user, 2026-11-17: "consider our setting when selecting materials, the
setting should carry more weight than the mood board"). The mood board's houses are photographs of real, mostly
tidy suburbs; these stand in a rain-soaked megacity under a deck of cloud and its soot. So each set is the most
weathered that still reads as kept (the upkeep spread above: no derelict, few run-down): stained, sooted, mossed or
lichened, scuffed, never broken. Over the sets, painted colours are dulled a little (`houseSoot`: 18% towards
grey, 8% darker), and every wall carries the rain's runs and splashback by an amount each unit draws (a stand-in
for upkeep, which HM6 sets). The setting also moved the roofs a little towards the cheap: a quarter of Victorian
and a fifth of Georgian and tract roofs re-laid in concrete tiles, ranches partly tiles and sheet, a tenth of
craftsman roofs sheet.

| Surface | Set | Chosen for | Where |
|---|---|---|---|
| Old stock brick, stretcher bond | `house_brick` ([Bricks097](https://ambientcg.com/view?id=Bricks097)) | sooted, spalled, mortar smeared | the pre-war styles (Victorian, Georgian, rowhouse, inter-war, China, self-built: `HOUSE_COL_OLD`) |
| Wire-cut brick | `facade_brick` (Bricks085, the city's) | cleaner, later | post-war, modern, North American and the rest |
| Render and stucco | `house_render` ([Plaster007](https://ambientcg.com/view?id=Plaster007)) | rain-stained, blotched | everywhere |
| Pebbledash, roughcast, concrete block | `house_roughcast` ([Concrete035](https://ambientcg.com/view?id=Concrete035)) | mottled, dark | render colour f, every block wall |
| Painted lap siding | `house_siding` ([WoodSiding009](https://ambientcg.com/view?id=WoodSiding009)) | paint dulled, boards' edges grimed | na craftsman, tract, ranch, tripledecker; uk modern; Japan |
| Stone (ashlar, brownstone) | `facade_stone` (Tiles143, the city's) | stained limestone | rowhouse, seu, mena |
| Slate | `house_slate` ([RoofingTiles003](https://ambientcg.com/view?id=RoofingTiles003)) | streaked dark grey | uk, rowhouse, Japan |
| Concrete tiles | `house_concrete_tile` ([RoofingTiles009](https://ambientcg.com/view?id=RoofingTiles009)) | blackened, moss down the joints | uk post-war and modern, re-roofed Victorian and Georgian, na tract and ranch, sea, sas, ssa |
| Shingles | `house_shingle` ([RoofingTiles015C](https://ambientcg.com/view?id=RoofingTiles015C)) | dirty, lichen-spotted | na; ambientCG has no asphalt shingle, these square tabs read as one from the street |
| Plain clay tiles | `house_clay` ([RoofingTiles014B](https://ambientcg.com/view?id=RoofingTiles014B)) | old, dirty, lichen | uk inter-war, seu, mena, lat, sas, sea shophouses, China and Korea in part |
| Barrel and glazed tiles | `roof_tiles` (RoofingTiles012A, the city's) | the rolled form | na spanish; China, Korea and Japan's kawara |
| Corrugated sheet | `house_sheet` ([CorrugatedSteel003](https://ambientcg.com/view?id=CorrugatedSteel003)) | paint scuffed, dirt down its fall | sas, ssa, lat, Japan, Korea; some na |
| Flat roofs | `roof_gravel`, `concrete` (the city's) | | everywhere |
| Timber (porch posts, fences), painted metal (railings, grilles), glass | the kit's existing materials (HM4) | | everywhere |

Not found on ambientCG: Flemish bond (stretcher bond stands in), an asphalt shingle (square tabs stand in), a
true pebbledash (a mottled roughcast). Rust-through sheet (CorrugatedSteel007B) was looked at and left for HM6's
run-down houses, not every sheet roof.

No one set over 40% of a house's visible surface (NB3's rule): a wall set and a roof set at most share most of it.

## Lights

| Light | Kind | Notes |
|---|---|---|
| Porch and entrance lamps | warm (2700-3000 K) bulkhead or lantern by the door, one a house | in the light table with ray-traced shadows; most on at night |
| Windows | lit at night by §13.7's share (~30-45% at 19:00, ~10% at 03:00) | a cheap room behind the glass: a curtain or blind and a lit back wall, not the office impostors |
| Street | the streets' existing lamps | |
| By day | the windows dark interiors, the porch lamps off | |

## Budgets

A suburb is vast and repetitive: a 110 m chunk holds 35-60 houses since HM2b's tight plots (41 a hectare of lot, 61
on the slice's block) and tens of thousands are in view from a rise.
That decides the shape of the budgets: the character is **instanced**, the merged shells are small, and the far
view is proxies.

| What | Budget | Why |
|---|---|---|
| Near shell (walls, roofs, reveals, merged; a near layer within ~150 m) | **1,000 triangles a detached house, 700 a semi half or terrace unit** (shared walls, narrower fronts); 1,200 before HM2b | 60 houses a chunk at ~850 = 51k triangles, 6.1 MB; ~12 chunks near = ~75 MB of the 504 MB pool |
| Character kit (eaves parts, porches, bays, chimney pots, gutters, frames: instances of shared modules) | **150 instances a house** within 60 m; the module library under 20 MB over its levels, indexed | NB2's instancing; detail without the pool |
| Far shell (base layer, to the 1.9 km view distance) | **60 triangles a house** (walls, a roof prism, chimney boxes) | ~900 chunks in view: 60 x 60 x 3 x 40 B = 0.43 MB a chunk |
| A street of 40 houses | 34k merged triangles, 6,000 instances | the slice's street, HM5 |
| The pool at the worst suburban view | the residential layers under 150 MB; the whole pool under 90% | measured at HM8 |
| Proxies (beyond the view distance) | **one prism a row, pair or detached house's block, not a box a house** | the proxies' pool holds 768k boxes and is already too small (TODO "Code structure"); a ring of suburbs 1.4 km deep on average round a 100 km city holds ~0.6M houses (HM2b: half the land, three times as dense) |
| Frame cost | **+0.5 ms on the ground, +1.0 ms from the air** at the worst suburban views (the 2.5 ms budget's share) | the vendors' gate settled at ~1 ms for a street of bars; houses are the whole view, so the per-house cost must be lower |
| Generation | **30 ms a chunk of 60 houses** | no per-house occlusion bake: the kit's modules carry theirs (baked once); a house's shell gets cheap per-vertex occlusion |
| Collision | walls, porches' posts and steps, boundaries with their gates open | boxes from the plan |

## Captures and the comparison board

`city_test --home` prints the views (`view NAME --cam X Z YAW PITCH EYE_Y`): the slice's frontage from the street at
3 and 10 m (looking in at a house front 6 m back, turned off the front if a line of sight needs it), 30 and 100 m down
the street (at the pavement in front of the slice: since HM3a the houses across the street stand where those views
were), one from the air (150 m out, 90 m up) and one over the roofs (40 m out, 22 m up: HM3b). `--home ARCHETYPE --style STYLE` forces them in the game too (`--home-arch`, `--home-style`), so another region's style can be shot in Los Angeles.

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
