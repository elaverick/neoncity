# Rule-Based Procedural Generation of Office Buildings and Skyscrapers

**Status:** Proposed high-level design, prototyped in a browser toy

## 1. Purpose

The goal is to generate varied, coherent office blocks and skyscrapers at runtime without hand-authoring every building. A building should be described by rules and parameters, then constructed from a sequence of geometric operations.

The proposed system combines two complementary ideas:

- **A typed building graph** describes the building's parts and how they relate to one another: which sections sit above other sections, what supports them, and where entrances and façade openings belong.
- **A polygon-based geometry system** creates the horizontal footprint of each section. These two-dimensional outlines are then extruded and joined to produce the three-dimensional building.

The graph defines the building's organisation; the polygons define its shape. Keeping those responsibilities separate makes the system easier to validate, extend and debug.

The initial target is exterior geometry for office blocks and skyscrapers. The system should support regular buildings as the common case, with less common features, such as projecting upper floors, spans and twisting profiles, introduced only when their rules can be satisfied. Individual buildings can then be placed on a grid to form districts whose character is set by biasing the choices, not by changing the rules.

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
- **Façade segment:** an exterior edge or face that can receive façade treatment and, where permitted, openings.
- **Opening or entrance:** a door, loading opening or other opening with a semantic purpose and placement constraints.

### Principal relationships

- `above` / `below`: vertical ordering of sections.
- `supported_by`: a section or projecting part depends on a support element or supported section. A section may have more than one supporter.
- `contains`: a building contains sections; a section may contain support elements, voids or façade features.
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

A **district** is a named set of biases applied when buildings are generated, such as archetype weights, floor-count scaling and the palettes in use. A downtown district might favour tall glass towers and twin towers, while a midrise district favours stone and bronze office blocks. Districts change which permitted choices are likely; they never relax a rule. The same cell seed under a different district may therefore yield a different archetype, but any building is still valid.

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
- deterministic generation, validation and a bounded fallback strategy;
- districts as sets of generation biases.

Defer detailed interiors, stairs and lift layouts, complex structural simulation, curved geometry beyond faceted approximation and exhaustive architectural styles. These features can later be introduced as new graph nodes, footprint operations and constraint rules without redesigning the basic model.

## 11. Expected result

The proposed system generates buildings by selecting a permitted structural pattern, creating valid two-dimensional footprints for its sections, checking the relationships between those sections, and converting the accepted result into three-dimensional geometry. Its variety comes from controlled choices within a rule set, spread across several independent axes, while its graph and constraints provide a way to prevent invalid combinations.

This provides a foundation for generating coherent, repeatable city architecture, with ordinary office buildings as the baseline and more distinctive forms, such as stepped, flared or twisting towers, bridged twin towers and supported overhangs, introduced as explicit, validated variations.
