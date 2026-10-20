# Rule-Based Procedural Generation of Office Buildings and Skyscrapers

**Status:** Proposed high-level design, prototyped in a browser toy

## 1. Purpose

The goal is to generate varied, coherent office blocks and skyscrapers at runtime without hand-authoring every building. A building should be described by rules and parameters, then constructed from a sequence of geometric operations.

The proposed system combines two complementary ideas:

- **A typed building graph** describes the building's parts and how they relate to one another: which sections sit above other sections, what supports them, and where entrances and façade openings belong.
- **A polygon-based geometry system** creates the horizontal footprint of each section. These two-dimensional outlines are then extruded and joined to produce the three-dimensional building.

The graph defines the building's organisation; the polygons define its shape. Keeping those responsibilities separate makes the system easier to validate, extend and debug.

The initial target is exterior geometry for office blocks and skyscrapers, with industrial process plants as a second family (section 11). The system should support regular buildings as the common case, with less common features, such as projecting upper floors, spans and twisting profiles, introduced only when their rules can be satisfied. Individual buildings can then be placed on a grid to form districts whose character is set by biasing the choices, not by changing the rules.

## 2. Design principles

1. **Rules before geometry.** Choose a valid building structure before producing its final mesh.
2. **Constrained variety.** Randomness selects among permitted alternatives; it must not bypass architectural or geometric constraints.
3. **Repeatable results.** The same world seed and building location should produce the same building.
4. **Composable parts.** Building sections, footprints, support arrangements, façade features and roof treatments should be generated and validated as distinct concepts.
5. **Graceful failure.** If a proposed combination is invalid, retry with another permitted choice or simplify the building rather than emitting broken geometry.
6. **Plausibility, not structural certification.** Rules should prevent obviously unsupported or nonsensical forms, but the generator is not a substitute for engineering analysis.
7. **Variety from many small axes.** Silhouette, footprint, profile, façade rhythm, ground floor and roof can each vary independently. Combining several modest variations gives more distinct buildings than a few dramatic ones.

## 3. The building graph

Each generated building is represented as a graph of typed nodes and relationships. The exact storage format is an implementation choice; the graph is the conceptual model used by the rules and validation process.

### Principal node types

- **Building:** overall archetype, seed, site limits and building-wide parameters.
- **Building section:** a vertically bounded portion of the building, with a type, elevation, height and footprint polygon. A footprint may carry a void, such as a courtyard. A section may rest on one supporting section, or on several, as when a block spans two piers or a sky bridge joins two towers.
- **Support element:** an abstract structural element such as a column, raked strut, core, structural wall, pier or transfer structure.
- **Plant unit:** for industrial plants, a process vessel (column, tank, domed vessel, exchanger), a stack or flare, a steel frame or pipe rack, an elevated deck, or a small building. Units are placed as footprint polygons with a height rather than as stacked floors (section 11).
- **Pipe and access element:** a pipe is a route between two ports on units; a ladder or stair is an access element that reaches an elevated level.
- **Façade segment:** an exterior edge or face that can receive façade treatment and, where permitted, openings.
- **Opening or entrance:** a door, loading opening or other opening with a semantic purpose and placement constraints.

### Principal relationships

- `above` / `below`: vertical ordering of sections.
- `supported_by`: a section or projecting part depends on a support element or supported section. A section may have more than one supporter.
- `contains`: a building contains sections; a section may contain support elements, voids or façade features.
- `connects`: a pipe joins a port on one plant unit to a port on another.
- `braced_by`: a tall vessel is tied laterally to a steel frame.
- `reached_by`: an elevated deck or frame level is reached from the ground by a ladder or stair.
- `opens_on`: an entrance or opening belongs to a particular façade segment.
- `opens_into`: an entrance connects the exterior to a lobby, service space or other permitted destination.

A section's footprint is a two-dimensional polygon in the building's horizontal plane. Its elevation and height define the vertical interval over which that footprint is extruded. Adjacent sections need not have identical footprints: their differences create setbacks, wider podiums, crowns, overhangs and gradual changes of profile.

