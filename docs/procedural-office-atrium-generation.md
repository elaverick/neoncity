# Procedural Generation of Office-Tower Atriums and Lower Floors

**Status:** Proposed high-level design. Takes its building footprints from the rule-based building generator.

## 1. Purpose

This document describes an algorithm for generating plausible ground-floor and lower-floor interiors for large office buildings in a real-time 3D city simulation.

The initial target is the public-facing base of an office tower: an entrance, lobby or atrium, reception, access control, lifts, stairs, and supporting spaces. The result must fit the building that already exists, look architecturally coherent from the street, and support closer inspection where the simulation requires it.

The proposal is deliberately narrower than a general floor-plan generator. It produces convincing results from a controlled set of rules, and leaves the underlying system open to future grammars for shops, department stores, restaurants, residential entrances, transport buildings, and other building types.

## 2. Core idea

Treat interior generation as three related but separate problems:

1. **Meaning:** decide what kinds of spaces the building needs and how they relate.
2. **Layout:** fit those spaces into the footprint while respecting dimensions, access, and circulation.
3. **Realisation:** turn the accepted layout into walls, floors, doors, furniture, lighting, and other renderable assets.

A **spatial grammar** describes the kinds of spaces that may be created and the rules for combining them. **Subdivision** divides the available floor area into regions for those spaces. **Validation** checks the arrangement before geometry is built.

These parts stay separate. The grammar does not know how a lift mesh is built, and the geometry builder does not decide whether a reception desk belongs in an office lobby.

The layout stage works on a **planning grid**, not on polygons (section 5). This one decision makes overlap, connectivity, clearance and leftover-space tests simple on any footprint shape, and it is the main reason the approach is quick to implement.

## 3. Scope and assumptions

The first grammar covers the public and service areas of an office building's ground floor:

- a street entrance and optional entrance vestibule;
- a principal lobby, optionally a double-height lobby or atrium;
- reception, waiting and optional lobby café areas;
- security gates across the route to the lifts;
- one or more lift and stair cores with their lift halls;
- back-of-house rooms: toilets, security, mail, facilities, cleaners, plant, bins, loading, meeting rooms, showers, cycle storage, escape stairs;
- atrium voids with galleries or podium floors above.

It does not generate the office floors above. Those use a cheaper representation. Atrium voids are modelled through upper floors only as slab edges, balustrades and a glazed roof.

The generator must cope with everything the building generator can produce, not only rectangles. In practice this means:

- orthogonal footprints (rectangle, L, T, U, cross), chamfered corners and faceted round drums;
- courtyards cut through the footprint;
- a ground floor split into several sections, such as two piers under a bridging block;
- more than one tower above the base, each needing its own core;
- ground floors of normal or double height;
- podium or projecting floors directly above the ground floor.

## 4. Inputs and outputs

### 4.1 Input contract

The generator consumes the building generator's output. Every input has an explicit default, recorded in the layout so that later stages never guess differently.

| Input | Used for | Default when missing |
|---|---|---|
| Ground-floor footprint polygon(s), with courtyard holes | Usable area, envelope | Required |
| Openings on each ground section: position, width, outward direction, kind (primary, service, loading) | Planning frame, entrance, vestibule, rooms anchored behind service doors | Synthesise a service door on the most street-facing edge |
| Tower footprints above the base | Core position (the lift core sits under its tower) | Seeded rule: rear-centre, rear-side or centre |
| Structural cores from the building graph | Core position if under a tower, otherwise a fixed solid obstruction | None |
| Floors served by each core | Lift count | Total building floors |
| Ground storey height | Atrium mode (double height is possible only on a tall ground floor) | Normal storey |
| Section directly above the ground floor (footprint, floor count) | Which atrium levels are possible; the void must fit inside it | No podium atrium |
| Column spacing | Structural grid | Seeded spacing |
| Building seed, section id, interior variant | Reproducible variation | Required |

### 4.2 Outputs

The output is a **semantic layout**, not a mesh. It contains:

- the planning frame and grid;
- named regions, each a set of cells with a type, label and enclosure;
- zones (reception, waiting, café) as rectangles inside open regions;
- doors and gates as sets of cell edges, with their kind;
- the main route from the entrance to each lift hall;
- fixtures with a footprint, an orientation and the region they belong to;
- the atrium description: mode, void, levels, height;
- the planned and realised connection graph;
- a decision log of every placement, simplification and rejection.

## 5. Planning representation

### 5.1 Planning frame

Turn the building's own axes in 90° steps so that the primary entrance faces "front" (the low edge of the plan). Snap a diagonal entrance edge to the nearest axis. Because the turn is always a multiple of 90°, the frame stays aligned with the building's axes. "Front", "rear", "left" and "right" then have a fixed meaning for every rule that follows.

### 5.2 Planning grid

Cover the footprint's bounding box with square cells of half a metre. Each cell records three things:

- **occupancy:** outside, usable, or courtyard;
- **owner:** the region that has claimed it, or none;
- **flags:** route reserve, atrium void, door clearance, fixture, column, lift approach.

