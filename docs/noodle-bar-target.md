# The noodle bar slice: target sheet

The target for Part A of the street vendor rework (NB0-NB8 in the street vendor roadmap): one canopy noodle bar,
built procedurally to final quality before any breadth. Every later step is judged against this sheet, the
references and the office interior impostors (the quality bar), not against `docs/street-vendor-generation.md`'s
browser-prototype budget.

**References** (the user's, untracked in the repository's root): `newVendorRef1.jpg` and `newVendorRef2.jpg` (concept
boards: the massing, the hood, the counter, the kitchen and its details), `vendorRef1.png` and `vendorRef2.png` (a
canopy noodle bar under neon in rain; one in steam and crowds). Mood and construction are taken from them; the
design stays our own: no film's or game's bar, dragon sign or tiled pillar is reproduced.

## The slice site

| | |
|---|---|
| Where | The hawker frontage (apron) site holding (332.60, 1182.58), home city frame (`VENDOR_SLICE_X/Z`, `src/city/vendor_sites.h`) |
| Site | 3.85 m frontage x 3.48 m deep, its back against the row's wall (hostWall), open sky, floor at Y -21.36 |
| Faces | (0.54, -0.84): onto the street, walk 1.0 m in front |
| Affluence | 0.24: poor (the references' rust and grime are its natural condition) |
| Forced | archetype canopy, trade noodles (`VendorSite.slice`) |
| Check | `city_test --slice`: exactly one slice, a canopy noodle bar; its views and the office's |

If the city's plan moves the site (SF3 re-weights the sites), `city_test --slice` fails; pick a new one with
`city_test --slice find X Z` and update the constants and this table.

## Parts and dimensions

The site's frame: x across the frontage, z out from the back wall (0) to the front (3.48), y up from the floor.
Sizes are targets; the generator may vary them within the ranges, the slice's first build sits mid-range.

### The frame and walls
| Part | Size | Notes |
|---|---|---|
| Corner posts | 60 x 60 mm box or 50 x 50 x 5 angle | front corners, carry the hood's front beam |
| Side walls | full depth to the counter's back, 2.3 m to the hood's spring | riveted sheet panels ~1.2 m wide on angle framing; laps 25 mm; rivets 6 mm heads at 60 mm pitch along seams |
| Back wall | the host wall, a steel lining 0.1 m off it | splashback in front of the range |
| Splashback | 0.9 to 1.8 m above the floor, the range's width + 0.3 m | small white tiles 100 x 100 (or 150 x 75 brick bond) with dirty 2 mm grout, or stainless sheet |
| Floor | the apron's paving; a 20 mm steel chequer plate behind the counter | |

### The hood (the silhouette)
| Part | Size | Notes |
|---|---|---|
| Form | barrel vault, its axis along the frontage; curving down to the front | straight on a wall site, wrapping round on a corner (NB4's second form) |
| Span (front to back) | the site's depth + 0.6 to 0.9 m overhang past the counter | the overhang shelters the stools |
| Spring height | 2.35 to 2.5 m (front lip) | clear of V_CLEAR_MIN 2.3 m |
| Rise | 0.6 to 0.9 m above the spring | top at 3.1 to 3.4 m |
| Ribs | 60 x 30 mm, pitch 0.45 to 0.6 m | standing above the sheet, front to back |
| Panels | sheet 1.5 mm between ribs, lapped 25 mm, rivet rows along the laps | rusted copper-brown, streaked from rivets and seams |
| Rolled lip | 50 to 60 mm round, along the front edge | |
| Fascia | 0.30 to 0.40 m tall under the lip, full frontage | carries the icon signs |
| Icon signs | 0.35 to 0.45 m square, 0.8 to 1.0 m pitch | bowl, chopsticks, steaming bowl, cup: lit boxes (generic shapes) |
| Strip lights | T8 tubes 1.2 or 1.5 m under the fascia, 1 to 3 | in a shallow steel channel |
| Ends | closed by the side walls' arched tops (tympana) | |

### The counter and stools
| Part | Size | Notes |
|---|---|---|
| Height | 1.00 to 1.05 m | |
| Top | 0.40 to 0.50 m deep, 3 mm worn steel, 40 mm turned-down edge | a raised pass shelf behind it at 1.25 m optional |
| Front | panelled, panels 0.5 to 0.7 m wide, riveted frames | kick plate 0.1 m, dented |
| Foot rail | 40 to 50 mm tube, 0.20 m high, 0.25 m out, on brackets | |
| Stools | seat 0.36 m round, 0.75 m high, 60 mm pedestal, 0.40 m base | pitch 0.62 m; 5 on the slice's frontage; 0.3 m out from the counter |
| Cook's aisle | 0.9 to 1.0 m behind the counter | the reach rule: everything the cook uses within 0.7 m of a place in it |

### The kitchen line
| Part | Size | Notes |
|---|---|---|
| Back bench | 0.60 to 0.70 m deep, 0.90 m high, steel, undershelf | the range and bain-marie stand on or in it |
| Range | 2 burners on a 0.45 m pitch (or 3) | cast-iron grates of 15 mm bars 0.45 x 0.45; knobs 40 mm on the front |
| Wok burner | ring 0.35 m, a raised cradle | the second form may swap a burner for it |
| Stock pots | 20 to 40 l: 0.30 to 0.40 m round and high | copper and steel; lids, loop handles; broth inside |
| Steamer stack | 0.30 m round, 2 to 4 tiers | bamboo or steel |
| Bain-marie | 3 to 4 GN 1/3 pans (325 x 176 mm, 100 to 150 deep) | lids with handles, one open (curry); dials, two pilot lights |
| Rice cooker | 0.35 m round, 0.30 high | |
| Utensil rail | 0.6 to 1.0 m long at 1.5 to 1.6 m | spider, ladle, slotted spatula, cleaver, S-hooks |
| Shelves | at 1.45 and 1.80 m, 0.25 to 0.30 m deep, on brackets | stacked bowls (0.18 to 0.20 m round), labelled jars (0.12 to 0.20 m), bottles |
| Extractor canopy | over the range, 0.6 m deep, lip at 1.95 to 2.0 m | a fan (0.3 to 0.4 m) in a grille; duct 0.25 to 0.30 m round up the host wall, bends and flanges |
| Gas bottles | at most 2 (V_GAS_MAX): 47 kg (0.37 round, 1.3 high) or 13 kg (0.31, 0.6) | chained; the gas line to the range |

### Services (the dressing that sells "built into the city")
| Part | Size | Notes |
|---|---|---|
| Gas line | 15 to 22 mm pipe with elbows and saddle clips | a pressure gauge (63 to 100 mm face, glass) and a valve (wheel or lever) |
| Power panel | 0.4 x 0.6 m riveted box | 3 ceramic insulators, 4 toggle switches, hand-written numbers, a warning sign |
| Cable bundles | 10 to 25 mm cables, catenary sag between clips | from the panel to the lights, the fan, up and off the site |
| Conduits and pipes | 20 to 60 mm, along the walls and over the hood | with brackets and flanges |

### The front
| Part | Size | Notes |
|---|---|---|
| Menu A-frame | 0.6 x 0.9 m | chalk board in a timber frame |
| Bell | 85 mm brass dome | on the counter or the hatch's sill (the service hatch variant) |

## Materials

Design first; NB3 fetches what this needs from ambientCG (packed at 1024 by `tools/pbr_pack.c`). Cheap and weathered
(the user's rule), but rusted riveted steel and copper sheet lead; flake board and planks only as minor parts; no
one set across most of the vendor.

| Surface | Set (to fetch or reuse) | Where |
|---|---|---|
| Rusted copper-brown sheet | rusted steel plate, tinted | the hood's panels, side walls |
| Riveted plate | riveted / painted metal plate | counter front, power panel |
| Worn stainless | brushed stainless (exists), scratched | counter top, bench, bain-marie, shelves |
| Tarnished copper | copper | stock pots, the gas line |
| Cast iron | cast iron / rough black metal | grates, the wok burner |
| Chipped painted metal | painted metal, chipped | posts, stools' bases, the extractor |
| Small tiles | white tiles, dirty grout | splashback |
| Worn rubber | rubber | stool seats, cable sheaths |
| Grimy enamel | enamel / ceramic | bowls, the rice cooker |
| Glass | (shader) | gauge faces, jars, the hatch |
| Wear | the grime-streak decals (`grime_streaks_a/b`), a curvature term from the kit | rust runs from rivets and seams, grease and soot round the range and up the hood, edge wear, wet streaks under the lip |

Checks (NB3): the share of the vendor's area each set covers (none over 40%); every part has a set.

## Lights

| Light | Kind | Notes |
|---|---|---|
| Hood strips | 1 to 3 T8 tubes, cold white (5000 to 6500 K) | the counter and the stools; ray-traced shadows |
| Kitchen | 1 to 2 warm lamps (2700 to 3000 K) under the extractor and the shelves | |
| Burners | small blue points, flickering | |
| Icon signs | red and cyan, lit boxes or tube neon | air lights in the rain's mist |
| By day | the tubes on, the signs dimmer than the sky | the kitchen reads as a dark interior with warm light |

## Budgets

| What | Budget | Why |
|---|---|---|
| Unique structure (hood, walls, counter: merged, a near vendor layer) | 6,000 triangles near (0.7 MB of the 504 MB non-indexed pool at 40 B a vertex) | a lane of 44 vendors at most +31 MB; the pool is 84% full down a lane already |
| Instanced kit (modules: stools, pots, grates, rivet rows, ribs, utensils, jars) | 150 instances a vendor; the module library 300,000 triangles over its levels, indexed (under 20 MB), uploaded once | detail without the pool. NB1 measured: the library 139,000 triangles over its levels; a slice's kit about 62 instances, but the hood alone sets down ~190 single rivets (curved laps take no straight rows): NB2 decides whether rivets are instances or a per-vendor near mesh |
| Levels | L0 to 8 m, L1 to 30 m, L2 to 120 m; beyond, the far shell | screen size picks within them (NB2) |
| Far shell (base layer, to the 1.9 km view distance) | 500 triangles | casts the city's shadows, shelters the rain |
| Triangles drawn, one slice at 3 m | 60,000 at most (structure + L0 instances): **to revise in NB2**. NB1 measured a plausible kit at 137,000 at L0 (57,000 at L1, 15,000 at L2) and the hood 3,000 + 11,000 in rivets; support loops (for the baked occlusion and wear) are a third of L0 | the office impostors' near tier draws far more; the gate is the frame cost below, measured on NB2's spike |
| Frame cost | +0.3 ms at the worst view at most (a row of six slices; the lane at night), shadows included | |
| Generation | +3 ms a chunk for six slices at most | streaming stays smooth |
| Collision | the counter, the walls, the posts, the stools' bases, the A-frame | outlines as `vendor_solids` |

## Captures and the comparison board

`city_test --slice` prints the views (`view NAME --cam X Z YAW PITCH EYE_Y`): the slice from in front at 3, 8, 25 and
80 m (turned off its front if the line of sight needs it, the eye kept off the buildings' plots and arcades; the far
one from 3 m up and as near as 50 m if no 80 m line is clear: at the slice it is 75 m, 75 degrees off, down the
street), and an office impostor tower's curtain
wall at the same distances, level at its fourth floor (the impostor work's capture spot, `--impostors 1`).

```
python tools/slice_capture.py LABEL [--only night|dusk|noon] [--views slice|office|all]
python tools/slice_board.py LABEL
```

take every view in three lights (night in rain at 18:50, dusk at 17:05, noon; the city keeps Los Angeles' time) into
`build/slice/LABEL/`, then lay one board per light: the slice's four distances, the office's four below, the
references below. Each NB step captures under its own label (`nb0` is the old canopy bar, the baseline).

## The judgement (NB8, and at each step)

- **Silhouette at 25 and 80 m:** the barrel hood with its ribs and lip, the fascia's icons, the stools' row; not a box.
- **Depth at 8 m:** the kitchen reads as a real space behind the counter (the range, pots, shelves, the extractor), lit.
- **Detail at 3 m:** rivets, laps, grates, gauges, handles, labels as geometry or proper material, not paint on a flat.
- **Surfaces:** rust, grease and wet as the references; nothing looks new, nothing cartoon-clean; no texture repeats
  across most of it.
- **Beside the office impostors** at the same distance: as dense, as lit, as believable.
- **No people** (the user's rule, until people are a project of their own).