Façade, ground-floor and roof treatments are attributes of sections and façade segments. They influence appearance but do not change the structural graph.

## 4. Four layers of rules

### 4.1 Building grammar

The building grammar defines valid high-level arrangements of sections. It selects the building family and establishes which section types may follow or connect to one another.

Illustrative patterns include:

- `OfficeBlock → GroundFloor + RepeatedOfficeFloors + OptionalSetback + Roof`
- `Skyscraper → Podium + Tower + OptionalCrown`
- `Tower → RepeatedTowerFloors + OptionalSetbacks`, or a tower built from thin slices when it has a non-straight profile
- `Podium → GroundFloor + OptionalUpperPodiumFloors`
- `TwinTowers → Podium + TowerA + TowerB + SkyBridge + OptionalCrowns`
- `ProjectingPodium → GroundFloor + ProjectingBlock + Tower`
- `GatewayBuilding → PierA + PierB + SpanningBlock + Tower`
- `Plant → Layout + Units + Connections + Access + Utilities` (see section 11 for the plant patterns)

These are conceptual patterns, not a fixed syntax. A grammar rule may specify a required component, an optional component, or a bounded repetition. For example, a tower may contain a variable number of repeated floors, while a crown is optional.

**Tower profiles.** A tower may be straight, tapered, waisted, flared or twisted. A non-straight profile is expressed as a short stack of thin sections, each derived from the first by scaling and/or rotating its footprint. The profile is a grammar choice; the support rules in section 7 decide whether it is acceptable.

### 4.2 Footprint grammar

The footprint grammar defines how a section's polygon is created or modified. Begin with a simple footprint, then apply a controlled set of operations, for example:

- create a base polygon from a permitted family: rectangle, chamfered rectangle, L, T, U, cross or a faceted drum approximating a circle or ellipse;
- chamfer or clip corners;
- add a wing by joining another polygon;
- subtract a courtyard or atrium, leaving a ring of floor plate;
- inset or scale a footprint to create a setback, or offset it outward or inward to create façade bands, parapets and arcades;
- rotate an upper footprint relative to the one below, as a whole or by a small increment per slice;
- extend part of a footprint beyond the section below, if the overhang rules allow it.

The system should use robust polygon operations and validate the result after each modification. Footprints must have valid boundaries and must not self-intersect. A subtracted void must lie wholly inside its outer polygon and keep a minimum width of floor plate around it. Rules should also limit extreme slivers, tiny edges and other shapes that are unsuitable for the intended building scale.

**Minimum width.** No section above the ground floor may be thinner than four bays at its narrowest point, measured as the smallest extent of the footprint across any direction (the narrowest calliper width). With bays of 2.5 to 3.6 m that is roughly 10 to 14 m, so a tall tower can never end in a needle only one or two windows wide. The rule applies to every tower slice, setback and crown, not just the base, and it is checked when footprints are generated (so a narrow proposal is rejected and another is tried) and again when the finished building is validated. A taper, waist or setback that would breach it is rejected for that seed, and the generator falls back to a plainer profile if every attempt fails. On a plot too narrow for four bays, the upper floors need only be as wide as the plot leaves room for (its narrow side less 2.5 m), so a narrow shophouse keeps its storeys. A sky bridge, a link between two towers rather than a floor plate, is exempt.

An upper footprint that is placed within a lower one is checked against the lower footprint with conservative sampling along its boundary. This test is the basis for deciding whether a connection is an ordinary stack or needs a support arrangement.

### 4.3 Constraint and validation rules

Constraints define which graph and geometry combinations are acceptable. They include building-wide rules, section-specific rules, polygon validity, access requirements and support relationships.

A connection that is geometrically possible is not automatically a valid building arrangement. For example, an upper floor could be placed above a lower floor, but an unsupported projection should be rejected unless the required support arrangement is present.

### 4.4 Surface and silhouette treatments

These rules vary what a valid building looks like without altering its structure. Each is chosen from a small permitted set, per building or per section.