Regions are sets of cells. The grammar usually claims rectangles, but the footprint can clip them, so a room against a curved facade keeps a straight inner wall and a curved outer one. The true polygon is used only at realisation, to clip floor finishes and to extend partitions to the facade.

Most tests are rectangle queries, such as "is this rectangle entirely free?". Answer them in constant time with prefix sums over the relevant flag. Use distance fields for clearance tests and flood fill for connectivity tests.

### 5.3 Kinds of region

- **Open regions:** lobby, lift hall, secure side, vestibule. People move freely within them.
- **Enclosed rooms:** walled or glazed. They are entered only through doors.
- **Solid parts:** lift shafts, stairs, risers, structural cores. They are drawn as solid masses with doors on their faces.
- **Zones:** reception, waiting and café areas inside the lobby. They mark function but own no cells, so they never become walls.

## 6. The office-lobby grammar

```text
OfficeBase
├── PublicEntrance            required
│   └── Vestibule             optional, glazed
├── MainLobby                 required, open; owns all unclaimed usable cells
│   ├── Reception zone        optional
│   ├── Waiting zone          optional
│   ├── Café zone             optional
│   └── AtriumVoid            optional, vertical attribute of lobby cells
├── AccessControl             optional: secure side + barrier + gates
├── Core (one per tower)      required
│   ├── LiftHall              open
│   ├── Lifts                 solid, doors onto the hall
│   ├── Stairs                solid, doors onto the hall
│   └── Risers                solid
├── EscapeStair               optional, enclosed, fire exit to the street
└── BackOfHouse rooms         optional, enclosed (section 6.3)
```

This is a list of requirements, not a tree of non-overlapping rooms. The lobby is whatever remains after everything else is placed. Zones sit inside it. An atrium void is a flag on lobby cells, not a region.

### 6.1 Rules express intent and constraints

A rule specifies which region or feature it creates and whether that is required or optional. It also gives preferred and minimum dimensions, preferred neighbours and position (near the core, near the entrance, near a service door, at the rear) and enclosure (open, walled, glazed, solid). Finally, it states the conditions under which the rule may be omitted. Rules never dictate fixed coordinates.

### 6.2 Required and optional features

The required minimum is an entrance, a lobby, at least one core with a reachable lift hall, and a route between them that is clear and wide enough. Everything else is optional, included by floor area and seeded chance (Appendix A), and dropped in a fixed order when a layout fails (section 7, step 7). A small footprint produces a compact, valid lobby rather than a failure.

### 6.3 Room catalogue

| Room | Width along its band | Minimum size | Placement preference | Enclosure | Included when |
|---|---|---|---|---|---|
| Toilets | 5 to 8 m; split into A and B (plus an accessible WC) when long enough | 15 m², 2.6 m | Near a lift hall | Walled | Most floors |
| Cleaners' store | 2 to 3 m | 4 m², 1.8 m | Near a lift hall | Walled | Medium floors |
| Security office | 3.5 to 5 m | 10 m², 2.6 m | Near the entrance | Walled | Gated or large floors |
| Mail room | 3 to 5 m | 9 m², 2.4 m | Near the entrance | Walled | Medium floors |
| Facilities office | 3.5 to 5.5 m | 11 m², 2.6 m | Towards the rear | Walled | Large floors |
| Plant room | 5 to 8 m | 18 m², 3 m | Towards the rear | Walled | Large floors |
| Meeting room | 4 to 6 m | 13 m², 3 m | Near the entrance | Glazed | Large floors |
| Showers and changing | 5 to 8 m | 18 m², 3 m | Near a lift hall | Walled | Large floors |
| Cycle store | 5 to 9 m | 20 m², 3.5 m | Towards the rear | Walled | Large floors |
| Bin store, loading bay | 3.5 to 9 m | 9 to 26 m² | Must contain its service door | Walled | One per service or loading door |
| Escape stair | Fixed 6 × 3 m | — | Far from the core and entrance | Walled, fire exit | Large floors whose core has fewer than two stairs |

### 6.4 Boundary types

The difference between a functional boundary and a physical enclosure is made concrete by one table. Realisation reads it, and so does validation.

| Between | Becomes | Passable |
|---|---|---|
| Two cells of the same region | Nothing | Yes |
| Lobby and lift hall (not gated) | Open | Yes |
| Lobby and secure side, or lobby and a gated lift hall | Low glass barrier | Only through gates |
| Secure side and lift hall | Open | Yes |
| Vestibule and anything | Full-height glass | Only through doors |
| Room and open region | Wall, or glass for glazed rooms | Only through doors |
| Room and room | Wall | No |
| Anything and a solid part | The solid part's own face | Only through lift or stair doors |
| Anything and outside | Building envelope, from the true polygon | Only through openings |

## 7. Layout algorithm

The process is deterministic and bounded, and the **order is part of the design**. Each step only claims cells that earlier steps left free, so the things that matter most (structure, cores and circulation) are fixed before anything decorative.

### Step 1: Prepare

Build the frame and grid. Locate the entrance cell, the first usable cell behind the primary door. Measure the usable area, which drives most later choices.

### Step 2: Choose parameters

Draw every once-per-layout choice up front from the layout's seed:

- vestibule or not;
- reception, waiting and café, and the reception position (central or side);
- gated or open lift halls, and the number of gate lanes;
- compact or expansive lobby, which sets the back-of-house band depth;
- atrium mode;
- the default core rule;
- column grid and its spacing;
- the order in which to use the rear, left and right bands;
- the room list;
- lift count, bank type and stair count for each core.

### Step 3: Place anchors, in this order

1. **Structural grid.** Place columns at a regular spacing, inset from the facade and courtyard. Later anchors may absorb columns (into core walls or lift halls, or within a void), and each absorption is logged.
2. **Fixed obstructions.** Claim structural cores from the building graph that do not sit under a tower as solid cells.
3. **Cores.** Build each core from a template: lifts in one bank or two facing banks, stairs at the ends or the closed end, risers behind, and a lift hall on the door side. Try every position and all four orientations. Score each candidate by:
   - distance from the tower centre, or from the default rule's position;
   - how much of the core falls outside the tower footprint;
   - whether the hall mouth faces the entrance;
   - free space in front of the hall mouth;
   - how much of the core intrudes into the frontage;
   - how close the hall is to the entrance.

   Accept the best-scoring candidate whose hall is reachable from the entrance through a corridor at least 1.5 m wide. If none fits, reduce the lift count by one and retry. With two towers, place one core per tower, the second after the first.
