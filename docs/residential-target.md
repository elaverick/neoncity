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

## The character kit (HM4)

The shells give the massing; the kit gives a house the depth the user asked for (the roadmap's "too flat"): 70
modules of the procedural modelling kit (`src/gen/pmodel_house.c`, `PMK_H_*`), each at three levels, drawn
instanced (NB2's way) and placed by the assemblies in `src/gen/pmodel_house.h` (HM5 sets them down on the
shells). Every part is the vocabulary of "The character layer" in the roadmap:

| Group | Modules |
|---|---|
| Roof edges | rafter tail (craftsman, notched end), knee brace, eaves bracket (Victorian console with scrolls and a drop), bargeboards (scalloped, gothic cusps), finial (turned, with a pendant), half-round and ogee gutters, stop end, swan necks (0.3 and 0.6 m reach), downpipe, clip, shoe, hopper head, ridge tiles (rolls on their bedding; crested) |
| Walls | string course, plinth, quoins (two courses, long and short), soldier course, timber (half-timbering), hung tiles |
| Openings | sills (stone with a drip, timber, concrete), lintels (stone, concrete), keystone, segmental brick arch, hood mould with its stops, craftsman casing; frames: sash (two over two, horned), Georgian six over six, inter-war casement (bars in the top lights and across the leaves), craftsman grid (four over one), ranch picture, plastic replacement |
| Dressings | louvred and panelled shutters, security grille, window box with plants |
| Doors | four-panel, six-panel, craftsman (three lights over a dentil shelf), half-glazed, plastic; knob, letterbox, knocker at L0; sunburst fanlight; doorcases (pilasters, frieze, cornice; pedimented), each with its fanlight |
| Porches | turned post, craftsman column (tapered on a brick pier with a stone cap), square post, spandrel, balustrade, flat canopy (lead top) and gabled canopy on brackets |
| Bays | corner post (stretched to the storey), cornice with dentils, apron panel |
| Roofscape | gabled, hipped and flat dormers (casement, glass, cheeks in the roof's covering), chimney pots (plain, cannon, crown, cowl), corbelled stack cap with flaunching, vent pipe, rooflight |

**Frames.** A wall piece has the wall's face at z 0, out along +z; only what sits in a reveal (sills, frames,
doors) goes behind it, to the glass's plane or hidden by it. Runs are a metre along x and stretched by their
instance along their length, which leaves their sections exact (gutters, courses, sills, lintels, cornices,
ridges, bargeboards, balustrades); runs of repeated parts are laid as whole metres. Frames and doors are built at a
usual size (a sash 0.95 x 1.6, a casement 1.5 x 1.2, a door 0.9 x 2.05...) and stretched to their opening, within
about a quarter so bars stay bars. Dormers are built for a 30-degree roof and bury their backs in a steeper one.
No instance is mirrored (a negative scale turns a module inside out under the city's culling): handed parts are
placed the other way round instead (the left bargeboards climb from the eaves, the right fall from the apex).

**Materials.** The kit has its own (`PMAT_HTRIM` on in `pmodel.h`), shaded by the houses' code, not the vendors'
(`shadeHouseKit`, `shaders/city/house.glsl`): trim, a dormer's or canopy's roof and hung tiles go through
`shadeHouse` itself (the shells' trim paint, roof sets and clay); stone, brick and cement take the shells' palettes
and sets mapped in the world, so a stretched course keeps its grain; terracotta, rainwater goods (black iron, white,
grey or brown plastic), lead, black ironmongery, stained timber, white plastic, plants and the shells' glass are
painted there. The colours are the unit's, in the instance (`house_surf.h` `HOUSE_KIT_*`: trim, door, brick and old
stock, stone, rainwater goods in the flags; the roof's covering and colour in the part byte), so one module serves
every house. The kit lights as the shells do: neither takes the ray-traced lamp shadows or the sky trace yet (TODO).

**Measured** (`pmodel_lab --only house`, 2026-11-17): 44,890 triangles over the kit's three levels (the whole
library 381,476, 21.3 MB, built in 191 ms at start-up); the library's budget raised from 340,000 to 400,000. No
support loops (their L0 was three times heavier for nothing a street view shows). Each module's budget is its
measure plus a tenth. The vignettes (a style's street side dressed over stand-in walls and roofs):

| Vignette | Instances | L0 | L1 | L2 |
|---|---|---|---|---|
| Victorian terrace (two-storey canted bay, sashes, hood mould, hopper, crested ridge, stack with four pots) | 61 | 16,600 | 7,500 | 3,600 |
| Georgian (three storeys of six over six, keystones, quoins, string course, pedimented doorcase, cornice) | 77 | 16,800 | 10,300 | 3,900 |
| Craftsman bungalow (knee braces, rafter tails, tapered columns on piers, grid windows in casings) | 27 | 4,000 | 2,100 | 1,200 |
| Inter-war semi (square bay, half-timbered gable, scalloped bargeboards, hung tiles, gabled canopy) | 66 | 18,700 | 8,200 | 2,700 |
| Roofscape (three dormers, rooflight, vent, gothic bargeboards and finial, four pots, ogee gutter) | 35 | 11,000 | 6,300 | 2,400 |
| Post-war (plastic windows and door, concrete sills and lintels, flat canopy, a grille) | 31 | 7,400 | 5,300 | 1,500 |
| Southern (panelled and louvred shutters, a window box, a grille, a fanlight) | 23 | 8,500 | 3,000 | 1,200 |

All within the 150 instances a house. L0 is drawn within 8 m only, L1 to 30 m; a house's L1 dressing is
2,000-10,000 triangles.

## The character layer (HM5)

The kit set down on the houses (`src/gen/home_character.c`): a second grammar over the shells, run on the near
shell's own solids before its faces are emitted (`home_shell_kit`), its instances riding in the near layer.

**The sets.** Style and affluence choose a row's set (`home_char_set`: the street's windows' frame, sill and head
and the rest's, door and surround, gutter and its colour, hopper or swan neck, eaves, the street's gables, ridge,
the front's reliefs, shutters, boxes, grilles, porch posts, spandrels, balustrade, door canopy, dormers). A row or a
pair is built as one and shares it; each unit then draws its upkeep and its owner's changes:

| Upkeep | Share (affluence a) | What it moves |
|---|---|---|
| kept | 0.45 + 0.35a | the row's set as built |
| tired | 72% of the rest | replacements 1.4 times as likely; shutters taken down |
| run-down | the rest (about 10% at middling affluence) | replacements 1.8 times as likely; grilles on the ground floor (two in five), no window boxes |

| Owners' changes | How often |
|---|---|
| replacement windows (plastic in Britain, vinyl in North America; the ranch's picture window kept) | Victorian 15%, Georgian 6%, inter-war 25%, post-war 20%, rowhouse and triple-decker 18%, tract and ranch 15%, craftsman 10%, southern Europe 12%, elsewhere 10%; times the upkeep's factor; otherwise the back alone 12% |
| a door of their own (plastic in Britain, a steel six-panel in North America) | 18%, times the upkeep's factor |
| plastic gutters where the row's were iron, or a colour of their own | 30% (times upkeep), 12% |
| a canopy of their own (flat for gabled, gabled for flat) | 12% |
| a loft conversion's flat dormer at the back (Britain); a gabled one (craftsman, tract) | 20-35% by affluence; 10% |
| a dormer on the street's slope (Victorian gabled, inter-war hipped, a tract house's Cape Cod gabled; a craftsman row's flat ones as built) | 20%, 20%, 15% |
| window boxes on a kept house's upper street windows (Britain, North America) | 12% |
| porch posts their own (turned for plain or the other way; spandrels with turned posts, half the time) | 15% |
| rooflights on the back slope; a soil vent | modern 40%, Britain and southern Europe 20%, else 5%; Britain and North America 50% |

**What goes where.** Every window the shell cut gets its frame (several lights side by side where the opening is
over a third wider than the frame's usual size), sill and head (brick heads only on brick walls; a Georgian top
floor's plain lintels); the street's windows shutters where the wall has their width free each side, boxes on the
upper floors; ground floor grilles by the row's custom or the owner's upkeep. Bays get a frame, sill and apron in
each light, a cornice round each storey's head and posts on their corners. Doors get their leaf (an enclosed
porch's own door half glazed) and the surround the row's set gives where the wall has room (a doorcase only on a
bare front, a fanlight also at the back of a recess); a porch `HP_CANOPY` gets the kit's canopy unless a doorcase
stands there (Georgian). Open, lean-to and veranda porches get the set's posts, spandrels on turned posts, a
balustrade off the steps. Every eaves of a pitched roof gets a gutter where nothing stands in its way (stop ends
but at a party wall); a main's, wing's, outrigger's or extension's longest run a downpipe at a corner or a party
wall; brackets under a front's eaves (scaled to the overhang), rafter tails along the walls (not round a hip).
Street gables get bargeboards and a finial, half-timbering; every craftsman gable knee braces (the shell's
craftsman verges now reach as far as the eaves, to 0.75 m). Ridges and hips get their tiles (none on shingle or
sheet); stacks their corbelled cap and a pot a flue; fronts their plinth (to the floor's height), string course
at the first floor, cornice under a parapet, hung tiles between the storeys, quoins on free corners.

**The rules, as checks with logged repairs** (`repair char-<rule>:`; `home_lab`'s counts a unit, 2,600 rows an
archetype): downpipes (none where both ends of the run are blocked: 0.013-0.036), dormers (none where the slope has
no clear place: 0.005-0.007), shutters (0.02-0.03), doorcases and fanlights (a plainer surround: 0.05-0.06), quoins
(0.001-0.003), canopies (a smaller one, or none: under 0.001), a front door left with no porch, canopy or doorcase
(0.001), bargeboards (the verge meets another roof), the budget (2 detached houses in 2,600 over 150 even dressed
lean, the most 156; none of the semis or terraces). Nothing is placed through a roof plane or another solid: every
run is sampled along its line against the shell's solids.

**Trim.** The frames, bays' posts and cornices, doorcases and porch posts take the unit's trim colour, so a row
whose trim drew black or dark green read as black holes or green bays from end to end. Victorian, Georgian and
rowhouse rows' trim is now white, cream or grey (`FIN` in `home_shell.c`); owners still paint their own (one
in five).

**Coverage** (the user, before sign-off: "the modules prepared had a lot more options than you've used"). The first
pass built the whole kit but placed little of it: bargeboards, finials and half-timbering only on gables facing the
street (Victorian and inter-war mains face the street with their eaves, so almost none), a fanlight never fit a
Victorian recess (its head 0.15 m too low) and was hidden under a canopy, the surround test counted a recess as
blocked (the body still holds its space), and plastic replacements were set high for the setting (the most placed
module of all, eight a house). Now: the gables seen from the street (their own and the sides', never the back's)
take bargeboards, finials and half-timbering; a fanlight shrinks to the room under a recess's head or a porch's
ceiling (0.3 m at least) and a canopy lifts over it; replacement rates are halved; owners add window boxes and swap
porch posts. `home_lab` checks it: each North American and British style's signature pieces on at least a set share
of its units (43 pieces: a Victorian's sashes, hood moulds, bays, gothic bargeboards, finials, fanlights, hoppers,
brackets, string courses and crested ridges; a Georgian's six over six, keystones and doorcases; an inter-war's
casements, scalloped bargeboards, half-timbering, hung tiles, bays and soldier courses; a craftsman's rafter
tails, knee braces, columns, grids, casings and door...), and `home_lab N --coverage` prints every style's share of
every module. Before and after, a Victorian unit: gothic bargeboards 10% to 44%, finials 10% to 44%, fanlights 19%
to 73%, original sashes 65% to 83%; an inter-war's scalloped bargeboards 13% to 25%, half-timbering 4% to 13%,
original casements 47% to 73%. Still rare by the houses' own make-up: spandrels and turned posts (6%: Victorian
porches are mostly recessed or canopies in tight yards), gabled and hipped dormers (most slopes too short or steep).

**Departures.** Wall reliefs are instanced, not merged: a course is one stretched instance a wall, so merging saves
nothing. Lights split rather than stretch. Doorcases' fanlights stand on the wall's face over the door, not in the
opening (their glass is opaque, set back in the doorcase's frame). Dormers stand on the slope with no opening cut
(their backs are buried in the tiles; only roofs of 30-52 degrees take them).

**Measured** (2026-11-17). `home_lab`, 2,600 rows an archetype, both mirrors: 76 instances a detached house (the
most 156), 59 a semi half (150), 44 a terrace unit (149); the dressed near shell 0.12-0.14 ms a unit (the bare one
0.12-0.16 for both levels); no instance mirrored, none different twice, no bad face or z-fight in a dressed shell.
Most placed a house: plastic frames 8, ridge rolls 8, pipe clips 6, sills 6-9, gutter stop ends 4. In the game
(noon, the slice's views, two runs each, `--homes bare` against the default):

| View | Bare | Dressed | Kit drawn |
|---|---|---|---|
| From the air (150 m out, 90 m up) | 7.94 ms | 7.94-7.96 (+0.0) | 649 instances at L2 (past 120 m the kit is gone) |
| Down the street, 30 m | 3.39-3.69 | 3.56-3.58 (+0.0 to +0.2) | 1,768 |
| At the front, 10 m | 3.22 | 3.38-3.46 (+0.16 to +0.24) | 1,801 (115 at L1) |
| Over the roofs (40 m out, 22 m up) | 5.13-5.14 | 5.30 (+0.17) | 1,940; 458 in the TLAS |

Within the budgets (+0.5 ms on the ground, +1.0 from the air). The pool fell a point (the shell lost its stack
caps and posts to the kit).

## The shared retrofit kit (HM5c1)

The user's concept sheets (`docs/reference/international-residential`, 2026-11-20: one sheet a style, panel 1 the
house as built, panels 2-6 its Neon City retrofits) put the same things on every house of every region, panel 1
included: black tanks and steel ones on the roof or on tall stands, solar panels on the slopes and on canopies with
downlights under them, copper and steel pipes up the walls, meter boxes, a lantern or a bulkhead by the door,
aerials, railings round flat roofs, breeze-block screens, pots, vines and plants trailing over parapets, gates
over doors and razor wire. HM5c1 is that kit, on **every house of a style with a sheet, retrofitted or not** (the
user); the six North American and British styles without one (craftsman, Spanish, ranch, tract, triple-decker,
modern) keep HM5's look. The insertions of panels 2-6 (box bays, cantilevers, struts, stair towers) are HM5c2's.

**The modules** (`src/gen/pmodel_retrofit.c`, `PMK_R_*`, 33 at three levels, frames in `pmodel_retrofit.h`; houses'
modules, so shaded by `shadeHouseKit`):

| Group | Modules | Sheets' panels |
|---|---|---|
| Roof | solar panel and a row of three (framed cells on clamps), canopy post (80 mm SHS on a base plate) and beam (140 x 60 RHS), downlight, LED strip; a black 1000 litre tank (ribbed, its lid, outlet and overflow; scaled for 500-2000 litres), a steel tank on legs, a braced stand bay (1.5 m, stacked) and its platform; a Yagi aerial and the old cross aerial on a mast | every panel 1-6's roofs; Victorian 1 (panels on the slope, a steel tank), inter-war 1-6 (canopies over the roof, black tanks, cross aerials), Africa block 1, 3-5 (tanks on tall stands) |
| Services | copper and steel pipes, their elbows (collared sockets), a gate valve, saddle clips, a meter cabinet with a conduit | RCC 1-6, shophouse 2-6, self-built 1-6, Georgian 1-6 (pipes and meters up the fronts) |
| Screens | breeze-block panels (diamonds, rings: three blocks of 0.4 m by three), a roof railing, a security gate (bars and a lattice, padlocked), a sheet gate, razor wire | Africa block 1-6, MENA villa 2-6, RCC 2-3, inter-war 3, 5 (boundaries, HM7's); RCC, self-built and shophouse roofs (railings) |
| Lights | a coach lantern on its bracket, a bulkhead | Georgian 1-6, Victorian 1, MENA 1-6 (lanterns), Africa and Japan (bulkheads) |
| Plants | a pot with a shrub and flowers, a palm or banana in a concrete pot, a trough, a climbing plant's card (HMv, below), trailing plants | every sheet: bougainvillea (MENA, Southern Europe, South Asia), vines up the fronts (inter-war 2, Georgian 2, 4, 5), plants over the parapets (shophouse 2, 4, 6) |

New materials (`PMAT_HGALV` on): galvanised or grey-painted steel with rust weeping from it, copper going brown
with some verdigris, tank plastic (black; a few blue, green or cream), solar cells under glass (a grid of 0.16 m cells,
the city's grime on the glass), breeze concrete (bare or limewashed), flowers (magenta, orange, red, white), and two
that glow: an LED's diffuser and a lantern's glass. A house's lights share their unit's seed (`homes.c`), so they
go on together, by the lamps' photocells (on all day under the cities' deck) and a share of houses by the hour
(`houseLightOn`). They are emissive only: lighting the walls round them (the light table, ray-traced shadows) is
HM6's. Downlights and LED strips cast no shadow. 23,018 triangles over the three levels (the library 404,400; its
budget raised to 430,000); every module watertight; budgets their measure plus a tenth.

**The grammar** (`src/gen/home_retrofit.c`, after the character layer on the same near shell). Each unit's owner
chooses (no row-wide set: neighbours differ), from its style's region's profile (the chances below), the row's
affluence (solar, canopies, LEDs, lanterns, steel tanks and plants rise with it; gates, razor wire, tanks on stands and
the old aerials with poverty) and its upkeep (tired and run-down thin the plants and lights, add gates and wire):

| Region (sheets) | Tanks | Solar | On a flat roof | Walls and door |
|---|---|---|---|---|
| Britain (Victorian, Georgian, inter-war, council) | 0.55: a cradle by the ridge, a flat extension's roof | 0.6: panels on a slope, a frame over the ridge (0.3) | railings 0.4 | copper risers, a meter, a lantern (Georgian pairs), cross aerials on the stacks, vines |
| North America (brownstone) | 0.6, steel | 0.55, canopies | railings 0.6 | lanterns at the stoop, pots up it |
| Southern Europe | 0.6 | 0.6 | railings 0.5, plants over the parapet | bougainvillea, pots, palms |
| East Asia | Japan 0.1, China and Korea 0.7 | 0.45 | railings 0.6 | meters (a bank in Japan), bulkheads |
| South-East Asia | 0.8 | 0.6, canopies | railings 0.7, trailing plants 0.6, breeze 0.4 | copper risers, plants, gates 0.35 |
| South Asia | 0.95, black; stands 0.35 | 0.5 | railings 0.8, breeze 0.5 | banks of meters, steel risers, gates 0.55 |
| Middle East | 0.9, low stands | 0.6, canopies | breeze 0.6 | lanterns (pairs), bougainvillea, palms |
| Sub-Saharan Africa | 0.9, on tall stands (2.5-4.5 m) in the yard | 0.4 | razor wire 0.4 | bulkheads, gates 0.6 |
| Latin America | 0.95, black | 0.6, canopies | railings 0.8, breeze 0.5 | meters, gates 0.5 |

Where things go: a flat roof's deck takes its tanks along the back, then a canopy over its front (2.3-2.6 m, the
lights along its street side), then its edges railings, a breeze screen on the street's parapet or razor wire on the
others, plants trailing over the street's parapet (stopping short of the windows under it), pots and an aerial. A
pitched roof takes its tank on a stand in the yard (where that is the custom), on a flat extension's roof or on a
cradle by the ridge (its legs down to the tiles), panels on a slope (the back mostly, the street's one in three)
or a level frame over the ridge, and an aerial lashed to a stack or on the ridge. On the walls: a light by the door
(a bulkhead over it where nothing stands there), a meter or a bank of them with a conduit up to the eaves, a
security gate over the door, risers (one to three, the street's wall mostly) up to the eaves and into the wall or
over a parapet and down onto the deck, a vine where a wall is clear to a window or the eaves (a trough at its foot),
an LED strip under the street's eaves, a breeze-block wall along a veranda, pots by the door.

**The rules**, as HM5's: every piece keeps out of the shell's solids (sampled), out of the windows and doors with a
margin, off a recess's opening, on its own wall, roof slab or deck, and out of what the character layer set down
there (downpipes, dormers, rooflights, vents, shutters, window boxes, porch posts: the occupancy, `hc_occupy`) and of
its own earlier pieces. A piece that finds no place is a logged repair (`repair char-solar`, `-tank`, `-riser`,
`-wall`, `-terrace`); `home_lab` counts them by reason.

**The budget.** HM5's 150 instances a house is raised to **230** (`UNIT_BUDGET`) for the kit the sheets put on every
house: about 20 more instances a unit on average (the six styles without a sheet included at none), a flat roof's
canopy and terrace 40-60 at most. A unit over it is dressed lean (HM5's), which also drops the retrofit's second vine,
LED strips, trailing plants, screens and deck pots.

### The vines (HMv)

The kit's first vines (tiles of leaf clumps on stems) read as green blobs. A climbing plant is now one card
(`m_r_vine`: two triangles at every level) standing 0.10 m off its wall (`HR_VINE_DEPTH`), stretched over the plant,
and the leaves are the card's steep parallax down to the wall (`shadeVine` in `shaders/city/house.glsl`).

- **The art** is the user's: one sheet of six plants side by side, generated with their image tool
  (`assets_src/vines/vines_six.png`, clean alpha). `tools/vine_pack.py` cuts each plant into a 256 px cell of one atlas
  (`assets/textures/vine`, `assets/materials/vine.json`), joins its top to its foot along the cheapest seam so the column
  repeats up a wall, fills the colour under alpha 0 from the nearest leaf, and derives the height (0 where nothing
  grows; the column's bulge, the photograph's lit leaves over the dark gaps, flowers over leaves), a normal from the
  softened height, occlusion and roughness (glossy leaves, matt petals). In order: clematis, ivy, wisteria,
  passionflower, bougainvillea, black-eyed Susan (`HR_VINE_*`).
- **The plant by region** (`vine_species`, the profile's bloom share choosing a flowering one): Britain and North
  America ivy or clematis; East Asia ivy or wisteria; Southern Europe and MENA bougainvillea, else passionflower or ivy;
  Latin America passionflower or bougainvillea; South and South-East Asia and Africa black-eyed Susan or bougainvillea.
  One plant a house (its owner's seed). It spreads as wide as the wall lets it, 1.1-2.6 m narrowing by 0.45 m to
  0.9 m, as tall as it can climb (to the eaves or under the first window).
- **The shader** grows the card as columns of the plant's cell, 0.72 m across and a 2.5 m tile, half a column apart
  (a card under 0.94 m takes one column stretched): each mirrored or not and started at its own height in the tile,
  the outer two stopping 0.3-1.6 m short, every top thinning to its highest leaves. The eye's ray steps down through
  the highest column's height (4-16 steps by how many pixels the depth spans; none far off or square on); where it
  reaches the wall with nothing met the pixel is cut. The atlas's alpha sets how many of the pixel's four samples it
  covers (dithered), so far off the leaves thin by their coverage rather than vanish. What the ray met shadows itself
  from the sun and each lamp (`reliefShadow`). The photograph's colour is taken a fifth darker and a little greyer,
  grimier near the ground, darker and glossier in rain.
- **Drawing.** A cut pixel still writes its depth (the kit's shader keeps its early depth tests), so the cards
  (`pm_module_cut`) are drawn after the rest of the kit and left out of the depth prepass: the wall behind takes the
  prepass, and its ray-traced shadows are the vine's. They cast none (a card would cast its whole rectangle). The
  kit's culling sphere now takes an instance's largest stretch (a card is far taller than it is wide).
- **Captures:** `city_test --home [--style S]` also prints up to three `home_vineN` views (each vine's plant and size),
  which `tools/home_capture.py --views home_vine0,home_vine1,home_vine2` takes.
- **Trailing plants** (over parapets, balcony rails, box tops, frame shelves and hexagons' sills) were still the old
  leaf blobs, which close to read as rows of bells (the user's snag, 2026-11-23). `hr_trail` now hangs the same card
  upside down in front of the edge's face (its thinning top become the strands' tips; `shadeVine` takes the card's up
  from its uv, so a hung card's parallax runs the right way) and stands a 0.32 m card of the same plant on the edge
  behind it, both cut edges at the edge's line: 4 triangles a run against ~1,100. The plant by region as the climbers'
  (`home_trail_species`) but never the wisteria, whose flowers would stand up. A short card thins over its top third
  (not 0.7 m), else only its petals were left.

## Each style's body to its sheet (HM5e1)

HM5c1's board put the sheets beside the game, and the user's verdict (2026-11-22) was that the bodies under the kit
missed their sheets: Britain's two-storey pitched terrace, with its porches, surrounds and stoops, stood in for every
other region. HM5e1 builds each style's **body** to its sheet's panel 1 (the checklist in the plan's "HM5e" section);
balconies (HM5e2) and the front walls and gates (HM5e3) follow, then the insertions (HM5c2). The user's calls: three
steps, a board each; the styles to their sheets' 3-4 storeys; shop fronts on rcc 0.3, self-built 0.4, shophouse 1.0.

| Style (sheet) | Body now | How |
|---|---|---|
| rowhouse (Brownstone) | 3-4 storeys, flat behind its cornice, a 1.1-1.5 m stoop; canted bays the main's full height on six rows in ten, masonry (no timber posts or aprons), dark trim | `bayFull`; the bay a flat-topped volume under the cornice |
| georgian | 3-4 storeys (the attic storey's short windows on the top one) | storeys |
| postwar (Council Estate) | no bays; flat door canopies | bay 0, canopy 0.7 |
| villa (Southern Europe) | mostly hipped | unchanged massing (its balconies: HM5e2) |
| townhouse | 3-4 storeys on the street line, half flat with a roof terrace, half a low pitch; doors in a reveal, two in five arched | `street`, `arched` |
| china | one-storey hutong rows under grey tile (a quarter of rows), else 3-4-storey flat concrete blocks; red double doors | the row's roof by its storeys; plank leaves (door pattern 4) |
| korea (walk-up villa) | 3-4 storeys over **pilotis** on four rows in five: the ground storey open on columns (a middle one past 6 m) for parking, the door and stair in a core at the back; flat or hipped (the mansard's stand-in) | `HP_PILOTIS` |
| shophouse | 2-3 storeys, tile pitch behind a parapet front, on the street line over a **five-foot way** (1.6-2.1 m deep behind 0.44 m piers), a shop beside every door at its back | `HP_ARCADE`, shop 1.0 |
| rcc | 2-4 storeys, flat, a **chajja** (a 0.5 m concrete sunshade slab) over every window, units built up unevenly (a storey or two short of the row's tallest on a quarter), shop fronts on three units in ten | chajjas in the shell; `HUnit.storeys`; shop 0.3 |
| bungalow | hipped tile, a deep veranda on most | weights |
| villa (Middle East) | 2-3 cubic storeys, flat, **crenellated** parapets on the street's front and returns, a big arched door (1.2-1.5 m) in an arched reveal, tall rooms (3.0-3.3 m) | merlons on the coping; arched openings |
| courtyard | 1-2 storeys on the street line, crenellated, blank walls but for a few small windows set high, one big arched door (1.4-1.8 m), tall rooms (3.2-3.6 m) | `street`, `arched` |
| block (Africa) | one storey mostly, hipped corrugated sheet at 22-28 degrees, a front veranda on columns | weights, sheet roofs 0.85 |
| walled | two storeys mostly, hipped | weights (the upper veranda: HM5e2; the compound wall: HM5e3) |
| selfbuilt | 1-4 storeys on the street line, flat, built up unevenly (two units in five a storey or two short), shop fronts on two in five | `street`, `HUnit.storeys`, shop 0.4 |
| colonial | one storey on the street line under a deep tile eave (0.6-0.9 m), tall rooms (3.3-3.8 m), an arched door on seven rows in ten | `street`, `arched` |

**Bays** stand only where a sheet shows them: Victorian, inter-war and the brownstone. Every other style's are gone
(home_lab fails a style with any).

**The ground floors.** A five-foot way and pilotis are porches (`HP_ARCADE`, `HP_PILOTIS`) of the recessed family
(`home_porch_recessed`): carved out of the main as a recessed porch is (its ceiling the floor above's soffit, its floor
paved or tiled), the door at their back, no stoop or steps. Pilotis carve the free sides too, leaving columns at the
corners. The walker walks into both (`city_home_outlines` with `walk`: piers, columns and the core); the grass keeps off
their footprints. **A shop front** (`HUnit.shopX0-1`) is an opening 1.8 m wide at least beside the door, in the front
wall or at the back of a five-foot way: `HF_GARAGE` with pattern 1, which `house.glsl` (`houseShop`) draws as a roller
shutter out of hours, else rolled up into its box over the shop lit by its own strip light (shelves of packs, a counter);
most shops keep 7-22, a few all night, a few never open. The ground storey's windows keep off all three.

**A street-line front** (`street`: townhouse, shophouse, courtyard, colonial, self-built) has no front yard (the plots'
front 0, no car pad, no front boundary): its door stands in a reveal 0.3-0.7 m deep carved from the pavement's level, with
no stoop out over the pavement. The rules ask for no front garden of these styles.

**Arched doors** (`arched`: the courtyard house, the Middle East's villa, colonial, two in five townhouses): the door
springs at 1.95-2.2 m, a half circle over it where the storey has room, else a segment (a fifth of its span at least,
else square), its reveal's arch over it; both are convex openings whose heads are tangent planes round the arc. Their
leaves, and China's red gates, are the shell's: a pair of plank leaves, iron-studded, ring handles (door pattern 4); the
kit sets no leaf, surround or canopy at them (its doors are square: HM5b1's carved surrounds).

**Unevenly built rows** (`HStyleDef.uneven`, flat rows only): a unit's own storeys (`HUnit.storeys`, never over the row's
main, which keeps the tallest); `home_unit_vol` gives a slice as it stands, and the shell, its far shell, the dressing and
the retrofit's walls and decks all build from it. Where a unit stands over a lower neighbour its party wall shows blank
above the neighbour's roof and its deck takes a parapet on that side; the dividing parapet stands to the lower one's top.

**Also:** door widths and storey heights by style (`doorW0-1`, `storeyH0-1`); steps' finishes to the sheets (stone and
concrete for the orange tile on most); a part against a party wall (an outrigger back to back with its neighbour's)
stops its verge or eaves at the wall; styles that build no chimneys build none on their terraces' party walls either.

`home_lab` surveys each style's body (storeys, the share 3+, flat roofs, bays, on the street line, five-foot ways,
pilotis, shop fronts, arched doors, built up unevenly) and fails a style short of its sheet; its new rules (`recess`:
carved out of the front of its main, under its first floor, a five-foot way's piers left standing; `shop`: in its unit,
clear of its door) are broken on purpose and caught.

## Balconies (HM5e2)

The second step of HM5e: the balconies each sheet's panel 1 shows (the plan's checklist), as a family in the graph
(`HBalcony`, `src/gen/home_balcony.c`), placed after the repairs (the volumes are final) from a stream of their own, so
every style without balconies keeps its houses exactly.

| Style (sheet) | Kind | Where | Units with any (home_lab) |
|---|---|---|---|
| georgian | **balconette**: an ornate iron rail (a band of rosettes under the handrail) on a 0.26-0.32 m stone ledge | every first-floor window, the row as one (an owner leaves it off one time in ten) | 0.73 |
| townhouse | **slab**, small (0.45-0.7 m), plain iron rail, stone consoles on four in five | every upper window its own (four in five) | 0.86 |
| villa (Southern Europe) | **slab** 0.8-1.2 m, iron rail, consoles on three in five | over the first floor's middle bay or two | 0.62 |
| china | **slab** with a steel rail, a third **glazed** | the flat blocks only, stacked, a bay or two | 0.53 |
| korea | **glazed** (a solid upstand, sliding glass to the slab over it) or **slab** with a steel rail, half each | over the pilotis on every storey, broad (two bays to wall to wall) | 0.86 |
| linkhouse | **slab** 0.9-1.2 m, steel rail | one first-floor bay | 0.49 |
| rcc | **slab** 0.9-1.3 m, steel rail | a bay or two, stacked on most storeys; chajjas shade the other windows (none under a balcony) | 0.74 |
| selfbuilt | **slab** 0.6-0.8 m, out over the pavement, one in seven never railed | a bay to wall to wall, stacked on some | 0.51 |
| villa (Middle East) | **loggia** carved 1.2-1.6 m in, under an arch on seven in ten, iron rail across its mouth | over the door, a bay or two | 0.80 |
| walled | **veranda**: the upper storey's front carved 1.8-2.4 m deep between 0.3 m piers (openings at most 3.2 m), a balustrade in each | the first floor's whole front | 0.41 (two-storey houses only) |

Victorian, inter-war, post-war, brownstone, Japan, shophouse (its louvred French windows are HM5b's), bungalow,
courtyard, block and colonial have none, and home_lab fails any that grows one. The chajja (an RCC window's sunshade
slab) was HM5e1's.

**The geometry.** A balcony stands on its unit's slice of the main's front, across whole bays of the window rhythm
(`home_front_bays`: a window a bay of about 2.7 m, as the shell lays them), so the window behind it becomes a **French
window** (`home_french_width`: 0.9-1.5 m, its sill a 3 cm threshold over the floor, its head 2.2 m up; the kit's
`h_win_french`, two glazed leaves on kick panels under a top light, no sill or window box). Its slab is a shell solid at
both levels (`SK_BALC`, 0.18 m deep, its top 4 cm under the floor: a step down out of the window), so it joins the wall
by the CSG and casts the shell's shadows (the walker's collision is the plan's outlines: every balcony is overhead);
a balconette's ledge is 0.12 m of stone; a glazed one adds a 0.12 m upstand (the unit's wall)
and the slab over it (the next storey's balcony where one covers it). A loggia is an opening carved into the body (its
ceiling, its tiled floor, an arched head as the doors' are), a veranda one carve an opening plus the space behind each
pier; their French windows stand at the back, one in each opening.

**The rules** (`home_check_balconies`, called by `home_validate`; `home_lab` breaks one on purpose, `balcony`): an upper
storey its unit has (an unevenly built unit's short top has none); within its unit's slice, 0.3 m off a party wall or a
corner, 0.9 m wide at least; a projecting one over its own plot, 0.3 m inside the boundary, or at most **0.8 m over the
pavement** from a street-line front (the townhouse's and self-built's, as such balconies are); a carved one leaves 3 m
of body behind it; nothing rising under it in front of the wall (a porch, a bay, a canopy) within reach of its slab; its
French window under the eaves; two on one storey of a unit 0.2 m apart at least. Where the chosen bays fail, the nearest
that fit are taken. Two repairs make room instead: **a balcony over a door canopy** (0.6 m deep at least, the door's
whole width under it) takes the canopy's place, its slab sheltering the door (the door rule accepts a porch, a canopy
or such a balcony); and **a veranda or loggia over a ground-floor porch** has the porch roofed flat under its floor (a
double veranda).

**The dressing** (`hs_balconies`, before the retrofit layer, which keeps out of the balconies' boxes): rails along the
front and back to the wall at both ends (plain or ornate iron with an end post, the retrofit kit's steel tube rail, or
HM4's turned balustrade on a veranda), the glazing along an enclosed one's front and ends, stone consoles under the
Southern European slabs; and the owners': pots and palms (by region, fewer where run down), plants trailing over the
rail, an LED strip under the slab's edge (most in East Asia). Kit `h_win_french`, `h_rail_iron`, `h_rail_ornate`,
`h_rail_post`, `h_glazing`, `h_console` (`pmodel_lab`).

## Boundaries and gates (HM5e3)

The third step of HM5e: the front walls, railings, fences and gates each sheet's panel 1 shows (the plan's boundary
column). HM2's garden always planned them (`HBound` lines along the street, between the front yards and round the back,
`HGate` gaps for the path and the drive) but nothing drew them. `src/gen/home_bound.c` gives them their style's heights,
tops and gates from a stream of its own (`home_bounds`, after the garden is laid), so the rest of every house is as it
was; the shell builds the solid parts, the kit what stands on them.

| Style (sheet) | Front | Gates | Also |
|---|---|---|---|
| rowhouse (Brownstone) | spear-topped area railings 0.9-1.1 m on a stone kerb | iron, on iron posts | iron rails up both sides of the stoop |
| victorian | a dwarf brick wall 0.45-0.7 m under spear railings (six rows in ten), or railings on a kerb | iron | |
| georgian | spear-topped area railings 1.0-1.2 m on a stone kerb | iron | stoop rails |
| interwar | a low brick wall 0.6-0.9 m, piers every 2.4 m, short iron railings on half | iron | |
| postwar (Council Estate) | a low brick wall 0.5-0.8 m, piers at its ends and gates, railings on half | iron | |
| villa (Southern Europe) | a rendered wall 0.7-1.0 m under iron railings, piers every 2.6 m | iron; a pair across a drive | lanterns on the gate piers (seven in ten) |
| japan | galvanised welded mesh over a block plinth (eight in ten), or a block wall | mesh | |
| china | the courtyard wall, grey brick 2.0-2.5 m under a tile cap | the gatehouse: deep piers, a lintel wall and a tile roof over red plank doors | |
| korea | a low wall 0.9-1.2 m; the pilotis' parking open to the street | steel bars | |
| linkhouse | painted steel railings over a rendered plinth, piers at ends and gates | steel; sliding across the car porch | lanterns (four in ten) |
| rcc | a rendered wall 1.0-1.4 m, a breeze band on a quarter | steel bars; sheet steel across a drive | |
| bungalow | a rendered wall 0.55-0.75 m under pierced ring blocks, piers every 2.5 m | iron | lanterns (eight in ten) |
| villa (Middle East) | a rendered wall 1.9-2.6 m, capped piers every 3 m | pierced steel, a pair across the drive | lanterns (nine in ten) |
| block | breeze blocks 0.9-1.2 m on a solid plinth (no wall on 15%) | sheet steel | |
| walled | a rendered wall 2.0-2.3 m, a breeze band under the coping on half, piers every 3 m | sheet steel, a pair across the drive | razor wire on the copings, the gate kiosk (beside a gate, its front in the wall's line), lanterns |

The street-line styles (townhouse, shophouse, courtyard, self-built, colonial) have none; North America's styles without a
sheet keep theirs planned and undrawn (`HBoundSet.drawn`). Hedges, and the timber fences behind the houses, are HM7's;
so are a compound's side and rear walls' detail (they are built plain).

**The geometry.** A drawn boundary (`home_bound_drawn`: walls, railings, and fences with a top) stands inside its plot:
the street's line from z 0 back by its thickness (`BOUND_T` 0.22 m, a railing's kerb 0.3), an outer side line in from the
plot's edge, a party line straddling it (`home_bound_rect`). The wall, kerb or plinth is a shell solid (`add_bounds`) with
a coping (a slab 4 cm proud each side, or China's tile cap: a ridge of the house's covering); a breeze band's coping
sits over the band. **Piers** (`home_piers`) stand at a front's ends (a party line's straddling it, a row end's set in),
at every gate (set back into the wall so the opening keeps its width), and along the wall every `pier` metres; a stub of
wall between two gaps is one pier. A party line's pier stands on the lower unit's ground and reaches the higher one's
top. Piers take a cap 4 cm proud. Far off (past 220 m) only walls 1.5 m and taller are built (their solid part, no
copings or piers): the compounds' and China's.

**The kit** (`hs_bounds`, after the balconies): the tops run between the piers (iron and spear railings, painted steel,
mesh in 1.8 m panels with a post at the end, breeze-block or ring panels stretched to whole blocks); gates' leaves
between the piers (one leaf, a pair, or a sliding one with its track; the retrofit kit's bar and sheet leaves, and the
new iron, pierced, mesh and steel ones); a railing's gate hangs on iron posts; lanterns on the gate piers' street faces;
razor wire along a compound's front copings; a stoop's rails (a newel, the sloped handrail, balusters on each tread, the
level rail to the door). The retrofit layer keeps off the walls' tops and the gates' swing. New modules: `h_rail_spear`,
`h_gate_iron`, `h_bar_iron`, `r_fence_steel`, `r_fence_mesh`, `r_gate_pierced` (`pmodel_lab`).

**China's gatehouse** and **the kiosk** are shell solids: the gatehouse's piers (0.5 m, deeper than the wall), its lintel
wall, a gabled tile roof along the wall and the plank leaves (door pattern 4, the house's red) both faces; the kiosk
(1.8-2.2 m square, `HE_KIOSK`, six compound rows in ten where the yard has room) a rendered room with a window to the
street and a door to the yard under a hipped roof of the house's covering. The wall is cut where the kiosk stands.

**The rules** (`home_check_bounds`, called by `home_validate`; `home_lab` breaks two on purpose, `gate` and `kiosk`): a
gate stands in a gap of its unit's front (no boundary across it); a pair's leaves 0.45 m at least; a sliding leaf has a
wall at least its width beside it to run behind (else it is a pair); gates and walls at most 3 m; a kiosk on its plot,
its front in the street's line, 0.3 m clear of the house, on no path, drive or car, the wall cut there. Owners in poorer
rows of South Asia, Africa and Latin America swap an iron gate for a steel security gate. `home_lab` surveys each style's
fronts (built, kinds, tops, mean height, gates, gatehouses, kiosks, razor wire, lamps, stoop rails) and fails a style short
of its sheet, or one without a sheet or on the street line that builds any.

**The walker** collides with the walls, piers, shut gates, gatehouse piers and kiosks (`city_home_outlines`, now up to
256 outlines a row), and the grass keeps off them.

## Retrofit insertions (HM5c2)

Panels 2-6 of every sheet are the same house since its owners rebuilt bits of it: boxes cantilevered off the front, bays
rebuilt rounded, angled frames, hexagons, fins, screens, stairs, towers, unfinished storeys, rooms on the roof. HM5c2 builds
them in three steps (the user's split, 2026-11-23): **HM5c2a** boxes and rounded bays and how an insertion joins its house;
HM5c2b frames, hexagons, fins and grids, tall screen strips; HM5c2c stairs, stair towers, unfinished storeys, roof rooms. The
plan's "HM5c2" section holds the survey of each sheet's panels and the table below comes from it.

**The draw** (`src/gen/home_insert.c`, `home_inserts`, after the boundaries: everything else of the house is final). Three
units in five are retrofitted (the user, 2026-11-20), each owner drawing one kind (seven in ten) or two from the style's
table, among the kinds the unit's body can take at all (a box needs an upper storey or a bay to replace, an unfinished storey
or a roof room a flat roof, a stair tower a front yard). Every draw is kept in `HGraph.insPlan`, built or not: the user chose
the final table from the start, so a unit whose kind is a later step's stays plain until then, and each board shows its
kinds at their final density. The draws are hashes of each unit's own seed, so no other part of a house moved, and HM5c2b/c
add builders without moving HM5c2a's boxes. The styles without a sheet have none.

| Style | Kinds drawn (weights) | Boxes' makes |
|---|---|---|
| victorian | box 0.45, round 0.25, screen 0.25, fins 0.05 | glass 0.6, concrete 0.25, screen 0.15 |
| georgian | box 0.25, fins 0.2, hex, tower, roof room 0.15, screen 0.1 | glass 0.85 |
| interwar | round 0.4, box 0.25, screen 0.2, frame 0.15 | glass 0.7 |
| postwar | box 0.5, frame 0.35, screen 0.15 | concrete 0.6 |
| rowhouse | box 0.35, round 0.2, frame 0.2, tower 0.15, screen 0.1 | glass 0.75 |
| villa (SEU) | box 0.3, round 0.25, roof room 0.2, frame 0.15, stairs 0.1 | screen 0.5, concrete 0.4 |
| townhouse | box 0.6, round 0.15, fins 0.15, screen 0.1 | concrete, screen 0.45 each |
| japan | box 0.45, fins 0.35, hex 0.2 | concrete 0.5, container 0.4 |
| china | box 0.7, screen 0.2, frame 0.1 | concrete 0.4, glass (caged) 0.3, screen 0.3 |
| korea | box 0.35, round 0.25, stairs 0.2, roof room, screen 0.1 | concrete 0.5, screen 0.4 |
| shophouse | box 0.45, round, screen 0.2, stairs 0.15 | screen 0.4, concrete, glass (caged) 0.3 |
| linkhouse | box 0.55, frame 0.25, round 0.2 | screen 0.6, glass (caged) 0.25 |
| rcc | box 0.4, stairs 0.2, round, unfinished 0.15, screen 0.1 | screen 0.45, glass (caged) 0.35 |
| bungalow | frame 0.3, screen, roof room 0.25, fins 0.2 | (one storey: no boxes) |
| villa (MENA) | box 0.45, frame 0.25, round, screen 0.15 | screen 0.7 |
| courtyard | box 0.6, frame 0.25, roof room 0.15 | screen 0.8 |
| block | unfinished 0.8, screen 0.2 | |
| walled | box 0.35, round 0.25, frame 0.2, fins, screen 0.1 | screen 0.6 |
| selfbuilt | box 0.3, frame 0.25, stairs 0.2, round, roof room 0.1, unfinished 0.05 | concrete 0.4, screen 0.35, container 0.25 |
| colonial | box, frame 0.25, screen 0.2, stairs, roof room 0.15 | concrete, screen 0.5 |

**Where a box or a rounded bay goes** (HM5c2a). On its unit's slice of the main's street front, across one or two whole bays of
the front's window rhythm (`home_front_bays`, as the balconies), so the windows beside it keep their places. Where the unit
has a bay (Victorian, inter-war, brownstone), a rounded bay most times (a glass or concrete box often) is built **in the bay's
place**, on the ground: the bay is gone, the new front up to 0.7 m wider each side (never over the door) and 0.2-0.5 m deeper,
within the front yard's margin, rising a storey over the old bay a time in three. Otherwise it hangs from the first floor (the
second, a time in four, on a taller house), one storey or two (a glass box three), 0.8-1.6 m out; a container one storey. Bays
without a balcony are tried first, the nearest to the drawn ones, then fewer storeys; a balcony is taken in only where no free
bay will do.

**The rules** (`home_check_inserts`, in `home_validate`; `home_lab` breaks one on purpose, `insert`): on its unit's slice,
0.3 m off a party wall or a corner (`INS_PARTY`); out over its plot to 0.3 m of the boundary, or at most 0.8 m over a
street-line front's pavement (`INS_STREET`, the balconies' figure); hung from a storey its unit has, its top under the eaves'
overhang, slab and gutter on a pitched roof, at the deck of a flat one, under a balcony over it (and that balcony's consoles);
its top storey with room for a window; nothing rising under it in front of the wall (a porch, a bay, a canopy) but a flat porch
roof it rests on (the walled compound's double veranda); on the ground only in its yard's projection margin and off the door,
the path, the drive, cars and the boundaries; no balcony of its unit on its storeys, no glazed balcony's cap under it; the unit's
door still sheltered; a unit's two insertions apart. **What it displaces**, each logged (`repair insert:`): the bay it replaces;
a door canopy under a box at the first floor that covers the door (its soffit is the door's shelter: the door rule accepts a box
as it accepts a balcony, `home_door_insert`); the balconies on its storeys (taken in: `HInsert.took`); a tree's place under a
grounded one.

**How it joins the house** (`src/gen/home_shell_ins.c`). Every insertion is convex solids (`SK_INS`) added to the shell before
its windows are cut, so `add_windows`' "something stands in front" test sees them and **no window is cut behind one**; its back
runs 0.05 m into the wall, and the CSG removes the wall behind it, as a balcony's slab joins. Its own windows are openings carved
into it, their reveals its finish, their frames modern dark paint. A hung one's floor slab is 0.22 m under its storey's floor
(`INS_SLAB`: over every recess's ceiling, which sits 0.25-0.35 m under it; at 0.3 a box's underside lay in a recessed porch's
ceiling's plane, home_lab's z-fight check found it).

| Make | Near | Far |
|---|---|---|
| concrete box | one solid of cast concrete (`HW_CONCRETE`: the PBR concrete set under panel joints every 1.2 x 0.6 m; pale cream, greys, buff, and the faded pink, mint and ochre the South Asian and Latin American sheets paint them), a window a storey across its front, on a deep one a narrow window in its sides | one solid, the house's windows painted on it at its storeys |
| screen box | the same, with a recess a storey across its front, glass at its back; the kit's breeze blocks (diamonds or rings) stand in its mouth, so a lit room glows through the lattice at night | as the concrete box |
| steel-and-glass box | painted steel bands at its foot, its floors and its top, posts at its corners, glass between them 0.05 m in (glass on a solid's faces, each face a pane: `emit_all` gives it its uv and size), mullions painted about a metre apart (`HG_CURTAIN`); black, grey or bronze steel; in South and South-East Asia and China mostly a cage, the kit's grilles over every pane | one solid of glass |
| container | one solid of corrugated sheet (the roofs' metal set on its walls, ribs upright; grey, rust, red, green, blue), a window or two | the same |
| rounded bay | bands of one convex plan whose front corners are rounded (3-6 chords a quarter by its radius): a plinth or spandrel, ribbon glass 0.08 m in, the head, a cornice slab 0.1 m out over its top; concrete, or the house's own render repainted | its plan at two chords a quarter, plain |

Every pane of one insertion's storey shares its key (`HOUSE_AUX_SHARED`, house.glsl), so a ribbon window is lit, and its blind
drawn, as one room. **The dressing** (`hs_inserts`, after the boundaries, before the retrofit layer, which keeps clear of it):
an LED strip along a hung box's soffit or downlights under it (most in Asia, the Middle East and Latin America; by affluence and
upkeep), a screen box's breeze blocks, a cage's grilles, plants trailing over a box's top. **The walker and the grass** keep off
one standing on the ground (`city_home_outlines`).

**Measured** (`home_lab`, 10,000 rows an archetype): every row accepted at level 0, none invalid or different twice, the rule
broken on purpose caught every time; shells (2,600 rows) with no bad face, none different twice, no z-fight. Retrofitted 0.59-0.62
of units in every style with a sheet, none in the rest; of the units drawing a box or rounded bay, 0.74-1.00 found a place (the
Victorian's and inter-war's tightest, 0.77 and 0.74: bays, porches and stoops fill their fronts). Built a unit: boxes 0.13-0.41,
rounded bays 0.06-0.22; on the ground in a bay's place 0.43 (brownstone) to 0.76 (inter-war) of those styles' insertions.
Against the same code with insertions off: the near shell +13 to +15 triangles a unit (805 a detached house, budget 1,000; 649
a semi half and 562 a terrace unit, budget 700), the far shell +3 (57, 55, 51; budget 60), the kit's instances a little fewer
(covered windows lose their frames). In the game (noon, Korea forced over Los Angeles' slice: its boxes and rounded fronts
are the densest; three runs each, the same build with insertions off): at an insertion from 10 m 3.99 ms against 4.09 (the kit
0.20 against 0.25), down the street at 30 m 4.22 against 4.30, over the roofs 5.19 against 5.20, from the air 8.63 against 8.60:
**no cost** past the runs' spread (the windows and their frames the insertions cover pay for them). Board
`build/home/hm5c2a/board_noon_a.jpg`, `board_noon_b.jpg`, `board_night_a.jpg`, `board_night_b.jpg` (17 styles: bungalow, block
and colonial have no box or rounded bay to show until HM5c2b/c).

### Frames, hexagons, fins and screen strips (HM5c2b)

The second step's four kinds are all **members**: convex prisms of the insertion's cast concrete, either a polygon in the
front's elevation pushed out from the wall (a frame's legs and beams, a hexagon's sides, a shelf, a strip's posts) or a
polygon in plan pushed up (a fin). `home_insert_members` derives them from the `HInsert` (its kind, a form `HIS_*`, the
members' width `w` and thickness `t`, a `lean`), and the rules, the shell, the dressing and the walker all read that one list.

**The windows stay.** A member touching the wall stands only on the piers between windows or on the floor lines between a
head and the next sill (`home_front_windows` lays the front's windows out as the shell cuts them, French windows where
balconies are, with the shell's 0.12 m margin). A member crossing windows stands at least 0.4 m off the wall (`INS_OFF`, past
the shell's 0.35 m "something stands in front" test), so the window behind is still cut, lit and seen through the gap; a
porch's or bay's windows the same, from its face. Only the hexagons replace the windows they cover: their glass is the new
window.

| Kind | Forms (the sheets' panels) | Built |
|---|---|---|
| frame | **portal** (two legs, a head), **chamfer** (its top corners cut: the council estate's 5), **lean** (one leg raked, a trapezoid: link house 3, Middle East villa 6, Southern European villa 5), **zigzag** (a diagonal a storey, turn about: the brownstone's 3), **apex** (two raked legs meeting over the middle: the compound's 5) | its plane 0.85-1.2 m out, members 0.32-0.45 wide and 0.3-0.38 thick; tied back by shelves at its floor lines (ledges, planters trailing over them) and a slab at its head where they clear the windows; on the ground in the front yard, in front of a porch, veranda or bay in its way (the bungalow's car porch frame), or hung from the first floor on a shelf; a breeze screen in one storey's cell a time in three, downlights under its head |
| fins | **blade** (raked in plan, a sawtooth: Georgian 3, townhouse 5): square on its pier out to 0.37 m, then raked 0.45-0.85 m in front of the windows to a tip 0.75-1.05 m out; with no pier (a narrow front) panels standing off the wall in front of the windows; **brise-soleil** (bungalow 6, Japan, the compound): thin upright fins a 0.4 m pitch standing off the wall between two cheeks; **grid** (Japan 2): blades and a shelf at each floor line, an egg-crate; **louvre** (Japan 5): slats over the top storey between two cheeks | over the whole front, half of it or a bay; blades may come within 0.05 m of a party wall (`FIN_PARTY`: thin, they pair up where both neighbours have them) |
| hexagons | **honeycomb** (Georgian 6): one, two, three or five flat-topped hexagons over two or three storeys, each a ring of six members round a hexagonal pane (a side two share is one member), plants over some sills; **hex-clad** box (Japan 4): a concrete box whose panels' joints are hexagons (`HW_CONCRETE`'s colour bit 0x10 in `house.glsl`), placed as a box | hung from the first floor (the second on a tall house), 0.5-0.7 m deep |
| screen strip | **strip**: a lattice one bay wide or 1.0-1.5 m, two to four storeys, between two posts and a head 0.62-0.75 m out, brackets back to the wall at the floor lines; the kit's breeze blocks, or the new **star lattice** (`PMK_R_BREEZE_STAR`: two squares a block, interlocking; the Georgian, Victorian, Middle Eastern and compound sheets' mashrabiya) | on the ground in the yard (in front of what is in its way) or hung from the first floor; the windows behind glow through it at night |

A one-storey house with no yard to stand in (a street-line front, or a South Asian bungalow's veranda filling its front)
takes its frame, brise-soleil or strip **raised**: on corbels 0.4 m over its floor (`HInsert.raised`), as a hung one. A hung
one's foot clears a canopy or porch roof just under it (it is lifted 0.18 m over it).

**The rules** (`member_fault`, in `home_check_inserts`): every member within the slice (0.3 m off a party wall, fins 0.05 m)
and its reach (0.8 m over a street-line front's pavement); under the eaves; one on the ground in the front yard's margin, off
the door's approach, the garden's paths, cars and tree places, and the boundaries; nothing in front of the wall under one (a
porch, a bay, a canopy) and none within 0.4 m in front of a porch's or bay's face; no balcony in its way (those it meets are
given up first: its shelves are the ledges now); none touching the wall in front of a window (but the hexagons); a hung
frame, strip or fins tied back to the wall; 0.05 m from the unit's other insertion (the sheets set strips hard by boxes:
Georgian 2, brownstone 5). The rule broken on purpose (`insert`) runs a box, a frame or a strip up to its party wall.

**The shell** (`home_shell_ins.c`, `add_members`): a solid a member (`SK_INS`), its faces in the wall none, its upper faces
copings; a hexagon's glass a pane on its front only (the rings bury its sides), lit as one room with its insertion. Past
220 m only the honeycomb's glass: frames, fins and strips are under a pixel there. The walker keeps off members' feet.

**Measured** (`home_lab`, 10,000 rows; 2026-11-23): 0 invalid, 0 not deterministic, 0 z-fighting in every archetype; the
rule broken on purpose caught every time. Of the units drawing one of this step's kinds, the share that found a place:
frames 0.57 (inter-war: bays, porches and stoops fill its fronts) to 1.00, hexagons 0.68 (Japan) and 0.76 (Georgian), fins
0.54 (Victorian: its bays leave few piers) to 1.00 (townhouse), screen strips 0.50 (the South Asian bungalow, its veranda
across the front) to 1.00; 4-12 members an insertion (the Georgian honeycomb most). Every form the table names was built.
Against HM5c2a: the near shell +29 to +33 triangles (838 a detached house, budget 1,000; 680 a semi half and 591 a terrace
unit, budget 700), the far shell unchanged (57, 54, 50; budget 60). In the game (Georgian, whose hexagons, fins and strips
carry the most members; night, three runs each, the same build with members off): at an insertion 4.89 ms against 4.92,
down the street at 30 m 4.23 against 4.17, over the roofs 5.15 against 5.06, from the air 8.48 against 8.46: **under
0.1 ms** everywhere (budget +0.5 on the ground, +1.0 from the air). Boards `build/home/hm5c2b/board_noon_a.jpg`,
`board_noon_b.jpg`, `board_night_a.jpg`, `board_night_b.jpg` (all 20 styles with a sheet; the villa (Southern Europe) and
RCC found two views, not three).

### Stairs and stair towers (HM5c2c1)

HM5c2c was split in two before starting (2026-11-23): the circulation on the front here, the roof's additions (unfinished
storeys, roof rooms) in HM5c2c2. The sheets: Korea 2 (a concrete dog-leg with solid upstands) and 6 (a straight flight), RCC
3 (a dog-leg across the front, steel rails), the self-built sheets' 1 and 3 (a straight flight, a dog-leg) and 5 (an open
steel stair), shophouse 6 (a flight from the first floor to the second), the Southern European villa's 3; Georgian 5 (a
glass stair tower past the parapet) and the brownstone's 6 (a glass service tower where the bay was).

**A stair** (`HI_STAIRS`, a member kind) rises from the ground along a wall of its unit to a **landing at an upper door** on
each floor it reaches: the owners split the house, the upper floor with its own way in from outside. **Which wall** (the user,
2026-11-23, after the first board: stairs must reach the ground, and stand "on the side of the property rather than the
front", mid-terrace ones at the back, where they are seen from the lanes HM2b cut between the plots): against the **front**
of a house with a front yard, or a **free side** (the side gap of a detached house, a semi's open side, a terrace's end unit);
where neither takes it, against the **back**, into the rear yard. **Never on a pavement**: a street-line house's stair is on
its side or its back. (The first build hung street-line ones from a first-floor door up to the next: dropped.) Each stair keeps
its wall (`HInsert.wall`, `HIW_*`; `home_ins_point` maps along it and out of it to the row's frame, and the rules, the shell,
the dressing, the walker and the views all read it). The door (0.9 by 2.05 m, the unit's door colour, panelled or glazed over)
is cut in the main's body where a window was (`add_stair_doors`; `add_windows` and `home_wall_windows` leave that window
out: `home_insert_door_at`). `home_stair_parts` lays it out from its door (`HInsert.door`), the way it rises (`lean`), its
flights' width (`w`) and tread (`t`):

| Form | Built |
|---|---|
| **straight** (`HIS_STAIR`) | one flight a storey against the wall from the ground to a landing 1.05 m long round the door (longer over the front door, below); on a side, rising away from the street (its foot by the pavement or the front yard) |
| **dog-leg** (`HIS_DOGLEG`) | two flights a storey: the first in the outer lane away from the landing, a half landing over both lanes, the second in the lane against the wall back to the landing; a second storey's climb stacked over the first (the same risers a flight, so its half landing stands over the first's); columns under the half landings' far corner and the landing's free corner; two lanes need 1.6-2.0 m (a rear yard mostly) |

Concrete: an inclined slab a flight (0.2 m plumb, its foot cut level on the ground) with a triangle a step on it, treads and
landings as steps (`HF_STEP`, tiled on some Southern European and colonial ones), and either **solid upstands** along the open
sides with a spine between a dog-leg's lanes (Korea 2: `screen`) or a kerb and the **kit's steel rails** (posts a metre or so
apart on the treads, a top and a middle rail along the slope, the roof railing round the landings). **Steel** (`cage`; Self
Built 5): two stringers a flight, plate treads, plate landings, posts under a grounded one's landings, all painted steel
(`HF_TRIM`). Risers at most 0.2 m (0.22 hung), treads 0.21-0.25 m, flights 0.72-1.1 m wide (narrowed to a shallow yard). A
bulkhead over each upper door, a pot on some concrete landings.

**Where:** the front and the free sides first, the back only where none of them takes it. On each wall (`stair_wall`: its run,
the slice at the front and back, the main's depth on a side; its reach, to the front yard's margin, the rear boundary less
0.3 m, the side boundary less 0.1 m) the placing tries every door (the wall's window places on the first floor, then every
quarter metre) both ways, in the drawn form first, and keeps the one that **costs the fewest windows** (those its doors take
and those its flights stand in front of: the shell leaves them uncut, a blank wall behind the flight; `home_wall_windows` lays
a back's or a side's out as the shell does), a window's place, the drawn way, the front over a side and the side its owner would rather (so a street's side stairs are not all one way) on a tie. On the front it
tries the landing **over the unit's front door** first (Korea 6): the landing is then the door's shelter and the canopy there
goes, as under HM5c2a's boxes (`home_door_insert`). A unit with no wall a storey's flight fits against at its steepest, or none
with room out of it for a flight, draws its next kind instead (`stair_room` in `feasible`: the plan's "a kind a unit's body
can't take is skipped"). That changed the draw of those units only; their other kinds are HM5c2a's and HM5c2b's placing.

**The rules** (`stair_fault`, in the row's frame): its wall one a stair may stand against; every member and door within the
wall's run (0.2 m off a party wall or a corner), its reach, within its own plot; nothing standing in its way (a porch, a bay,
a wing, an outrigger, an extension, a neighbour's too, a stack); on the front, balconies in its way given up and its doors
never onto a carved one; apart from the unit's other insertion, which is checked again with the stair in (its door moves the
windows other members are laid out by). Not the members' window rule (flights may stand in front of windows) nor the tie.
What is **under 2.2 m** of it keeps off the paths, drives, cars, sheds and boundaries (a flight rises over a path);
**nothing under the door's head and 0.15 m** stands in the front door's way, and a door set back in a recess (a pilotis'
core, a five-foot way's shop) keeps a way in 1.2 m wide through the recess's mouth; nor under 2.6 m in front of a shop front.
The rule broken on purpose (`insert`) pushes a stair off the end of its wall's run.

**A stair tower** (`HI_TOWER`, a glass box on the ground): a bay wide (1.9-2.5 m; over the whole of a bay it takes the place
of, the brownstone's 6: 3.0-3.2 m), 1.7-2.4 m deep in the yard, from the ground the house's full height. Over a flat roof or a
parapet it rises **2.6 m past the wall plate by a head** that runs back over the roof (`home_tower_back`: 1.0 m over a flat
roof's deck with a steel door out onto it at its back, 0.3 m over a Georgian parapet's coping); else under the gutter. Painted
steel posts at its corners and bands at its foot, its floors, the head's floor and its top, a cap proud all round; glass between
the bands with a **stairwell painted behind it** (`HG_STAIR`, `house.glsl` `houseStairwell`): a dog-leg's flights with their
rails seen with parallax (the near flight 0.6 m in, the far one 1.3 m, the back wall 1.9 m), dark against a lit wall at night
(a stairwell's lights, on most of the night whatever the homes' hours), dim by day; the ground storey's front its glazed door
(`HG_STAIR_DOOR`). A steel riser up one side half the time (the brownstone's), plants trailing over its cap. Far: its glass,
head and cap. The windows behind it are inside it (not cut).

**Measured** (`home_lab`, 10,000 rows; 2026-11-23, with stairs on the sides and backs): 0 invalid, 0 not deterministic, 0
bad faces, 0 z-fighting in every archetype; the rule broken on purpose caught every time. Of the units drawing a stair, the
share that found a place: Korea's 0.99, RCC's 0.95, the Southern European villa's 0.94, colonial's 0.89, the self-built's 0.76,
the shophouse's 0.67 (against the front alone they had been 0.24-0.60); home_lab's floor for stairs is 0.5, as the other kinds'.
Their walls: the street-line styles' on a side a sixth to two fifths of the time (the self-built's 0.36, the shophouse's 0.40,
colonial's 0.73), the rest at the back; the yard styles' at the front a quarter to a third of the time, on a side (RCC 0.28, the
villa's 0.19; Korea's 0.5 m side gaps take none) or the back for the rest (0.58-0.66). Dog-legs: two fifths to a half (most
at the back, where the yards are deep); steel: a third of the self-built's; solid upstands: over half of Korea's. 20-25
members a stair; 0.5 to 1.6 windows a stair costs on its wall (4 at most). Forced into Los Angeles for the captures, a style
takes LA's yards and side gaps (Korea's side stairs on the board are LA's). Towers: the brownstone's 0.60 (in the bay's place), Georgian's 0.52 (its doors and area
steps take the rest), every one with a head. Against HM5c2b: the near shell +6 to +8 triangles (846 a detached house, 686 a
semi half, 597 a terrace unit), the far +1 (58, 55, 51; budget 60). In the game (night, three runs each, the same build with
stairs and towers off): at a Korean stair 4.69 ms against 4.74, down the street at 30 m 3.81 against 3.81, at a Georgian
tower 4.66 against 4.67, from the air over a Georgian suburb 8.60 against 8.49: nothing on the ground, **+0.11 ms from the
air** (budget +0.5 on the ground, +1.0 from the air). Boards `build/home/hm5c2c1/board_noon_a.jpg`, `board_noon_b.jpg`,
`board_night_a.jpg`, `board_night_b.jpg` (Korea, RCC, self-built, shophouse; the Southern European villa, Georgian, the
brownstone). The near shell's triangles with the stairs on sides and backs: 850 a detached house, 693 a semi half, 602 a
terrace unit; the far 58, 57, 53 (budget 60). The frame again with them (self-built, night, three runs each, with and without
stairs and towers): down the street at 30 m 3.30 ms against 3.29, from the air 8.17 against 8.16. Board
`build/home/hm5c2c1/board_closeups.jpg` shows stairs close to (home_lab's renders) on each wall: a side's in a narrow gap is
seen only glancingly from the street, as a passer-by would.

### Unfinished storeys and roof rooms (HM5c2c2)

The roof's additions (`src/gen/home_insert_roof.c`; shells in `home_shell_ins.c`). The sheets: block 2 (an unfinished upper
storey on starter bars over a one-storey house), RCC 1 and the self-built sheets' 1 (column stubs with their bars on the roof
as built), RCC's and the self-built's storeys going up; Korea 4, courtyard 5 and self-built 5 (rooms on a flat roof, a
container among them); Georgian 3, 4 and 6 (glass pavilions behind the parapet, 4's a drum), the Southern European villa's 4
and 6, bungalow 4 (a container through the hipped roof), colonial 4 (a screen room on the roof). The user's calls
(2026-11-24): (1) on a pitched roof a room is a **box dormer** through the front slope; (2) **block's deck**: the owner takes
the pitched roof off a one-storey house and casts a flat deck to build up on; (3) the **stubs** stand on most RCC and
self-built roofs as part of the house as built. Each kind's use and the way up to it:

| Kind | What it is for | How it is reached |
|---|---|---|
| **unfinished storey** (`HI_UNFINISHED`) | the next storey, built as money comes: walls each to its own height, holes where its windows will go, columns cast to the next floor with their bars out, no roof | the house's own stair carries on up; a one-storey house has a builder's ladder against it |
| **stubs** (not an insertion: `home_stubs`) | the columns cast for a storey not built yet, their bars out | none: they stand on the roof terrace's parapets |
| **roof room on a flat roof** (`HI_ROOFROOM`) | a room on the terrace: glass, a container, a screen room, a concrete room; a door onto the terrace and a window | the house's stair to the roof |
| **roof room as a box dormer** (`raised`) | a loft room rising through the front slope from the top storey's ceiling, behind the gutter or the parapet, its top under the ridge | from inside (a loft conversion) |

**An unfinished storey** stands on its unit's whole slice of a flat main's deck (under `HOME_STOREYS_MAX`), its storey the
unit's next (`s0 = s1`). Its **walls** stand on the deck's edge, on the parapets (`UNF_WALL`, the parapet's thickness, so the
house's finish runs up the parapet's 0.7 m and the new work above it), each to its own height (`cells`: not yet, to the sills
0.93 m, to the windows' heads 2.21 m, or full, the storey less 0.45 m under a ring beam to come): block's all round most times
(its panel 2), the rest a wall at a time with the front first (RCC's every wall full one time in fifty). Bare grey block
(block's; dark grey a time in five), hollow clay brick (the self-built's panel 1) or either (RCC's). **Holes** through them where
the storey's windows will go (`home_unf_holes`: the front's and back's at the window rhythm's bays, a free side's in some of its
spans; none in a wall only to the sills, none on a party line, where the neighbour may build up against it), a notch to the
wall's top where it stops near their heads. **Columns** (`UNF_COL`, cast concrete) at its corners, on the bay lines front and
back and every 3.6 m along its free sides, to the next floor's slab, starter bars (`PMK_R_REBAR`) out of each. A side on a
party line where the neighbour already has the storey is the neighbour's wall (no wall, no columns there). A one-storey house's
storey has a builder's timber ladder (`PMK_R_LADDER`) leaning against a free side, else the front, clear of its windows.
Far: its walls, no holes or columns.

**Block's deck** (`cage`): a block house drawing an unfinished storey that stands on its own (detached), one storey under a
pitched roof, has its roof taken off and a flat deck cast (`cast_deck`: the main made flat, no parapet, its slab's edge 0.12 m
proud), logged as a repair; the deck is kept only if the whole house still keeps every rule with it (`home_validate` on a
copy), and the rules let that deck go without a parapet (`home_vol_cast`). Block's semis and terraces share one pitched roof
and draw a screen instead.

**The stubs** (`home_stubs`): seven RCC and self-built flat roofs in ten (none under an unfinished storey), a stub on each bay
line front and back (the corners among them), `STUB_W` square and 1.0-1.6 m over the deck (0.3-0.9 m past the parapet), bars
out of each. They stand **on the parapets' copings**, a millimetre over them, so neither cuts the other's faces (embedded,
they cut the parapets into fragments: 7 triangles a unit on the archetypes' average). A party corner: one stub on the line
where the neighbour has as many storeys (the west unit's, or the east one's where the west has none), none where the
neighbour stands taller, in on its own parapet over a lower one. None where a room or a stair tower's head (and the way out of
its door) stands.

**A roof room** on a **flat deck** stands at its back (0.4 m off the back wall, its front 1 m or more behind the front wall),
at an end of the slice or its middle, 0.3 m off a party wall, 2.4-4.4 m wide, 2.4-3.6 m deep, its top 2.5-2.8 m over the deck;
as a **box dormer** on a gable with its ridge along the street or a hip (the ridge 2.2 m over the wall plate at least), its front
0.5 m behind the wall's face (behind the gutter) or 0.6 m (behind a Georgian parapet), its top 0.3 m under the ridge (on a hip
lowered until a 2 m room fits inside the hips at its top), its back where its top meets the slope. Its box runs down 0.3 m
under the wall plate, so the roof's slab and the body hide its foot and the CSG joins them; its front's window stands over the
roof's covering there (`home_room_sill`). **The makes** by style (`ROOM_FACE`): Georgian's glass (a rounded front, the drum, a
time in three), the Southern European villa's glass, concrete or screen, Korea's concrete, glass or a container, the bungalow's
container or concrete, the courtyard house's concrete or screen, the self-built's container or concrete, colonial's screen or
concrete. A **concrete** room: its body and a roof slab proud all round, 0.6 m out over the door on a flat deck (a hood, a
downlight under it), a door (the unit's door colour) and a window or two, a narrow side window a time in two. A **screen** room:
its front a recess 0.4 m deep, glass at its back, breeze blocks in its mouth beside the door. A **container**: corrugated steel
all round (2.44 m by 2.59 m, 2.99 or 6.06 m long along the street), a door and windows cut, its windows barred half the time,
a bulkhead over the door; a container set through a slope (bungalow 4) may stand over the hips and its top up to 0.3 m past
the ridge (a box set on the roof, as the sheet's), else the room is built of concrete. **Glass**:
a foot band (a dormer's up to just over the covering), a roof band, glass between (`HG_CURTAIN`), painted steel posts at the
square corners, a glazed pair for its door on a flat deck. Pots by the door, plants trailing over a flat room's roof edge.
Far: one solid of its make.

**The rules** (`home_roof_fault`): an unfinished storey on its unit's whole slice of a flat roof with parapets (or the deck
cast for it); a room within its slice 0.3 m off a party wall, on a flat roof within its deck's rectangle, as a dormer behind
the gutter or the parapet, under the ridge, inside the hips at its top (a container: over them, 0.3 m past the ridge at
most), clear of any roof run up into the front slope (a bay's, a porch's); clear of the stacks; apart from the unit's other insertions (their boxes may touch, never cross; a stair tower's
head with a metre behind it, its door's way out onto the roof). The roof room's draw (`home_roof_feasible`) now admits a
pitched roof's slope, so the Georgian, Southern European villa, bungalow, colonial and Korean units' draws changed (which
units get which kind; the kinds' table is the plan's). The rule broken on purpose (`insert`) runs an unfinished storey past its
slice or a room up to its party wall.

**Measured** (`home_lab`, 10,000 rows, 2026-11-24): 0 invalid, 0 not deterministic, 0 bad faces, 0 z-fighting; the rule broken
on purpose caught every time. Of the units drawing an unfinished storey, those that built one: RCC 1.00, the self-built's 1.00,
block's 0.92 (its deck kept; 0.04 of block's units in the lab's even mix of archetypes, its detached one-storey houses); a
roof room: 0.82 (bungalow) to 1.00. Rooms as dormers: Georgian's, the villa's and colonial's all, the bungalow's two in three,
Korea's one in five; Georgian's glass 0.87 (a third rounded), the bungalow's containers 0.66, the self-built's 0.59. Stubs on
0.70 of RCC's flat roofs and 0.69 of the self-built's, about five a roof. The near shell: 861 triangles a detached house, 699
a semi half (budget 700), 607 a terrace unit, against 849, 691 and 600 with the roof's kinds off; the far 60, 57, 54 (budget
60). In the game (night, three runs each, the same build with the roof's kinds and stubs off): an RCC street 3.60 ms against
3.59, from the air over it 8.28 against 8.31, from the air over a Georgian suburb 7.31 against 7.28: **+0.03 ms at most**.
Boards `build/home/hm5c2c2/board_noon_a.jpg`, `board_night_a.jpg` (RCC, block, self-built, Georgian, bungalow),
`board_noon_b.jpg`, `board_night_b.jpg` (colonial, the Southern European villa, Korea, the courtyard house): the sheet, then
two views from across the street (a roof room's from a neighbour's upper window, its eye 2.5 m over the deck) and one from the
air (`city_test --home` with `NC_INS_KINDS=0x300`: `home_ins0..1`, `home_air0`); `board_closeups.jpg` (home_lab's renders).

## The other regions' kit (HM5b1)

What the sheets of the regions beyond Britain and North America show that HM4's and HM5c1's kits lack, built as modules
(`src/gen/pmodel_region.c`, frames in `pmodel_region.h`) at three levels; placing them by style is HM5b2's. The style
paragraphs they answer are in the plan's section "HM5b1: the other regions' kit, style by style" (signed off
2026-11-24, with the split by region and the wall finishes and pointed openings left to HM5b2).

**HM5b1a, East and South-East Asia** (29 modules, `PMK_H_WIN_SLIDING` to `PMK_H_WIN_HOOD`):

| Style (sheet, panels) | Modules |
|---|---|
| Japan (1, 4, 5) | aluminium sliding window, clear and frosted; storm-shutter (amado) box and its rail; door hood (hisashi); slat screen |
| China (1, 2) | ridge end; eave-end tiles; ring knocker; drum stone; couplet; fu diamond; paper lantern; lattice window; projecting window cage |
| Korea (1-5) | hazard sleeve; mailbox bank; Korea's ridge end and fine-grid lattice (the eave tiles shared) |
| Shophouse (1, 5) | pilaster; round column; louvred French window; vent panel; parapet crest; swing half doors (pintu pagar); canvas awning |
| Link house (1, 5) | curved car-porch canopy; gable vent; window hood |

Six materials (`PMAT_HALU` to `PMAT_HFROST`, shaded in `shaders/city/house.glsl` shadeHouseKit): anodised aluminium
(silver, bronze or black, picked by the unit's trim colour so a house's metal matches), red paper with brushed
characters (four strokes a character, gold or black by the unit; a couplet's every 0.2 m, the fu one big one, a
tassel plain), the paper lantern (red, lit by the unit's lights like HM5c1's lanterns), hazard stripes (0.4 m diagonals
in the world's metres, scuffed low down), striped canvas (five pairs by the unit, 0.15 m stripes along the wall), and
frosted sheet (milky polycarbonate; a window's pane or a lattice's paper, piece 1, glows warm when the unit's lights
are on). The lantern, the frosted window and the lattices share the unit's light seed (`home_kit_light`).

**Measured** (`pmodel_lab --only house`, 2026-11-24): every module closed, deterministic, no bad normals; L0 12-1,728
triangles (the Chinese lattice the most, then the louvred French window's 1,500), L1 12-1,248, L2 12-212; about 21,000
over the three levels together. The library's budget rose from 430,000 to 480,000 for HM5b1's three kits (it stood at
410,710; 431,498 with HM5b1a). Vignettes on stand-ins of each sheet's panel 1 (japan, china, korea, shophouse, linkhouse): 25-44 instances,
within a house's 150. Boards `build/home/hm5b1a/board_vignettes.jpg` (sheet beside vignette) and `board_modules.jpg`
(the new modules at their levels). Not yet seen in the game: nothing places them until HM5b2.

**HM5b1b, South Asia, the Middle East and Africa** (16 modules, `PMK_H_GRILLE_ORN` to `PMK_H_CCTV`):

| Style (sheet, panels) | Modules |
|---|---|
| RCC (1, 3, 4, 5) | ornamental window grille (lozenges, rings, a sunburst); precast pierced balustrade; corrugated tin awning |
| Bungalow (1, 2, 5) | fretwork frieze (scalloped arches, drops, rings); the deco parapet's pediment and roundel (the round column and the pierced balustrade shared) |
| Middle Eastern villa (1, 4, 5) | pointed arch surround (jambs, moulding, carved spandrels, alfiz); girih screen; mashrabiya oriel; wind tower |
| Courtyard (1, 2, 4) | studded door, square and arched; small timber lattice grille; waterspout (the surround and the mashrabiya shared) |
| Block (1, 4) | glass louvre (jalousie) window; barred security door |
| Walled compound (3) | CCTV camera on its bracket (the round column and balustrade shared) |

The pointed arch is two-centred (`HK_ARCH_*`: 1.6 m between the jambs, springing at 2.1 m, apex 3.16 m) and shared through
`hk_arch_y`/`hk_arch_apex`, so HM5b2's openings, the surround and the arched door agree. Until the shell cuts pointed
openings (HM5b2), the surround fills the corners of a square opening up to the apex back 0.35 m into the wall, so a square
opening reads as arched. The mashrabiya carries frosted paper behind its lattice (piece 1, lit by night with the unit's
lights: `home_kit_light`). One material more, `PMAT_HTIN` (79): corrugated tin, bare zinc or faded red, green or blue paint
by the unit, rusting in blotches and in runs down the fall (the vendors' `kit_rust_sheet` grain).

**Measured** (`pmodel_lab --only house`, 2026-11-24): every module closed, deterministic, no bad normals; L0 36-2,112
triangles (the mashrabiya the most, then the girih's 1,920), L1 36-1,068, L2 28-324; about 24,400 over the three levels
together; the library at 455,918 of its 480,000 (HM5b1c's kit has the rest). Vignettes on stand-ins of each sheet's panel 1 (rcc, bungalow, villa_mena, courtyard, block, walled): 12-39
instances, within a house's 150. Boards `build/home/hm5b1b/board_vignettes.jpg` and `board_modules.jpg`. Not yet seen in
the game: nothing places them until HM5b2.

## Massing and parts

| Part | Size | Notes |
|---|---|---|
| Storey | 2.7-3.0 m floor to floor (old houses' tall rooms more: courtyard 3.2-3.6, colonial 3.3-3.8, shophouse 3.1-3.5, Middle East villa 3.0-3.3); 1-4 storeys (HM5e1) | ground floor 0.15-0.45 m above the garden (a step or a stoop; rowhouse stoops to 1.5 m; on the street line 0.05-0.15) |
| Storeys | 1 (ranch, bungalows) to 3 (rowhouses, Georgian); 2 most | |
| Main volume | a rectangle, depth 7-10 m (terraces 8-11, ranch 9-12) | |
| Roof | gable, hip or lean-to spanning the short side; pitch 25-50° (§13.5's 25° floor); span at most 12 m | ranch 18-25°, a departure: real ranches are shallower than §13's floor; they are hips over short spans so they drain |
| Intersecting roofs | a wing's, outrigger's or rear extension's roof meets the main one: valleys, a lower ridge running in, a lean-to against a wall | HM3's hard geometry |
| Eaves | overhang 0.2-0.6 m (craftsman 0.6-0.9, georgian 0 behind a parapet) | soffit, fascia; rafter tails or brackets by style |
| Chimneys | stack 0.6-1.2 m wide, 0.45-0.6 m deep, 0.9-1.5 m over the ridge; a pot a flue | on a ridge, a gable end or a party wall, never mid-slope (§13.4) |
| Porch | depth 1.2-2.0 m; craftsman verandas to 2.5 m | over every front door |
| Bay window | projection 0.6-0.9 m (0.45 at least); square 1.8-2.7 m wide; canted set out from its front light, 1.1-1.5 m (1.0 at least, else square), its sides at 45 degrees, so 2.3-3.3 m overall; one or two storeys | inside the front yard, short of the boundary (no bay where the yard leaves under 0.45 m) |
| Window bay | 2.7 m (§13.7) | near: real reveals, glass set back 0.08-0.15 m, sills, frames, glazing bars |
| Door | 0.9-1.0 m wide (§13.5's 1 m clear with its frame), 2.0-2.1 m high | |
| Dormers | 1.2-2.4 m wide, gabled, hipped or flat | |
| Boundaries | walls 0.45-1.4 m, compounds' and China's 1.9-2.6 m, railings 0.9-1.2 m on a kerb, hedges 0.8-1.8 m, fences 1.2-1.8 m, gates 0.9-2.6 m (each sheet's: "Boundaries and gates") | na: mostly no front boundary (open, paved or gravelled fronts and driveway aprons; no lawns in a city since HM2b) |

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