- **Façade rhythm:** bay width, window style (curtain wall, ribbon, punched), vertical fins or horizontal spandrel bands, and a palette. Bay width is adapted per façade segment so that bays divide the segment evenly.
- **Accent and plant bands:** an optional accent band where a tower meets its base, and louvred plant floors at intervals on tall towers.
- **Ground floor:** a standard glazed lobby, a double-height lobby, or an arcade in which the ground floor is set back behind perimeter columns. An arcade is a modest overhang and is validated as one.
- **Roof and crown:** flat roof with plant rooms, helipad, roof garden, water tanks, solar array, hip roof or glass pyramid. Each is placed only where it fits inside the top footprint, with a flat-roof fallback. A crown may add a spire.

## 5. High-level generation algorithm

### Step 1: Establish the building identity

Obtain a world seed, building position and site or cell constraints. Derive a deterministic building seed from the world seed and location. Use separate derived random streams for major decisions, such as archetype, footprint, floor heights, façade variation and roof treatment, so that a change in one stage does not unnecessarily reshuffle every later choice.

### Step 2: Select a building archetype

Choose an archetype from those permitted at the location. The initial set should be deliberately small:

- **Office block:** a broad footprint and repeated floors, usually with a simple roof and optionally a setback or a courtyard.
- **Skyscraper:** a podium or base, a tower shaft with a straight or modified profile, and optional setbacks or crown.
- **Podium with projection:** a building in which a limited block may project beyond part of its lower footprint, span between two piers, or sit under a flared tower, when an explicit support pattern permits it.
- **Twin towers:** two towers on a shared podium, joined at height by a sky bridge.
- **Industrial plant:** a compact process plant of vessels, stacks, frames, decks and pipes, laid out around a pipe rack or under an elevated deck (section 11).

Archetypes should supply parameter ranges and eligible rules, not a single fixed design. A district (see section 8) supplies the selection weights.

### Step 3: Expand the building graph

Apply the building grammar to create the required section nodes and their vertical relationships. Select floor counts, section heights, ground-floor type, tower profile and optional features within the chosen archetype's limits.

At this point, the system has a structural outline of the building but does not yet need to have generated the final vertices or mesh.

### Step 4: Generate the base footprint

Create a valid starting polygon that fits the site's permitted dimensions. Use a controlled family of shapes rather than completely unconstrained random polygons. If the archetype calls for a courtyard, subtract it now and confirm that it fits.

### Step 5: Generate footprints for the sections

For each section, reuse or transform a previous footprint, or apply permitted polygon operations to create a new one. Examples include:

- repeating a tower footprint across many floors;
- insetting upper floors to create a setback;
- changing the footprint between a podium and tower, including rotating the tower against its podium;
- deriving each slice of a tapered, waisted, flared or twisted tower from the first slice;
- placing two separate tower footprints on a shared podium with a clear gap between them;
- adding or subtracting polygonal regions to create wings or voids;
- proposing an overhang only for section types that allow it.

The generator should prefer simple, coherent changes between neighbouring sections. Each candidate footprint must pass geometric validation before being accepted.

### Step 6: Resolve support relationships

Compare each section with the one or ones beneath it and identify where its footprint extends beyond them. Ordinary stacked or inset sections need no special rule. Any other relationship must be matched to a permitted support arrangement (see section 7), and its bounded parameter must be within the limit for that arrangement.

Reject the candidate if the rules cannot identify a plausible load path to the foundation or if a bound is exceeded. The generator should never treat a floating or unsupported section as an acceptable result merely because its polygon is valid.

### Step 7: Place entrances and façade openings

Derive façade segments from the accepted ground-level polygons, including any inner courtyard faces and the faces of piers. Select suitable segments for doors and other openings, applying the access rules described below. Once openings are placed, generate the surrounding façade geometry and materials.

### Step 8: Validate the completed building