4. **Vestibule.** Claim a glazed draught lobby directly behind the primary door with an inner door opposite. Omit it if it does not fit cleanly.
5. **Atrium void.** Find the largest free rectangle inside both the ground footprint and the floor above. Crop it to a target share of the floor, keeping its front edge and centring it on the entrance. Rooms may not claim void cells. A glazed courtyard simply becomes the void.
6. **Main route.** Find a path from the entrance (or the vestibule's inner door) to each lift hall. Movement cost rises steeply where clearance is low. Reserve a corridor about 2.5 m wide around the path, so that rooms and fixtures cannot take it later.
7. **Access control.** Claim a strip in front of each hall mouth as the secure side. Every boundary between the lobby and the secure side or hall becomes a barrier. Put the gate line where the route crosses that barrier, sized from the floors served. If the barrier line is too short for two lanes, drop access control and log it.

### Step 4: Connectivity plan

Before subdivision, record the required connections as a graph:

- entrance to vestibule, and vestibule to lobby;
- lobby to each lift hall, open or through gates;
- each hall to its lifts and stairs;
- lobby to each planned room, by a door;
- each service door to its room.

The validator checks that every edge is realised as a passable boundary. The final graph is reported as built.

### Step 5: Subdivide the back of house by bands

Rooms are cut from **bands** along the rear and side facades:

1. Place rooms anchored to service doors first; each must contain its door. Then the escape stair, then the rest of the room list.
2. For each room, try every position along each band at its preferred, minimum and maximum width. A candidate's depth is the band depth, measured from the facade, with a straight inner wall.
3. Reject candidates that:
   - touch the route reserve, the void, a lift approach or a door clearance;
   - reach into the frontage keep-clear depth;
   - follow a facade that varies too much across the room;
   - fail the minimum area, minimum dimension or compactness tests;
   - have no straight run of at least a door's width on their inner face.
4. Score the rest by the room's placement preference. Add a bonus for abutting rooms, cores or the facade at the ends. Add a penalty for leaving an alcove narrower than about 3 m. Add a little seeded jitter.
5. After placing a room, reserve a strip in front of its inner face as door clearance, so that a later room in another band cannot wall it off.
6. Split long toilet blocks along the band into toilets A and B, plus an accessible WC when there is room. This is the "split along one axis" rule applied a second time.

Then clean up:

- **Pockets** of unclaimed cells cut off from the main lobby join the neighbouring room they share most boundary with.
- **Slivers** narrower than 1.5 m that touch a room join that room. Find them with a morphological opening. Run the pocket pass again afterwards, in case absorbing a sliver cut something off.

Finally, the **lobby** claims every remaining usable cell.

### Step 6: Doors, then fixtures

**Doors.** Give each room a door on its longest straight boundary with walkable space, preferring the lobby to a lift hall or secure side. Size it by room type and mark clearance on both sides. A room with no possible door fails the layout, unless it is anchored to an exterior door. Give the escape stair a fire exit through the nearest facade.

**Fixtures.** Place them in priority order. Each has a footprint, an orientation, the region it must stay inside, and clearances. None may sit on a route reserve, door clearance, lift approach, column or another fixture.

1. **Reception:** a desk with a staff zone behind it and a clear area in front. It must face the entrance, sit a moderate distance from it and a little off the route, and ideally back onto a wall. The central or side parameter shifts the scoring.
2. **Waiting:** a pair of sofas and a table, near the frontage, at a comfortable distance from reception.
3. **Café:** a counter and tables, kept apart from reception.
4. **Decoration:** planters along the frontage and at the ends of lift halls, a feature piece in the void, and a feature stair to the gallery if there is one.
5. **Room furniture:** items along the wall opposite the door, by room type, such as cubicles, shelving, plant units, bins, pallets, a meeting table or cycle racks.

### Step 7: Validate

The checks, and how each is measured:

- **Required regions exist:** a lobby, and at least one core with lifts and a stair.
- **Reachability:** flood fill from the entrance over walkable cells, crossing boundaries only where section 6.4 allows. Every lift door, stair door, lift hall, vestibule and room with a door must be reached.
- **No improper overlap:** every fixture lies inside its own region and on no column. No two fixtures share a cell, except those placed as one group.
- **Clear approaches:** no fixture or column in a lift approach or on a door's outside cell.
- **Route width:** using a clearance field that counts fixtures as obstacles, the main route keeps at least 1.5 m clear width. The first 3 m inside the entrance and the last 2 m before a hall or gate line are excluded, because those are openings and their widths are checked as doors.
- **Void preserved:** no enclosed room or solid part lies under the void.

Retries follow a fixed ladder of six attempts. Each attempt draws fresh parameters from its own random stream.

| Attempts | Change |
|---|---|
| 1 to 3 | Full grammar |
| 4 | Drop optional rooms except toilets; drop waiting and café |
| 5 | Also drop gates, vestibule, atrium, escape stair and column grid |
| 6 | Known-good fallback: at most two lifts in one bank, one stair, no rooms or zones |

If all six fail, leave an open floor plate and log the reason. Only the successful attempt's log entries are kept; each failed attempt leaves one rejection line.

### Step 8: Realise

Realisation reads the layout and the boundary table:

- **Floor:** the true footprint polygon as a slab, with region finishes laid over it as row runs clipped to the polygon.
- **Partitions:** collect boundary edges by type, merge them into straight runs, and extend runs that end against the facade until they meet the envelope polygon. Doors leave gaps, with lintels. Gates become turnstile cabinets.
- **Envelope:** the true polygon, glazed with mullions at the building's bay width, with gaps at openings and fire exits.
- **Core:** lift shafts as solids with metal doors on the hall face, stairs as enclosures with two flights inside, risers and obstructions as solids.
- **Atrium:** at each level, the footprint of the floor above with the void cut out, a glass balustrade round the void, and a glazed roof at the top.
- **View modes:** a cutaway (walls stop at about 2.4 m), a plan cut (wall footprints only) and full height. Cutaway is the default for inspection.
- **Labels:** each region's label sits at the point furthest from its edges, with its area. Zones get dashed outlines.

Use reusable meshes and instancing. Support lower detail for distant interiors.

### Step 9: Cache and reproduce

Use **named random streams**: one per decision family (parameters, rooms, doors, reception, waiting, café, feature stair), each seeded by the layout seed, the stream name and the attempt number. Adding a decision in one family then does not reshuffle the others. The layout seed combines the building seed, the ground section id and the interior variant. The same inputs always reproduce the same layout, and changing only the variant gives a new interior for the same building.

## 8. Geometry, circulation, and variation

Plan circulation before rooms. The route is a structural part of the design, and rooms are fitted around it.

Variation comes from meaningful choices, always constrained by footprint, entrance, core position and building type:

- compact or expansive lobby, which changes band depth and room count;
- vestibule or none;
- central or side reception;
- open or gated lift halls, and the number of lanes;
- single or facing lift banks, and how many lifts;
- core position when there is no tower to fix it;
- which facades carry back-of-house rooms, and in what order;
- the room mix;
- atrium mode: single storey, double height, gallery, atrium through podium floors, or glazed courtyard;
- column grid spacing;
- finishes and asset choices.

Randomness picks among valid options. It never substitutes for the rules.

## 9. Multiple sections and multiple cores

- **Several ground sections**, such as two piers, are planned independently, each with its own frame. The section holding the primary entrance gets the full grammar. The others get a service grammar: one lift, one stair and a plant room, entered by their own service door or a synthesised one.
- **Several towers** give several cores. Each core gets its own route, and its own secure side and gate line when gated. A route may pass only through its own lift hall.

## 10. Extending the system to other building types

The office grammar is one module within a broader interior framework. The shared system provides:

- the frame and planning grid;
- prefix-sum and distance-field tests;
- template placement with scoring and a reachability check;
- route planning with reserves;
- band subdivision and leftover absorption;
- door and fixture placement;
- the boundary table mechanism, validation, the retry ladder and realisation.

Each building-type grammar supplies its own anchors, room catalogue, boundary table, fixture list and parameters. For example:

- **Shop:** display frontage instead of a vestibule, sales floor as the open region, checkout as a zone, stockroom anchored to the service door.
- **Department store:** escalator cores as anchors, departments as zones, several public levels.
- **Restaurant or café:** seating as the open region, the kitchen anchored to the service door, a counter zone.
- **Residential entrance:** a smaller lobby, mail and concierge zones, fob gates instead of speed gates.
- **Arcology:** a district rather than a lobby, with concourses, market blocks, a multi-tier canyon and stratified vertical access. Section 11 defines it in full.

A useful boundary remains: **the shared system provides spatial planning tools; each grammar defines meaning and requirements.**

## 11. Arcology grammar

An arcology is the generator's megastructure: any building on an oversized "mega" plot becomes one. Its exterior is already defined by the building generator:

- a single footprint covering the whole site, rectangular or with chamfered corners;
- 6 to 9 stacked tiers, each set back 4 to 8 m from the one below, stopping once the next tier would be narrower than 14 m;
- a 30 to 60 m spire on the top tier;
- the finer window grid, so the facade reads as one dense, continuous city-in-a-building rather than a cluster of towers;
- a double-height ground floor.

The arcology grammar plans the interior that this shell implies. It reuses the shared system from section 10: the planning frame and grid, anchors placed in order, a route reserve, band subdivision, the boundary table, the retry ladder and realisation. What changes is the meaning. An office lobby is a room. An arcology's ground floor is a district, and its interior is a city stacked vertically.

### 11.1 Design intent

The visual reference is the neo-noir megastructure: Syd Mead's stepped corporate pyramids and the street level of *Blade Runner*. The look is written as rules, so that it comes out of the layout rather than being painted on afterwards.

- **A city inside a building.** The ground floor is streets, not a lobby. Public routes carry the city's street grid through the building from facade to facade, lined with stalls and small shop buildings that stand inside the megastructure.
- **Vertical stratification.** Altitude means status. The lower tiers are crowded, retrofitted, wet and loud with signage. The upper tiers are sparse, monumental, warm and quiet. One value, the **stratum height** *t* (0 at the ground, 1 at the top tier), drives density, clutter, palette and signage throughout the building.
- **Monumental structure.** The structure is oversized and visible. Each tier's setback line comes down through the building as a ring of piers, so the stepped exterior repeats inside as nested colonnades. Stepped, ziggurat-like motifs echo the tiers in capitals, ceilings and screens.
- **The retrofit layer.** Services look added later: ducts, cable trays, pipes, vents, kiosks and signage bolted to the original structure. This layer is applied over the clean architecture, heavily in the public lower strata and not at all in the corporate strata.
- **Low-key light.** A few strong sources and deep darkness between them. Neon signage, sodium street lamps, cold shafts from the top of the canyon, slow searchlight sweeps, and slatted light through the fine window grid. Haze is always present, because it is what makes light visible.
- **Wet surfaces.** Public floors are glossy. Water drips under the light crown and pools where it lands.

### 11.2 Scope

The grammar plans three things in detail:

- the **concourse level** (the double-height ground floor);
- the **canyon**, a multi-tier atrium void, with its galleries and bridges, through the tiers it rises through;
- a **sky lobby and terrace** at the base of each upper tier.

All other floors use the cheap upper-floor representation, such as interior mapping keyed to the fine window grid. The spire is not planned.

### 11.3 Inputs

| Input | Used for | Default when missing |
|---|---|---|
| Tier stack: for each tier, its footprint (rectangle or chamfered), base elevation and floor count | Every anchor that depends on altitude | Required |
| Ground storey height | Concourse clear height | Always double height |
| Primary opening on the ground floor | Planning frame; main portal | Centre of the most street-facing edge |
| Street grid around the plot | Where portals and concourses line up with streets outside | Concourses on the column grid, centred on the canyon |
| Fine window grid pitch | Unit fronts on the canyon walls; slatted-light pattern | Building default |
| Seed, variant | Variation | Required |

The tiers usually share a centre, so the top tier fixes the building's vertical axis. The rules below do not assume it.

### 11.4 Grammar

```text
Arcology
├── ConcourseLevel                 required, double height
│   ├── Portals                    required on at least two opposite facades
│   ├── Concourses                 required, open, continuous portal to portal
│   │   └── Plaza zones            at crossings and in front of the express core
│   ├── CanyonFloor                required, open plaza under the void
│   ├── ExpressHall                required, gated for upper strata
│   ├── MarketBlocks               between concourses
│   │   ├── StallRows              along every edge facing a concourse
│   │   ├── LocalCoreHalls         where a local core lands
│   │   ├── EscapeStairs           added until the travel limit is met
│   │   └── BlockInterior          one use per block (section 11.6)
│   └── ServiceYard                anchored to the service portal
├── Canyon                         required
│   ├── Void                       through the tiers it fits
│   ├── Galleries                  one per floor the void passes
│   ├── Bridges                    within the span limit
│   └── LightCrown                 glazed cap, transfer structure
├── SpineCore                      required: express lifts to every tier
├── LocalCores                     each rises until its tier no longer contains it
├── TierDecks (one per tier ≥ 1)
│   ├── SkyLobby                   express stop, gated by stratum
│   └── Terrace                    setback roof of the tier below
└── Spire                          not planned
```

### 11.5 Strata

The tier stack is divided into strata by stratum height. The stratum sets the uses, access rules and style of every space in it.

| Stratum | Typical tiers | Uses | Access | Light | Signage | Retrofit | Materials |
|---|---|---|---|---|---|---|---|
| Street | Ground and the first tier | Market, food, repair, clinics, arcades, transit | Public | Sodium and neon, dense pools, heavy haze | Dense: every stall, column and fascia | Heavy | Wet concrete, steel, tile, grime |
| Habitat | Lower-middle tiers | Housing, small offices, services | Residents through the local core gate | Mixed, cooler, sparser | Moderate, on gallery fascias | Moderate | Concrete, painted steel |
| Commerce | Upper-middle tiers | Offices, hotels, clubs | Checkpoint at the sky lobby | Warm, controlled | Sparse, large screens only | Light | Glass, bronze, stone |
| Corporate | Top tier or two | Executive, boardrooms, observation | Express lift only, escorted | Warm, low, slatted sunset light | None | None | Polished stone, gold, timber |

### 11.6 Layout algorithm

The steps follow section 7, with these differences.

**Step 1: Prepare.** Read the tier stack. Build one planning frame for the whole building from the main portal, and one planning grid per deck: the concourse level, each gallery level and each sky lobby. Compute each tier's stratum height and stratum.

**Step 2: Choose parameters.** Draw these once:

- **Canyon mode:**
  - *spine*: a void ringing the spine core, which stands free in it;
  - *twin nave*: two voids along the long axis, one either side of the spine core;
  - *well*: one void in front of the spine core, on the main portal side.
- **Concourse pattern:**
  - *cross*: one concourse on each axis, crossing at the canyon;
  - *grid*: a cross plus parallel concourses;
  - *boulevard and ribs*: a double-width main concourse with a central colonnade, crossed by narrow ribs.
- **Other choices:** column spacing, gallery depth, the maximum number of tiers the canyon may rise through, stratum boundaries, signage and retrofit intensity, and the neon palette.

**Step 3: Place anchors, in this order.**

1. **Megastructure grid.** Lay a coarse column grid. Then project each upper tier's outline down onto every deck below as a ring of piers at the same spacing. This is the rule that brings the stepped exterior inside. Where a ring crosses a concourse, span the concourse with a transfer beam instead of a pier. Where a ring falls inside the canyon void, carry it on the light crown's transfer structure. Remove grid columns that are inside the void or too close to a ring pier.
2. **Spine core.** Place the express core at the vertical axis, inside the top tier with a margin. It serves every tier. Size its lift count from the number of tiers, since express lifts stop only at sky lobbies.
3. **Canyon.** Build the void for the chosen mode around or beside the spine core. Raise it tier by tier while it still fits inside that tier's footprint, inset by the gallery depth plus a margin. Stop at the last tier where it fits, or at the parameter limit, and cap it there with the light crown. If it does not fit the first upper tier, shrink the void and try again. If it still does not fit, the canyon becomes a double-height plaza and the decision is logged.
4. **Portals and concourses.** Concourses run **between column lines**, so the columns line the street edges instead of standing in the street. Each concourse runs from facade to facade and merges with the canyon floor where it crosses it. A portal opens wherever a concourse meets the facade. The main portal is on the primary opening. Add a service portal and service yard at the rear.
5. **Local cores.** Fill the floor with local cores, spread out by farthest-point sampling, inside blocks and clear of concourses, the canyon floor and the spine core. Their number comes from the floor area. **Each core rises until the next tier's footprint no longer contains it.** Its top tier is where it stops, and that tier's sky lobby is where people transfer to the express core. This produces the stepped arrangement of core heights that matches the stepped exterior.
6. **Express hall and checkpoints.** Place the express hall on the canyon floor at the spine core face nearest the main portal, with gates whose rule is set by the strata: public for the street stratum, residents for habitat, checkpoint for commerce, escort only for corporate.

**Step 4: Connectivity plan.** The vertical graph joins:

- portals to concourses, and concourses to the canyon floor;
- the canyon floor to the express hall;
- the express core to every sky lobby;
- each sky lobby to its local cores and terrace;
- local cores to the floors they serve;
- galleries to bridges, and bridges to the spine core.

Every tier must be reachable from the main portal.

**Step 5: Subdivide.**

1. **Market blocks** are the rectangles left between concourses, the canyon floor and the facade, clipped to the footprint. Blocks too narrow for a stall row plus a service passage become kiosk strips.
2. **Escape stairs** come first. Compute walking distance over the concourses and canyon floor from every stair door, core door and portal. Wherever it exceeds the travel limit, claim an escape stair at the edge of the nearest block, and repeat until the limit holds everywhere.
3. **Stall rows** come next. Every block edge that faces a concourse or the canyon floor is cut into stall units at the band depth, the "split along one axis" rule applied to frontage. A service passage runs behind the stalls.
4. **Block interiors** get one use each, drawn by size and stratum: night market, noodle hall, arcade, clinic, capsule hotel, repair shops, bathhouse, exchange, housing entrance (wherever a local core lands), or service yard. Blocks are low buildings within the building, about two-thirds of the clear height, with plant and clutter on their roofs. Ducts run in the space above.
5. **Galleries.** At each floor the canyon passes, a gallery ring runs round the void. Unit fronts line its back wall at the fine window grid pitch: shopfronts in the street stratum, flat doors in habitat, glass in commerce.

**Step 6: Fixtures and dressing.** Functional fixtures come first: gates, stall counters, kiosks, benches. Then three style layers, each driven by stratum height and none allowed into a concourse's clear width or below 3 m clear height over it.

- **Signage.** Signs go on fixed surfaces:
  - stall fascias, facing the concourse;
  - blade signs projecting from column faces;
  - banners on gallery fascias, facing the canyon;
  - giant screens on the spine core faces, facing the canyon.

  Density falls with altitude. Glyph content is procedural and never imitates real brands.
- **Retrofit.** Ducts follow concourse centrelines at ceiling height. Cable trays follow colonnades. Vents along stall backs emit steam. All of it fades out with altitude.
- **Light.** Sodium lamps at regular spacing along concourses; neon from signage; cold shafts down from the light crown; one or two slow searchlights sweeping the canyon; slatted window light on gallery floors near the facade. Keep a small fixed budget of real lights and let emissive surfaces and bloom carry the rest. Place drip zones under the light crown, with puddles where they land.

**Step 7: Validate.** In addition to section 7, step 7:

- every pair of opposite portals is connected by concourse;
- every stall fronts walkable space;
- every tier is reachable from the main portal through the vertical graph;
- every core lies inside every tier it passes through;
- the void lies inside every tier it passes through, inset by the gallery depth;
- every bridge is within the span limit;
- walking distance to a stair or portal is within the travel limit everywhere;
- signage, ducts and lamps leave the concourses' clear height free;
- real lights stay within budget.

Retries follow the section 7 ladder. On the fourth attempt, drop the grid concourse pattern and twin-nave mode. On the fifth, drop the canyon above the first tier. The sixth falls back to a cross pattern with a spine canyon one tier tall.

**Step 8: Realise.** Realisation follows section 7, step 8, with one useful device: an architectural **section view**. A vertical clipping plane through the canyon shows how the concourses, canyon, galleries, nested colonnades and stepped core heights relate in a single image. It is the arcology's equivalent of the plan render in section 13.

For the level of detail:

- the concourse level and canyon are built as geometry near the camera;
- sky lobbies and terraces are built as geometry within range;
- all other floors are interior-mapped using the fine window grid;
- the giant screens and signage atlas are shared across all arcologies.

**Step 9: Reproduce.** The seed is the building seed plus the variant. Use a separate stream per anchor family (grid, spine core, canyon, concourses, local cores, blocks, stalls, signage, retrofit, light), so the style layers can be retuned without moving the structure.

## 12. Performance and implementation considerations

The problem is bounded by one footprint and a few dozen regions. With a half-metre grid and footprints up to about 56 × 38 m, a whole layout including retries should take well under a second even in an unoptimised implementation, and a cutaway realisation needs a few hundred meshes. Core placement is the costliest step; prefix sums keep it cheap.

Measure layout, mesh construction and resident interior count separately. Keep layout and realisation apart, cache accepted layouts, instance common geometry, and generate detail only where it is visible.

## 13. Testing and debugging

- **Batch runs:** generate hundreds of buildings across every building type and record pass rate, attempts used, atrium modes, gate usage, room counts, worst-case time and grouped rejection reasons. Aim for every building to produce a valid layout, with the large majority passing on the first attempt.
- **Plan renders:** draw layouts top-down as images (regions, walls, doors, gates, fixtures, route, void). This is the fastest way to judge subdivision quality without a 3D engine.
- **Decision log:** use four tags. *info* for placements, *simplify* for relaxed preferences and omissions, *reject* for failed attempts, *pass* for success. Show the log beside the result.
- **Realisation smoke test:** run scene construction against a strict stand-in renderer that rejects non-finite values and degenerate shapes.

## 14. Implementation plan

Build in three milestones.

The first is a narrow vertical slice. A mostly rectangular footprint with one core, an entrance, a lobby, reception, a lift hall, the main route and a few back-of-house rooms. It should produce different valid layouts for different footprints and seeds, and fall back cleanly when optional features do not fit.

The second covers the full range of building-generator output:

- off-centre and twin cores;
- irregular footprints, including round and chamfered;
- courtyards;
- several ground sections;
- double-height lobbies, galleries and podium atria.

The third extends the system:

- several public entrances;
- a mezzanine floor planned as its own level rather than a slab;
- entrance canopies and revolving doors;
- the arcology grammar (section 11), the first grammar that plans more than one deck;
- a second, clearly different grammar (a shop) built on the shared framework to prove the boundary in section 10.

## 15. Success criteria

The algorithm is successful if it:

- generates visually plausible office entrances and atria from building-specific inputs;
- produces controlled variation without losing coherence;
- preserves circulation, access and structural exclusions;
- handles small and awkward footprints gracefully;
- is deterministic, with a safe fallback;
- separates semantic planning from geometry and assets;
- supports new grammars without restructuring the core system;
- keeps generation and rendering costs compatible with a real-time city.

## 16. Summary

A constraint-driven spatial grammar on a planning grid, aligned to the building's own axes and turned to face the entrance. Structure, cores, entrance, void, route and gates are anchored first, in a fixed order. Back-of-house rooms are cut from bands along the facades, and the lobby is whatever remains. A boundary table turns functional boundaries into physical ones. Validation is measured, retries follow a fixed ladder, and a separate realisation stage reads the result. The shared system owns the spatial mechanics; each grammar owns the meaning.

## Appendix A: Starting values

Treat these as configurable starting points, not limits.

| Parameter | Starting value |
|---|---|
| Planning grid cell | 0.5 m |
| Lift shaft (including walls) / lift door | 2.5 × 2.5 m / 1.0 m |
| Stair enclosure | 3 × 5.5 m |
| Lift hall depth (single bank) or width (facing banks) | 3.5 to 4 m |
| Lifts per core | round(floors served ÷ 7) + 1, between 2 and 8 |
| Facing banks | 6 or more lifts; sometimes from 4 |
| Core stairs | 2 when a single bank has 3 or more lifts, otherwise 1 |
| Lift approach kept clear | 1.5 m |
| Preferred distance from entrance to lift hall | 9 m or more |
| Main route: minimum clear width / reserved corridor | 1.5 m / about 2.5 m |
| Route cost penalties | below 2.4 m clear, steeply below 1.6 m |
| Vestibule depth / width | 2.5 to 3.2 m / door width plus 1 m each side |
| Frontage keep-clear (no back-of-house rooms) | 6 to 9 m |
| Back-of-house band depth | 5 to 6.5 m compact, 4.5 to 5.5 m expansive; 7 m for loading |
| Room compactness / allowed facade variation | fill ratio 0.7 or more / no more than 60% of band depth |
| Slivers absorbed | narrower than 1.5 m |
| Secure strip | 3 m deep, hall width plus 1 m each side |
| Gate lanes | round(floors ÷ 10) + 2, between 2 and 6; 0.9 m each plus a 1.2 m wide gate |
| Atrium void | at least 6 × 6 m; 22 to 36% of floor area |
| Gallery level | half the ground storey height |
| Podium atrium | 1 to 3 floors; floors of 480 m² or more |
| Column grid | 7.8 to 9 m spacing when the short side is 22 m or more; at least 2.6 m from the facade |
| Reception desk | 4.5 to 6.5 × 1 m; 1.5 m staff zone; 1.5 m clear in front; 5 to 16 m from the entrance; 2.5 to 7 m off the route |
| Waiting bay / café bay | 4.5 × 3.5 m / 7 × 5.5 m |
| Door widths | 1 m rooms, 1.5 m plant, 2 m loading; 1.5 m clearance each side |
| Escape stair | 6 × 3 m; floors of 650 m² or more whose core has fewer than 2 stairs |
| Cutaway wall height | 2.4 m |

| Feature | Included when (chance) |
|---|---|
| Vestibule | 65% |
| Reception | 200 m² or more (90%) |
| Waiting area | 300 m² or more, with reception (80%) |
| Lobby café | 650 m² or more (50%) |
| Gates | 360 m² or more and 6 or more floors (72%) |
| Toilets | 180 m² or more |
| Cleaners' store | 280 m² or more (80%) |
| Security office | gated or 500 m² or more (70%) |
| Mail room | 420 m² or more (60%) |
| Facilities office | 560 m² or more (55%) |
| Meeting room | 620 m² or more (55%); a second at 950 m² or more (60%) |
| Plant room | 700 m² or more (45%) |
| Showers and changing | 750 m² or more (45%) |
| Cycle store | 800 m² or more (40%) |
| Expansive lobby | keeps the first two optional rooms, each later one at 50% |
| Atrium on a double-height ground floor | gallery 55%, otherwise plain double height |
| Glazed courtyard atrium | 50% when the footprint has a courtyard |

## Appendix B: Arcology starting values

Treat these as configurable starting points, not limits.

| Parameter | Starting value |
|---|---|
| Column grid | 12 to 16 m spacing; columns 1.8 to 2.4 m across with flared capitals |
| Setback piers | One ring per upper tier on every deck below, at grid spacing; at least 0.4 × spacing from any grid column |
| Concourse | One column bay wide (spacing − column width − 1 m); boulevard two bays with a central colonnade |
| Concourse clear height | 5.5 m under ducts and signs (ground storey 7.2 m) |
| Portals | Concourse width; at least one on each of two opposite facades |
| Spine core | 10 to 16 m across; 4 to 8 express lifts; inside the top tier with a 2 m margin |
| Local cores | 8 × 8 m; one per 3,000 m² of ground floor, between 4 and 10; 2 to 4 lifts |
| Core containment margin | 3 m inside every tier the core passes |
| Canyon void margin round the spine | 8 to 14 m (spine mode); nave length 12 to 24 m; well length 14 to 28 m |
| Canyon height | Up to 3 to 5 tiers, while it fits inside the tier inset by gallery depth + 1 m |
| Gallery depth | 3 to 5 m; one per floor (3.6 m) inside the canyon |
| Bridge | 3 m wide; span at most 24 m; at least one per gallery level, staggered between levels |
| Travel limit to a stair or portal | 45 m walking distance |
| Escape stair | 4 × 7 m, at a block edge |
| Market block | At least 7 m in its short direction, otherwise a kiosk strip |
| Block mass height | 4 to 5.5 m, about two-thirds of clear height |
| Stall unit | 3 to 4.5 m wide, 3.5 m deep; 1.5 m service passage behind |
| Sodium lamp spacing | About 12 m along concourses, hung at 5 m |
| Real-light budget | 8 dynamic lights per visible arcology interior |
| Signage density | Street stratum: every stall plus one blade sign per column face on a concourse; falls linearly to zero at the corporate stratum |
| Strata boundaries | Street up to *t* = 0.2; habitat to 0.5; commerce to 0.85; corporate above |