Check graph consistency, polygon validity, section intersections, support requirements, entrance placement and other configured constraints. If the building fails, retry the failed decision with another permitted choice. After a bounded number of attempts, simplify the building, for example by dropping a rare feature or using a simpler footprint, rather than retrying indefinitely.

### Step 9: Generate renderable geometry

Extrude each valid section between its lower and upper elevations. Generate horizontal surfaces where required, including the undersides of overhangs. Connect exposed edges into façades, then apply window patterns, bands, materials, roof treatments and any support elements. Produce collision geometry as a separate output where the game requires it.

The final mesh is a result of the graph and its accepted geometry; it should not become the only record of the building's structure.

## 6. Rules for doors and entrances

Doors should be represented as semantic openings attached to façade segments, not as arbitrary texture details. This allows the generator to check whether a door is in a plausible and useful location before building the final mesh.

Suggested rules:

- A normally occupied office building should have a primary pedestrian entrance at ground level.
- Primary public entrances should be placed on an accessible façade segment that meets the ground-level approach or connects to a generated entrance platform or steps where those are supported by the game.
- Ordinary pedestrian entrances should not appear on arbitrary upper floors. Secondary access may be allowed on designated lower floors when the archetype explicitly supports it.
- Service doors and loading bays may be placed on designated lower-level façades, subject to separate rules.
- An opening must fit within its façade segment and must not conflict with corners, structural supports or other openings. Door width may be reduced to fit short segments, such as the facets of a drum, down to a minimum usable width.
- Where columns stand in front of a façade, such as an arcade or a supported overhang, a door must leave a clear gap in line with the columns.
- An entrance should connect to a plausible destination, such as a lobby or service area, rather than opening into an inaccessible solid section.
- Faces onto a courtyard are eligible for secondary access only, not for the primary entrance.

Entrance selection can be weighted by factors such as street frontage, the building's principal face and available façade width. Randomness chooses among eligible locations; it does not decide whether an ineligible location becomes valid.

The initial implementation need only model access semantically. Detailed door frames, handles and interior layouts can be added later.

## 7. Rules for overhangs, spans and step-outs

Wherever a section extends outside the footprint directly supporting it below, the extension must be explained by a support arrangement. These cases should be uncommon rather than a normal outcome of random footprint variation. The permitted arrangements share one pattern and differ only in the support used and the bound that applies.

| Arrangement | Description | Support | Bounded quantity |
|---|---|---|---|
| Side or corner projection | A block extends past the façade of the section below, along one side or around a corner | Columns, raked struts or a structural core | Projection distance, and the projecting extent along the façade |
| Arcade | An upper floor extends past a set-back ground floor | Perimeter columns | Setback distance |
| Gateway span | A block spans a through-passage between two piers | The piers, with an abstract transfer structure | Clear span |
| Step-out profile | Successive slices of a twisting, waisted or flared tower step outward relative to the slice below | A central core | Outward step per slice |
| Sky bridge | A link between two towers at height | The two towers | Clear span |

An overhang or span rule should require all of the following:

1. **Eligible location:** the building archetype and section type permit the arrangement, most likely at a podium or lower-level transition, or between towers.
2. **Bounded extension:** the projection, step or span stays within limits configured for that arrangement and support type. Limits are stricter for a core-only cantilever than for a row of columns, and for a corner cantilever than for a side cantilever. A projection should also stay below a fraction of the depth that supports it.
3. **Explicit support:** the graph identifies a plausible support system, such as columns, a structural core, structural walls, piers or a transfer structure.
4. **Continuous load path:** the abstract support relationships connect the projecting section to the building's supporting structure and ultimately to the foundation.
5. **Geometric clearance:** the extension does not intersect neighbouring buildings or violate site and access constraints.

Conceptually, an overhang is permitted only when:

`eligible section + bounded extension + valid support arrangement + clear geometry = accepted overhang`

If any condition fails, reject the proposal and use an ordinary footprint, setback or another valid alternative. Use a low selection weight for these rules so that they remain visually distinctive. Proposals are allowed to exceed the limits occasionally during generation; the validation step rejects them, and the rejection is part of the diagnostic record.

These checks establish procedural plausibility, not verified structural safety. Actual structural adequacy depends on material properties, loads, spans and detailed engineering. The generator should use conservative archetype-specific limits and explicit support patterns, without claiming to perform full engineering analysis.

## 8. Determinism, runtime use and districts

A building should be reproducible from its location and seed. For example, a city generator can derive each building's seed from the city seed and the integer coordinates of the building's grid cell. Returning to the same location then recreates the same choices without storing every building permanently.

A **district** is a named set of biases applied when buildings are generated, such as archetype weights, floor-count scaling and the palettes in use. A downtown district might favour tall glass towers and twin towers, while a midrise district favours stone and bronze office blocks. An industrial district selects only plant archetypes and may bias the plant style. Districts change which permitted choices are likely; they never relax a rule. The same cell seed under a different district may therefore yield a different archetype, but any building is still valid.

Small utility structures, such as the electrical substation in section 11.8, are given a low weight in every district, because a city needs them almost everywhere. An industrial district gives them a higher one.

The system may cache recently used graphs, meshes or both. Caching is a performance optimisation; it should not change the rules or the identity of a building. If the same seed, district and configuration are used, regenerating a building should give the same result.

## 9. Simplified algorithm sketch

```text
seed = deriveSeed(worldSeed, buildingLocation)
rng = createDeterministicRandomStreams(seed)

archetype = chooseEligibleArchetype(rng, district, siteConstraints)
graph = expandBuildingGrammar(archetype, rng)      // sections, profile, ground floor

footprints = generateFootprints(graph, rng)         // shapes, voids, rotations, slices
resolveSupportRelationships(graph, footprints)      // overhangs, spans, step-outs, arcades
validateSectionsAndSupports(graph)

placeEntrancesAndOpenings(graph, rng)
validateBuilding(graph)

if validation fails:
    retry permitted choices, then simplify if necessary

treatments = chooseFacadeAndRoofTreatments(graph, rng)
mesh = extrudeSectionsAndGenerateFacades(graph, treatments)
collisionGeometry = generateCollisionGeometryIfRequired(graph)
return building, mesh, collisionGeometry
```

This sketch describes the stages and their responsibilities, not a complete implementation specification. In practice, retries should be bounded and random choices should be recorded or reproducible so that failures can be diagnosed. A readable log of accepted and rejected proposals, with reasons, has proved useful for tuning limits.

## 10. Recommended initial scope

The first version should prove the graph, polygon and validation approach before trying to model every kind of architecture.

Include:

- office blocks, skyscrapers, podiums with a projection and twin towers, with a bounded number of floors;
- a small family of footprint shapes: rectangular, chamfered, L, T, U, cross and faceted drum;
- courtyards in office blocks, and rotated towers on podiums;
- repeated floor sections, podiums, setbacks, simple crowns and tower profiles (straight, tapered, waisted, flared, twisted);
- regular façade and window generation with varied bay width, fins or bands, and optional accent and plant bands;
- ground floors with a standard lobby, a double-height lobby or an arcade;
- a small set of roof and crown treatments;
- primary ground-level entrances and optional lower-level service doors;
- the overhang, span and step-out arrangements in section 7, each with explicit abstract supports;
- industrial plants in three styles (gas plant, gothic refinery, night refinery), with the vessel, stack, frame, deck, pipe and access rules in section 11;
- deterministic generation, validation and a bounded fallback strategy;
- districts as sets of generation biases.

Defer detailed interiors, process simulation for plants, stairs and lift layouts, complex structural simulation, curved geometry beyond faceted approximation and exhaustive architectural styles. These features can later be introduced as new graph nodes, footprint operations and constraint rules without redesigning the basic model.

## 11. Industrial plants

Industrial plants reuse the machinery above (a typed graph, polygon footprints, rules before geometry, validation with bounded retries, deterministic streams, districts). What changes is the vocabulary. A plant has no floors or occupied interior: it is a compact set of **units** (vessels, stacks, frames, decks, small buildings) joined by **pipes**, and its plausibility depends on slenderness, spans, supports and access rather than on bays and lobbies. The visual references are a flare-lit night refinery, a gothic-fantasy promethium refinery with domed vessels and elevated gantry decks, and a modern gas plant with tall stacks, steel frames and pipe racks. They differ in style, not in rules.

### 11.1 Graph additions

- **Units:** `column` (a tall slender vessel), `tank` (a squat drum, with a roof walkway), `dome vessel` (a drum with a domed head), `exchanger` (a horizontal drum on saddles), `stack` (a tapering chimney), `flare` (a lattice mast with a flame at the tip), `gasholder` (a guide-frame gasometer: a lattice drum whose telescoping lifts rise and fall with the gas, a rare unit), `frame` (a multi-level steel structure, including the pipe rack) and the **buildings**: `control building`, `hall` (a large single-storey process shed), `warehouse`, `office block` (a multi-storey block with a facade) and `substation` (a switch house with transformers and a gantry).
- **Deck:** an elevated platform with a footprint polygon, an elevation, optional holes where tall vessels pass through, and a set of supports.
- **Pipe:** an ordered route between a port on one unit and a port on another, with explicit supports.
- **Access element:** a ladder or stair from a free piece of ground to an elevated level.

New relationships are `connects` (pipe to its two units), `braced_by` (vessel to frame), `reached_by` (deck or frame level to its access element) and the existing `supported_by`, which now also covers legs, bents, vessel collars and pipe posts.

### 11.2 Plant grammar

- `Plant → Layout + Units + Connections + Access + Utilities`
- `Plant → ... + Works`, where `Works → Building*` (zero to one building on a light site, two to four on a heavy one)
- `RackPlant → PipeRack + UnitRow(side A) + UnitRow(side B) + Stack + OptionalFlare + ControlBuilding`
- `DeckPlant → ElevatedDeck + CarrierVessels + UnderDeckVessels + FlankingUnits + BridgePipes + Stack + OptionalFlare + ControlBuilding`
- `Unit → Column | FramedColumn | Tank | DomeVessel | Exchanger | Stack | Flare | Building`
- `Building → ControlBuilding | Hall | Warehouse | OfficeBlock | Substation`
- `FramedColumn → Column + SteelFrame(levels)`

A layout is chosen first, then units are proposed in turn and validated individually, then pipes are routed between them, then access elements are placed. A proposal that fails validation is retried with an alternative, for example a shorter column, or a tall column promoted to a framed column, before the unit is dropped.

### 11.3 Footprints and clearances

Vessels use the faceted drum, frames, racks, exchangers and buildings use rectangles, and decks use a rectangle with chamfered corners. A deck may have holes, one for each vessel that passes through it, each kept wholly inside the deck with a minimum margin and a clear ring around the vessel. All footprints must lie inside the site and keep a minimum access gap to every other unit. A flare also keeps a larger separation from every other unit, so it usually ends up near the edge of the site.

### 11.4 Support and bound rules

These follow the same pattern as section 7: an eligible unit, a bounded quantity, an explicit support, a continuous load path, and clear geometry.

| Arrangement | Description | Support | Bounded quantity |
|---|---|---|---|
| Free-standing vessel | A column, tank or stack stands on the ground | Its own shell and foundation | Slenderness (height over diameter): column 7.5, stack 13; tank height at most 1.3 times its diameter |
| Framed column | A taller column is tied to a steel frame around it | The frame, at two or more levels | Slenderness up to 14 |
| Lattice mast | A flare is a three-legged tapering mast | The mast base | Base width at least one tenth of the height |
| Pipe rack | A row of pipes on cross-bents | Pairs of posts at each bent | Bent spacing |
| Elevated deck | A platform above the ground between or around units | Legs and vessel collars | Reach (every point of the deck within 10 m of a support); overhang beyond the support hull; centre of mass inside the hull |
| Vessel-carried deck | A deck bears on a collar on a vessel that passes through it | The vessel, counted as one support point | Hole clearance; the vessel must be large enough to carry it |
| Gasholder | A guide-frame gasometer, with a lattice frame of posts and rings around nested lifts | Its guide posts and base ring | Frame height at most one times its diameter; guide posts no more than 9 m apart around the circumference; a roof ladder |
| Hall or warehouse | A large single-storey shed with a gable or saw-tooth roof | Its walls and roof trusses | Roof span at most 22 m; eaves height; a clear side for a door |
| Office block | A multi-storey block | Its core and frame | Height at most 4.5 times its narrow side on the ground tier, 10 times on the colossal tier; at least three bays across, or four if it is taller than about 40 m |
| Pipe run | A pipe leaves a port, rises to a run height, crosses and drops to another port | Posts or sleepers, standing on the ground or on a deck | Support spacing; clearance from other units |

### 11.4.1 Scale tiers

A plant is generated at one of two scales, chosen from the seed (the district may bias it).

- **Ground:** the vocabulary above at its usual size. Stacks and flares reach roughly 30 to 60 m.
- **Colossal:** the vertical rise of every tall unit is multiplied by a factor of about 2.6 to 3.9 so that stacks, flares, columns and office blocks stand alongside the large residential and commercial towers, up to a hard cap of about 235 m. Radii grow with the height, but only as far as the slenderness limits require, so a taller unit is a wider unit. The rules are the same ones, not looser ones: a column that is too slender is first widened and, if it is still too slender, tied to a frame or shortened; a stack is widened up to a maximum diameter before it is shortened; a flare keeps its base at least a tenth of its height and is capped at a base width of about 22 m. Office blocks on this tier are taller and wider, and may stand up to ten times their narrow side because they carry a core.

Pipe ports on a colossal unit stay within the lower 26 m, where the pipe racks and decks are, and a deck is raised to between 13 and 22 m so that vessel-carried decks keep a believable proportion. A colossal tier is dropped when the generator has to simplify twice.

Slenderness limits are plausibility proxies chosen so that a tall free-standing column is rejected and then either shortened or promoted to a framed column. They are not wind or seismic checks.

The gasholder is deliberately rare: only about one plant in eight is offered one (fewer end up with one, because it needs a large free circle), a little more often in the gothic and night styles than in the modern gas plant, and it is never added once the generator has had to simplify twice. It is placed early, because it needs the largest free circle on the site, and omitted with a logged reason if there is no room. Its fill level (how far the lifts are raised) is a random visual choice and does not affect validity.

### 11.5 Connections and access

- **Pipes are semantic.** Every process unit should have at least one connected port. A route is orthogonal (stub, riser, horizontal run, drop) and is tried in both axis orders and at several run heights. A route is rejected if any run passes through a unit or if a support post would stand inside one. A pipe that cannot be routed is dropped and the reason is recorded.
- **Elevated levels need access.** Every deck, framed level and tank roof is reached by a ladder or stair whose foot stands on free ground. Access elements are placed on exterior edges, not in front of other units.
- **Entrances.** A plant has one primary entrance: a door on a control building that faces the street side of the site. This is validated like a ground-level entrance in section 6. Halls, warehouses, office blocks and substations each get a service door on a side with free ground in front of it, and validation rejects a door that opens onto an obstruction.
- **Clearance.** A flare keeps its separation distance from every other unit, because of the heat it radiates, and stacks keep clear of decks.

### 11.6 Style treatments

As in section 4.4, style changes appearance, never structure. Each style selects a palette, head shapes, a pipe colour and a set of decorations:

- **Modern gas plant:** pale vessels, mint stacks with red and white marker bands, yellow pipework, open steel frames, a long pipe rack, a small flare or none.
- **Gothic refinery:** domed vessels with banded orange, cream and red paint, round hatches, black legs under cream decks, chimneys with cowled tops, and a hoist and claw hanging beneath a deck.
- **Buildings** take the style as well: pale cladding and ribbon glazing for the gas plant, ochre walls with green-lit windows and fins for the promethium refinery, dark clad sheds with saw-tooth roofs glazed in amber for the night refinery.
- **Night refinery:** dark soot-black steel, dense lamps on every level, several flares with flames, steam plumes from stacks, and red obstruction lights.

### 11.7 Algorithm sketch

```text
style  = chooseStyle(rng, district)
scale  = chooseScale(rng, district)                // ground or colossal (vertical rise factor)
layout = chooseLayout(rng, style)                  // rack or deck
units  = []
for each slot in layout:
    unit = proposeUnit(rng, slot, style)
    while validateUnit(unit) fails and attempts remain:   // slenderness, footprint, clearance
        record the rejection; adjust unit (shorten, add frame ties, resize)
    if valid: units.add(unit)
deck   = layout.deck ? proposeDeck(rng, units) : none
validateSupports(deck)                              // spacing, overhang, hull, load path
pipes  = routePipes(rng, units, deck)               // orthogonal, supported, collision-free
access = placeAccess(units, deck)                   // one per elevated level
building = placeControlBuilding(units)              // street-facing door
works    = placeWorks(rng, units, scale)            // halls, warehouses, offices, substations; each needs a clear side for a door
                                                    // too wide a roof is narrowed, too tall a block loses floors, too little ground shrinks it
validatePlant(units, deck, pipes, access)
if validation fails: retry, then simplify (fewer units, no frames, fewer pipes)
```

### 11.8 Electrical substation

An electrical substation is a small, self-contained site that fits almost anywhere, so it is offered as an archetype of its own in every district and not only in industrial ones. It is built from the same plant machinery (units, footprints, validation, a style, a fence and a gate) but is much smaller than a plant: a fenced pad of roughly 35 to 55 m by 27 to 34 m, instead of the whole site.

- **Units:** one to three `transformer`s, each standing in an oil bund; one `busbar gantry` of portal frames carrying three-phase conductors on insulator strings; a `switch house` with a service door; a small `control kiosk`; floodlight masts at the corners.
- **Grammar:** `Substation → Pad + TransformerRow + BusbarGantry + SwitchHouse + Kiosk + Fence + Gate`. The gantry runs along the back of the pad, the transformers stand in a row in front of it, and the switch house and kiosk sit at the street side, either side of the gate.
- **Relationships:** each transformer `connects` to the gantry (three phases), every unit is `supported_by` the foundation, and the switch house door `opens_on` the street side and `opens_into` the switch room.
- **Rules:**

| Arrangement | Support | Bounded quantity |
|---|---|---|
| Transformer in a bund | Its bund and foundation | Bund at least 1.5 m larger than the tank on every side |
| Transformer spacing | An oil fire must not spread | Bund-to-bund gap at least 5 m; closer than 10 m, a firewall is placed between them |
| Busbar gantry | Portal frames | Bay span at most 12 m; the gantry must cover every transformer it feeds |
| Gate and door | A clear lane | The gate lane and the area in front of the switch house door stay clear; the gate faces the street |

- **Style:** the usual three palettes, with pale cladding for the modern style, ochre walls and green-lit windows for the gothic style, and dark cladding for the night style. Style changes appearance only.
- **Validation and retry:** if a row would run off the pad or the switch house would block the gate, the proposal is rejected, a transformer is dropped or moved, and the layout is retried; after repeated failures the substation is simplified to a single transformer.

## 12. Expected result

The proposed system generates buildings by selecting a permitted structural pattern, creating valid two-dimensional footprints for its sections, checking the relationships between those sections, and converting the accepted result into three-dimensional geometry. Its variety comes from controlled choices within a rule set, spread across several independent axes, while its graph and constraints provide a way to prevent invalid combinations.

Industrial plants extend the same foundation to a second family of structures, where the explicit rules are slenderness, supports, spans, pipe routes and access rather than floors and façades.

This provides a foundation for generating coherent, repeatable city architecture, with ordinary office buildings as the baseline and more distinctive forms, such as stepped, flared or twisting towers, bridged twin towers and supported overhangs, introduced as explicit, validated variations.
