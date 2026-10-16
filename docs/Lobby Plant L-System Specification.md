# Lobby Plant Hybrid Plant-Generation Specification

**Specification ID:** LPLS-1.19  
**Purpose:** Hybrid L-system / space-colonization generation of varied, realistic-looking 3D potted plants for hotel, office, retail, and residential lobbies  
**Implementation target:** Platform- and language-agnostic; suitable for a real-time 3D engine

## Current build identity

The implementation and this document are **LPLS-1.19**. The browser toy displays this ID in the page header and build diagnostics.

## 1. Design goals

The system must generate recognisably different mature indoor plants, not merely different arrangements of the same branching tree. Each plant is described by a parametric, bounded, stochastic L-system and a small library of reusable geometry-generating terminal modules.

The grammar defines **architecture**: trunks, canes, axes, branches, nodes, leaf attachment positions, petioles, aerial roots, and compound-leaf arrangements. Terminal modules define **organ shape**: leaf blades, leaflets, palm fronds, umbrella leaves, and tapered stems. This division is intentional. Expanding every leaflet as a deep recursive string is unnecessary for visual variety and can become expensive. A compound organ can be emitted as one compact module while still being placed by the L-system.

The target is a static or rarely-changing mature plant. Growth animation can be added later by retaining the derivation depth or assigning birth times to generated organs; it is not required for normal lobby use.

### Non-goals

- Simulating plant physiology, water use, phototropism, or years of actual growth.
- Producing scientifically exact specimens for botanical research.
- Generating soil, decorative pots, planters, or lobby dressing. Those are separate scene assets, although the plant definition exposes a soil-level attachment point and a recommended footprint.

## 2. Authoritative generation pipeline

This sequence is normative. Each plant passes through the applicable stages once when created or when its parameters change. The plant grammar remains the authority for species identity; optional hybrid stages may refine a grammar but may not replace its distinctive architecture.

1. **Resolve parameters and envelope.** Select one species definition and resolve `height`, `widthScale`, `age`, `quality`, and deterministic `seed`. Define its footprint and growth envelope in local plant coordinates.
2. **Build the species macro-architecture.** Expand the bounded parametric L-system to produce the plant's primary axes and organ attachment transforms. For the woody-crown species `ficus_lyrata`, `ficus_benjamina`, and `schefflera_arboricola`, run space colonization to add secondary woody branch graphs inside the crown envelope by default. The colonization graph supplements—not replaces—the L-system trunk and species grammar. Use the L-system alone for Kentia, Dracaena, and ZZ because their architectures are not generic branching trees.
3. **Apply bounded space colonization where enabled.** Sample deterministic attraction points only inside the assigned envelope. In each iteration, associate live points with their nearest eligible branch node; remove points inside the kill radius; sum unit steering vectors for points within the influence radius; propose a bounded growth step. Accept a candidate only when it remains inside the envelope, is sufficiently far from existing non-parent nodes, respects branch/node/iteration budgets, and connects to its parent. Stop when no candidate can be accepted or a hard budget is reached.
4. **Interpret grammar and emit organ descriptors.** The 3D turtle emits tapered stem segments, petiole and rachis axes, compound-organ descriptors, blade descriptors, and aerial-root descriptors. Every organ stores a stable node path and deterministic local parameters.
5. **Refine woody centre-lines.** Apply low-amplitude deterministic three-octave coherent noise to the branch centre-lines. Apply one-pass gravity sag weighted by branch length, thickness, and flexibility. Fade deformation to zero at fixed root, trunk/crown, and graft-like attachment junctions. Do not apply branch noise to leaf contours.
6. **Generate organ geometry.** Use the shared leaf-shape system with a species profile. Compound fronds/heads use organ modules; repeated small foliage uses low-poly cluster templates. Fiddle-leaf fig signature blades use dedicated blade geometry.
7. **Place and orient foliage.** Use the species' phyllotaxis rule. Golden-angle rotation is available for suitable spiral arrangements, but never overrides opposite, two-ranked, pinnate, or palmate arrangements. A node must retain its required foliage even if a cluster is added nearby; cluster foliage is supplemental unless the species rule explicitly defines a cluster as the leaf organ.
8. **Apply geometry budgets and LOD.** Estimate costs before constructing meshes. Prune lower-priority branches and reduce small-organ counts before dropping major silhouette organs. Never stop foliage halfway through a branch or leave a structural branch bare solely because a global foliage counter was exhausted.
9. **Batch, validate, and cache.** Merge compatible static geometry, instance repeated cluster templates, calculate final bounds and normals, validate organ/triangle/draw-call budgets, and cache by `(species, seed, resolved parameters, quality, generator version)`.

### 2.1 Space-colonization dispatch

| Species | Macro-skeleton method | Rule |
|---|---|---|
| Fiddle-leaf fig | L-system trunk plus bounded colonized upper crown | Keep large individual leaves and a visible lower trunk. |
| Weeping fig | L-system braided or spiral trunk plus bounded colonized upper crown | Retain grammar foliage on every required twig internode; colonized clusters are supplemental. |
| Dwarf umbrella tree | L-system trunk plus bounded colonized rounded crown | Keep palmate leaflet fans as terminal compound organs, not as tree branches. |
| Kentia palm | L-system trunk and pinnate frond axes | Never apply broadleaf tree colonization to the palm. |
| Dracaena marginata | L-system canes and terminal rosettes | Canes remain mostly bare; foliage stays in terminal heads. |
| ZZ plant | L-system basal compound fronds | No woody-crown colonization. |

### 2.2 Space-colonization defaults

Use a species-specific ellipsoidal or custom envelope. At Low/Medium/High detail, begin with 180/300/460 attraction points and hard caps of 44/82/132 accepted new nodes per crown. Suggested defaults are `influenceRadius = 2.5 * stepLength`, `killRadius = 1.2 * stepLength`, and `collisionDistance = 0.65 * stepLength`; species definitions may override them. All values are scaled by the species reference height. Reject candidates outside the envelope; never project an outside node back onto the boundary, as that can create clumps along the silhouette.

### 2.3 Branch refinement

Use a pure deterministic noise function keyed by `(seed, species, stable node path, position, octave)`. Sum three bounded octaves and normalize amplitude. Branch flexibility decreases with wood radius and increases with branch length. The interactive `gravityStrength` parameter defaults to `0.85` on a range of `0–1.5` and affects both flexible woody axes and individual leaves. When `branchPhysics` is disabled, gravity-driven branch and organ sag is disabled. For L-system axes, track `branchRun`, the accumulated segment length since the current branch's `PUSH`; reset it on `PUSH` and restore it on `POP`. For each segment of length `ℓ`, let `s0` and `s1 = s0 + ℓ` be the previous and next branch-run lengths, `H` the plant reference height, `f` the flexibility in `[0,1]`, `h = sqrt(dir.x² + dir.z²)` the horizontal component of its direction, and `G = gravityStrength`. Compute `drop = min(0.45ℓ, G * 0.82 * f * h * (s1² - s0²) / H)`. Bias the segment tangent downward by `drop/ℓ`, normalize it, and orient the turtle frame to that tangent before emitting the next segment. This gives a cumulative parabolic droop rather than a constant sideways tilt, caps local curvature to prevent short twigs from folding, and leaves near-vertical trunks largely upright because `h` is small. For space-colonized woody crowns, apply `dir.y -= G * 0.11 * flex * horizontal * clamp(parentDistance / (0.28H), 0.15, 1.5)` before normalizing the growth direction. Preserve parent-child connectivity, the crown envelope, and fixed trunk-macro attachment endpoints. Do not reuse wood-noise displacement on leaf contours; apply organ sag using the leaf/petiole rules in Section 2.4.

### 2.4 Leaf, petiole and leaflet sag

The same `gravityStrength` control affects organs as well as their supporting branches. Branch deformation determines each leaf's attachment position and inherited orientation; organ sag then bends the petiole and blade independently. This deformation is baked into geometry during plant generation and must not run per frame.

Set the effective organ gravity to `G = branchPhysics ? clamp(gravityStrength, 0, 1.5) : 0`. Use species/shape-specific flexibility ratios, with broad fiddle-shaped and elliptic blades more flexible than narrow linear leaves and the thick leaflets of ZZ. Scale blade sag with organ length and a bounded width-to-length factor. For normalized blade coordinate `t` from base to tip, add downward displacement `(baselineDroop + G * L * flex) * t²`, where `L` is blade length and `flex` is the shape profile's blade-flex coefficient. Add modest additional down-curl at the outer margins, proportional to `G * L * edgeFlex * u² * sin(πt)`, where `u` spans -1 to +1 across the blade. Keep camber/edge cupping in the local blade normal direction separate from world-down gravity displacement.

Build a petiole from two to four tapered segments according to detail tier. Its centreline curves downward with `G * petioleLength * petioleFlex * t²`; orient the attached blade to the petiole's terminal tangent so the blade hangs naturally rather than remaining rigidly aligned with the original branch direction. Limit curvature so short petioles cannot fold back on themselves. For prebuilt/instanced foliage clusters, bake small species-specific droop into the cluster template at rebuild time; retain instancing for low draw-call cost. Gravity values of zero remove the added gravity-driven response while preserving any fixed species-authored baseline droop.

Recommended shape-flex coefficients in the reference implementation are: fiddle `0.115`, elliptic `0.135`, linear `0.070`, ZZ `0.055`, and generic simple blade `0.095`. Edge-flex coefficients are smaller (approximately `0.008–0.022`). These are dimensionless ratios, not world-space offsets; scale the result by blade length and clamp extreme displacements. Validate all presets at gravity values `0`, `0.85`, and `1.5` to confirm visible increased droop without inversion, floor penetration, or broken petiole connections.

### 2.5 Phyllotaxis and cluster placement

For spiral leaf arrangements, the default divergence angle is the golden angle, `α = π(3 − √5) ≈ 137.5078°`, with small deterministic jitter. Apply it only where botanically appropriate. Schefflera leaflets remain a radial whorl; Kentia leaflets remain paired along a rachis; ZZ leaflets remain alternating/subtly offset; Dracaena leaves remain a rosette. Instancing is a rendering optimisation and must not change the plant's geometry or organ arrangement.

## 3. Formal model

Use a **parametric stochastic L-system with guarded productions**. Each definition contains an axiom, a set of productions, and a parameter block. Productions are applied in parallel to non-terminal symbols. A rule with a guard applies only when its guard is true; where multiple alternatives exist, their stated weights determine the choice using the plant's deterministic random stream. A terminal symbol is interpreted geometrically and is not rewritten.

The notation below is intentionally pseudocode-like rather than tied to a particular L-system package. It can be translated to a conventional parametric L-system, a string-rewriting implementation, or a typed module list.

### 3.1 Rule notation

- `A(k,d) -> ...` — rewrite non-terminal `A` with parameters `k` and `d`.
- `when predicate` — guard for a production.
- `otherwise` — fallback production for that symbol.
- `ε` — remove the symbol; it emits no geometry.
- `choose { p: rule; ... }` — choose a rule by relative weight `p`.
- `rand(node, channel)` — deterministic pseudo-random value in `[0,1)` keyed by the plant seed, the stable node path, and a named channel.
- `jitter(node, channel, amplitude)` — deterministic value in `[-amplitude,+amplitude]`.
- `clamp(x,a,b)` — constrain `x` to the closed interval `[a,b]`.
- `min`, `max`, `smoothstep` and ordinary arithmetic have their usual meanings.

Use stable per-node randomness rather than one sequential random-number stream. A change to an unrelated branch should not cause the rest of the plant to reshuffle.

### 3.2 Parameters

Every plant instance has at least:

| Parameter | Meaning | Suggested default |
|---|---|---|
| `seed` | Stable seed for all stochastic choices | Caller-supplied integer or stable object ID hash |
| `height` | Plant height above the soil line, excluding its pot | Archetype-specific; normally 1.2–2.4 m |
| `widthScale` | Horizontal spread multiplier | 1.0 |
| `age` | Fullness/maturity control, not simulated age in years | 0.85–1.0 for mature lobby plants |
| `gravityStrength` | Bounded sag strength for flexible branches, petioles, individual blades, and cluster-leaf templates | 0.85; adjustable from 0 to 1.5 |
| `quality` | Geometry budget/LOD tier | `medium` |
| `leafVariation` | Strength of leaf-level variation | 0.08–0.18 |
| `bendVariation` | Strength of stem/branch directional variation | 0.03–0.10 |
| `foliageTintVariation` | Small colour variation within an archetype palette | 0.02–0.06 |

The archetype supplies all other values, including internode lengths, branching rates, leaf geometry descriptors, angles, and maximum counts. Scale linear dimensions from the archetype's reference height; do not scale every value independently if that would make leaves disproportionate to stems.

### 3.3 Bounded growth and budgets

Each definition declares explicit limits: maximum axial nodes, maximum branch depth, maximum branches, maximum emitted leaves/leaflets, and maximum derivation passes. The interpreter must also enforce a global organ/triangle budget. If a budget is reached, reject lower-priority buds first (normally the smallest, deepest, and most occluded branches), while keeping the primary axis and major silhouette organs.

The stochastic rules affect small-scale detail and branch choices, but the maximum height, broad silhouette, major crown structure, and root point must remain controlled by the species parameters. Avoid unrestricted productions such as `A -> A[+A][-A]` without a depth or count guard.

## 4. Shared 3D turtle and terminal modules

Each plant is generated in a local plant coordinate frame. The renderer may transform that frame into any world-space convention. The turtle maintains an orthonormal local frame: **forward**, **right**, and **up**. Initial forward points along the plant's primary growth direction; the initial right/up orientation is fixed for the species and seed.

### 4.1 Turtle commands

| Symbol | Meaning |
|---|---|
| `F(length, r0, r1, material)` | Emit a tapered stem segment from the current position to the position `length` units forward; radii taper from `r0` to `r1`; then advance the turtle to the segment end. |
| `f(length)` | Advance without emitting geometry. |
| `+(angle)` / `-(angle)` | Rotate around the turtle's current local up axis by positive/negative angle. |
| `&(angle)` / `^(angle)` | Pitch around the current local right axis by positive/negative angle. |
| `/(angle)` / `\(angle)` | Roll around the current forward axis by positive/negative angle. |
| `[` | Push the complete turtle pose and stem state onto a stack. |
| `]` | Restore the most recent pushed state. |
| `L(shape, length, width, petiole, droop, twist)` | Emit one terminal leaf. It does not advance the turtle. |
| `P(shape, length, width, leafletCount, droop)` | Emit a compound pinnate frond (rachis plus leaflets) or a similarly structured organ. It does not advance the turtle. |
| `U(shape, leafletCount, length, width, droop)` | Emit a compound palmate/umbrella leaf from a hub. It does not advance the turtle. |
| `W(length, curvature, radius)` | Emit an aerial root or exposed root; terminal module only. |
| `BT(strand, strandCount, length, phase, turns, orbit, radius)` | Emit one tapered helical strand whose centreline converges on the shared trunk axis at both ends. |
| `BC(length, radius)` | Emit a slender central trunk core that touches the braided strands and continues into the crown junction. |
| `ST(length, turns, orbit, phase, radius)` | Emit one genuinely curved/spiralling leader trunk. The centreline moves in 3D; spinning the turtle frame alone is not a trunk twist. |
| `J(height)` | Align the turtle at the top of an independently generated trunk macro without adding geometry. In a conventional turtle-only implementation, this can be replaced by moving along the generated centreline and returning its final pose. |
| `ε` | No output. |

All rotations are local-frame rotations, not rotations around a fixed global axis. The stack must save and restore position, orientation, current stem settings, and any other state that can affect interpretation. Bracket pairs must balance before geometry is accepted.

### 4.2 Terminal geometry modules

**Tapered stem `F`:** Generate a tube or joined tube segment with a tapered radius and a slightly irregular centre-line. Adjacent segments should meet cleanly. Use a low-sided cross-section for small twigs and more sides for trunks visible at close range. Slightly flare major stems near the soil line; do not use a uniform cylinder for every branch.

**Simple leaf `L`:** Generate a curved blade with a species-specific outline, a petiole, and a midrib. A practical leaf can use a small triangulated surface with a curved centre-line and a width profile (narrow at the petiole, widest near a species-specific fraction of the length, then tapered to the tip). Add subtle camber, twist, and edge waviness. Leaf orientation should be part of the grammar, not left entirely to random rotation.


**Compound frond `P`:** Generate a curved rachis and a bounded number of leaflets arranged along it. Each leaflet has a tapered outline, a small attachment angle, and a modest down-curve. For a palm, vary leaflet length toward the tip and alternate the left/right attachment positions. Construct the rachis and leaflets in one organ-generation pass; it is not necessary to rewrite every leaflet through several grammar generations.

**Umbrella leaf `U`:** Generate a central hub and several elliptic leaflets radiating from it, each on a short petiolule. The leaflet count, angular spacing, and droop determine the silhouette.

**Aerial root `W`:** Generate a thin curved cylinder or tapered curve. Limit roots to selected nodes and trim them at the soil/planter boundary where necessary.

**Braided trunk strand `BT`:** Generate a tapered tube along a bounded helical centre-line. The strand's centreline radius is `orbit * sin(πu)^0.7`, where `u` runs from 0 at the soil to 1 at the crown junction. The envelope brings every strand into the same basal root collar and upper junction; it must not leave separate floating endpoints. Keep orbit radius small enough that neighbouring strand surfaces touch, and add a slender central core `BC` that overlaps their inner surfaces slightly along the trunk. Use 4–5 strands, evenly phased around the axis, with small seeded variation in turn count and phase. Sample the curve at a quality-dependent bounded number of points.

**Single-leader spiral trunk `ST`:** Construct the trunk from an explicit 3D centreline with both vertical progress and lateral displacement. Use a smooth helical or corkscrew curve with a small asymmetric perturbation and an envelope that returns the centreline to the main axis at the soil and crown junction. Taper the tube from a visible basal radius to a smaller upper radius. Do not implement this by repeatedly rolling the turtle around its own forward axis: that rotates the coordinate frame but leaves a straight trunk path.

**Trunk/crown connection:** After `BT` + `BC`, or after `ST`, use `J(height)` to start the crown leader at the trunk's exact upper endpoint. The first crown segment must overlap the trunk junction in position and radius. Primary branches grow from nodes on this crown leader; their first segment begins at the attachment node and overlaps the supporting stem. Branches and trunks should form one connected wood structure, not independent decorative tubes placed near each other.

### 4.3 Leaf surface and material guidance

- Use species-specific outlines. A stretched ellipse is not an adequate universal leaf model.
- Treat each large signature leaf as a real triangulated surface with a curved centre-line, midrib, and species-appropriate thickness/camber. Recompute normals after deformation. Small compound leaflets may use cheaper reusable templates.
- Use species-specific roughness and gloss ranges. Ficus benjamina and ZZ leaves should read glossier than Kentia fronds. Fiddle-leaf fig blades use broad, visibly veined surfaces.
- Add restrained per-organ tint variation within a coherent species palette.
- Geometry must remain meaningful without special shaders. Renderer-specific translucency, normal maps, and subsurface effects are optional enhancements.

## 5. Shared production helpers

The names below are semantic helpers used in the species rules. Each plant definition may give them different parameter values; the helper names are not extra runtime requirements.

### 5.1 One-node axis pattern

An axial non-terminal `A(k,d)` represents the next growth point on an axis. In broadleaf definitions it typically expands as follows:

```text
A(k,d) -> F(internode(k), radius(k), radius(k+1), stemMaterial)
          [ rotationForLeaf(k) L(leafShape, leafLength(k), leafWidth(k), petiole(k), droop(k), twist(k)) ]
          [ optionalBranch(k,d) B(nextBranchIndex, d+1) ]
          A(k+1,d)
          when k < axisNodeLimit

A(k,d) -> [ rotationForTerminalLeaf(k) L(...) ]
          optionalTerminalOrgans(k)
          ε
          when k >= axisNodeLimit
```

This is a pattern, not a universal production that should be copied unchanged to every species. The palm, rosette and compound-leaf rules later in this document use different architecture.

### 5.2 Branch pattern

A branch bud must carry at least its depth and a finite node index. A generic branch grows along a short axis, places leaves at selected nodes, and terminates at its own node limit:

```text
B(k,d) -> F(branchLength(k,d), branchRadius(k,d), nextRadius(k,d), branchMaterial)
          [ rotationForBranchLeaf(k,d) L(branchLeafShape, leafLength(k,d), leafWidth(k,d), petiole(k,d), droop(k,d), twist(k,d)) ]
          B(k+1,d)
          when k < branchNodeLimit(d) and d <= maxBranchDepth

B(k,d) -> terminalLeafOrBud(k,d) ε
          otherwise
```

Optional sub-branches must be guarded by both a depth limit and a per-plant branch budget. A production may choose `ε` probabilistically to create natural gaps.

### 5.3 Angle, phyllotaxis and natural variation

Use angle patterns that suit the species. A golden-angle-like divergence near 137.5 degrees is a useful starting point for spiral leaf arrangements, but opposite leaves, two-ranked leaves, or palm fronds require their own arrangement. The golden angle must not be imposed universally.

A recommended generic pattern for a spiral organ sequence is:

```text
organYaw(k) = k * 137.5 degrees + jitter(node(k), "organ-yaw", yawNoise)
```

Randomness should perturb a deliberate base pattern rather than replace it. For example, use bounded variation in leaf tilt, branch azimuth, stem curvature, leaf length, and leaflet spacing. Avoid independent random values so large that the plant loses its species silhouette. In the productions, names such as `alternatingYaw`, `branchYaw`, and `organYaw` mean **azimuth around the current axis**: express this as a roll `/(angle)` before applying `&`/`^` to lean a leaf or branch away from the axis. For a near-vertical stem, rolling first rotates the plane in which the subsequent bend occurs; it prevents all side organs from emerging in one flat plane. Use `+`/`-` as additional direction turns only when a definition needs them.

## 6. Species catalogue and shared scaling

The supported library contains six archetypes. These are intended as strong visual silhouettes suitable for a large decorative planter; pot design, soil surface, moss, bark chips, and decorative top dressing are supplied separately.

The dimensions below are reference values for a mature specimen, measured above the soil line. They are not hard limits. `height` scales the reference plant; `widthScale` controls the horizontal envelope. The grammar parameters use metres when they represent distances.

| ID | Archetype | Reference height | Expected silhouette | Primary organ style |
|---|---|---:|---|---|
| `kentia_palm` | Kentia palm (*Howea forsteriana*) | 1.8 m | Slender trunk; arching open crown | Pinnate curved fronds |
| `dracaena_marginata` | Madagascar dragon tree (*Dracaena marginata*) | 1.7 m | Several narrow canes; leafy tufts at their tops | Long, narrow, tapering leaves |
| `ficus_lyrata` | Fiddle-leaf fig (*Ficus lyrata*) | 1.8 m | Upright woody stem with sparse branches and large leaves | Broad fiddle-shaped blades |
| `ficus_benjamina` | Weeping fig (*Ficus benjamina*) | 2.0 m | Braided multi-stem lower trunk with a high, fine, irregular crown; natural single-leader alternative | Numerous small elliptic leaves |
| `schefflera_arboricola` | Dwarf umbrella tree (*Heptapleurum arboricola*, syn. *Schefflera arboricola*) | 1.7 m | Woody stems with clustered leaf fans | Palmate compound leaves |
| `zamioculcas` | ZZ plant (*Zamioculcas zamiifolia*) | 1.0 m | Multiple arching upright stalks from the soil | Paired, glossy elliptic leaflets |

## 7. Plant-specific L-system definitions

These definitions use the shared commands from Section 4. Non-terminals are specific to each of the six supported archetypes, so symbols may be reused between definitions. `N` values are derived from the resolved height and quality tier. Numeric values shown are suitable starting points; tune them against reference photographs and the target camera distance.

### 7.1 Kentia palm — `kentia_palm`

**Architecture:** A single slender, ringed trunk ending in a crown of roughly 7–12 long, arching pinnate fronds. Fronds radiate around the trunk; they are not branches growing all the way up the trunk. Each frond has many narrow leaflets that become shorter toward the tip.

**Reference parameters**

- Trunk height: `H = height`; trunk radius at base approximately `0.035 * H`, tapering to `0.018 * H` near the crown.
- Trunk segments: `Nt = clamp(round(H / 0.12), 10, 22)`.
- Fronds: `Nf = clamp(round(8 + 2 * age), 7, 12)`.
- Leaflets per frond: 18–30 at medium/high quality; reduce with LOD.
- Frond length: 0.45–0.75 of total height; outer fronds tend longer and droop more.

**Axiom**

```text
TRUNK(0) CROWN(0)
```

**Productions**

```text
TRUNK(i) -> F(H/Nt, trunkRadius(i), trunkRadius(i+1), "palm-trunk") TRUNK(i+1)
           when i < Nt
TRUNK(i) -> ε
           when i >= Nt

CROWN(i) -> [ /(i*137.5deg + jitter(i,"azimuth",10deg))
              &(crownPitch(i))
              P("kentia-frond", frondLength(i), frondWidth(i), leafletCount(i), frondDroop(i)) ]
            CROWN(i+1)
            when i < Nf
CROWN(i) -> ε
            when i >= Nf
```

`CROWN` executes after `TRUNK`, so each frond attaches at the crown point. The bracket around each frond restores the crown's attachment pose before the next one is emitted. `P("kentia-frond",...)` curves the rachis outward and downward, with individual leaflets alternating along the rachis. Keep a few fronds more upright and some more horizontal; a uniform umbrella of identical fronds looks artificial.

### 7.2 Dracaena marginata — `dracaena_marginata`

**Architecture:** Three to five slim canes of different heights, each terminating in a compact rosette of long, narrow leaves. Cane bases may be close together, but must diverge above the soil line. The leaf tufts are dense at the top and largely absent from the bare cane.

**Reference parameters**

- Canes: 3 by default; occasionally 4 or 5 at a high quality tier.
- Cane heights: independently sample within `0.55H` to `H`, then ensure one or two tallest leaders dominate.
- Cane base radius: `0.018 * H` to `0.035 * H`; taper toward the top.
- Leaves per crown: 14–26; length 0.18–0.42 m for a 1.7 m plant.
- Leaf width: slender, typically 2–8% of leaf length.

**Axiom**

```text
[ CANE(0, height0, 0) ]
[ /(120deg) &(caneLean1) CANE(0, height1, 1) ]
[ /(240deg) &(caneLean2) CANE(0, height2, 2) ]
```

For a truly vertical turtle heading, use a small lateral lean as shown and a different local roll/leaf phase per cane. If all bases coincide visually, apply a tiny deterministic base-plane offset before each cane starts.

**Productions**

```text
CANE(i,h,phase) -> F(h/Nc, caneRadius(i,h), caneRadius(i+1,h), "dracaena-cane")
                   &(caneBend(i,phase)) /(caneTwist(i,phase))
                   CANE(i+1,h,phase)
                   when i < Nc
CANE(i,h,phase) -> ROSETTE(0, h, phase)
                   when i >= Nc

ROSETTE(j,h,phase) -> [ /(j*137.5deg + phase + jitter(j,"az",8deg))
                        &(leafPitch(j,phase))
                        L("dracaena-linear", leafLength(j,h), leafWidth(j,h), petiole(j), leafDroop(j), leafTwist(j)) ]
                      ROSETTE(j+1,h,phase)
                      when j < leafCount
ROSETTE(j,h,phase) -> ε
                      when j >= leafCount
```

The rosette is a leaf fan, not a branch tree. Leaves should radiate from the top of each cane, with longer outer leaves curving down and shorter inner leaves pointing more upward. Avoid giving every cane the same height and exactly the same leaf count.

### 7.3 Fiddle-leaf fig — `ficus_lyrata`

**Architecture:** One dominant woody axis, relatively long internodes, large leaves spaced along the axis, and a small number of stout lateral branches. The silhouette is sparse enough for individual leaves to remain legible.

**Reference parameters**

- Main-axis nodes: `Na = clamp(round(H / 0.12), 10, 18)`.
- Branch nodes: normally 2–5 major lateral branches, more likely in the upper half of the plant.
- Main stem radius: base `0.025H`, tapering to about `0.009H` near the top.
- Leaf length: 0.18–0.32 m at the 1.8 m reference height; width typically 0.65–0.85 of leaf length.
- Leaf count: roughly 24–50 including branch leaves at medium/high quality.

**Axiom**

```text
A(0,0)
```

**Productions**

```text
A(k,d) -> F(mainLength(k), mainRadius(k), mainRadius(k+1), "ficus-wood")
          [ /(alternatingYaw(k)) &(leafPitch(k))
            L("fiddle", fiddleLength(k), fiddleWidth(k), petioleLength(k), leafDroop(k), leafTwist(k)) ]
          [ /(branchYaw(k)) &(branchPitch(k)) B(0,1,k) ]
          A(k+1,d)
          when k < Na and branchSlot(k) is enabled

A(k,d) -> F(mainLength(k), mainRadius(k), mainRadius(k+1), "ficus-wood")
          [ /(alternatingYaw(k)) &(leafPitch(k)) L("fiddle", fiddleLength(k), fiddleWidth(k), petioleLength(k), leafDroop(k), leafTwist(k)) ]
          A(k+1,d)
          when k < Na and branchSlot(k) is disabled

A(k,d) -> [ /(alternatingYaw(k)) &(leafPitch(k)) L("fiddle", fiddleLength(k), fiddleWidth(k), petioleLength(k), leafDroop(k), leafTwist(k)) ] ε
          when k >= Na

B(j,d,k) -> F(branchLength(j,k), branchRadius(j,d), branchRadius(j+1,d), "ficus-wood")
            [ +(branchLeafYaw(j,d)) &(branchLeafPitch(j,d)) L("fiddle", branchLeafLength(j), branchLeafWidth(j), smallPetiole(j), branchDroop(j), branchTwist(j)) ]
            B(j+1,d,k)
            when j < branchNodeLimit and d <= maxBranchDepth
B(j,d,k) -> ε
            when j >= branchNodeLimit or d > maxBranchDepth
```

Use a maximum branch depth of 1 or 2. `branchSlot(k)` should favour a few well-spaced nodes and permit occasional skips. Leaf yaw alternates side-to-side along each axis; vary pitch so not all leaf faces are parallel to the camera. The blade module should have the characteristic broad, fiddle-like outline, a strong central vein, and modest rippling at the edge.

### 7.4 Weeping fig — `ficus_benjamina`

**Architecture:** The default lobby specimen is a trained ornamental ficus: a cluster of 4–5 close, intertwined lower stems merges into a single woody root collar and a shared upper crown junction. A fine upright scaffold emerges from this junction. Primary limbs originate in the upper third, first rise and fan outward, then curve over into drooping outer tips. Secondary and tertiary twigs fill the spaces between primary limbs; the result should be a dense, irregular, softly domed crown rather than an open umbrella or a set of bare spokes. Small glossy elliptic leaves are distributed along every internode and gathered at every twig tip.

A natural single-leader option uses one visibly twisted/corkscrew trunk and the same general crown architecture. In either form, branching is concentrated high in the plant: keep the lower roughly 60–64% as a readable trunk, not as a trunk covered with random spurs. The branches in the upper crown must be foliage-bearing throughout; do not use unconstrained random leaf omission or allow a global leaf cap to leave later branches bare.

This models a horticultural display form rather than asserting that every *Ficus benjamina* naturally develops braided stems. Botanical references describe a typical single trunk with fine, pendulous branchlets and dense foliage; spiral and braided stems are cultivated ornamental forms. See the references in Section 13.

**Reference dimensions and bounded parameters**

- Reference height: `H = 2.0 m`; intended canopy spread at maturity is approximately `1.0–1.4 m`, scaled by `widthScale` and maturity.
- Lower trunk length: `0.62H` for braided stems and `0.64H` for the single-leader form.
- Braided strands: `Nbt = clamp(round(3 + 2 * age), 4, 5)`. Use evenly distributed phase offsets, about `2.05 ± 0.17` turns, and a small orbit radius around `0.025–0.029 m` at the 2 m reference height.
- Braid closure: strand centreline orbit radius must approach zero at both ends using `orbit * sin(πu)^0.7`; all strands converge into the shared root collar and upper junction. A central heartwood of roughly `0.013–0.014 m` radius overlaps their inner surfaces. Neighbouring strand surfaces should touch or overlap slightly; they must not look like separate wires floating around an empty cylinder.
- Single-leader spiral: `1.4–1.7` turns across the lower trunk, with an orbit of around `0.044–0.054 m` and a visible basal radius of roughly `0.026 m`, tapering towards the crown. The actual centreline must move laterally through 3D space and converge at the root and crown junctions; rotating a turtle frame around an unchanged axis is not sufficient.
- Crown leader: approximately `0.17H`, starting exactly at the trunk's top endpoint. Use a gently tapering stem around `0.015 m` near its base and around `0.005–0.007 m` towards its tip.
- Primary limbs: 5–7 likely limbs, with 7, 11, or 14 internodes at low, medium, and high detail. Initial pitch should make each limb rise while it spreads away from the leader (roughly `0.70–1.04 rad` in this turtle convention). Increase the pitch along the outer section so the limb curves over and its last four internodes can droop; do not use a nearly horizontal launch angle that creates an umbrella silhouette.
- Secondary limbs: use selected internodes on each primary. Low detail uses two candidate slots; medium uses five slots at `[1, 3, 5, 7, 9]`; high uses five differently spaced slots at `[1, 4, 7, 10, 12]`. Each secondary has 4, 7, or 10 internodes for low, medium, or high detail. Tertiary twigs have 3, 5, or 7 internodes and are more frequent at higher detail. Apply stochastic decisions per node/slot, not one shared random value for the entire parent limb.
- Leaves: primary blades are typically `0.054–0.088 m` long and about `0.39–0.48` of their length in width; smaller secondary and tertiary leaves are also elliptic. Every primary and secondary internode gets two opposing leaf sprays, plus a third leaf on selected nodes. Every tertiary internode receives two leaves, with a third at alternating nodes in high detail. Each twig terminus gets a small cluster. Do not drop a whole node's foliage to save budget.
- Budgets: prune branch creation before leaf generation. Use branch caps of 14, 50, and 60 for Low, Medium, and High detail, with corresponding foliage caps of 260, 1,000, and 1,800. For a mature specimen (age `0.92`, variation `0.45`), use expected leaf-placement counts of roughly 138, 767, and 1,336 for Low, Medium, and High as initial tuning references. Profile the actual implementation on target hardware.

**Axiom — braided ornamental form**

```text
BT(0,Nbt,0.62H,phase(0),turns(0),orbit(0),radius(0))
BT(1,Nbt,0.62H,phase(1),turns(1),orbit(1),radius(1))
...
BT(Nbt-1,Nbt,0.62H,phase(Nbt-1),turns(Nbt-1),orbit(Nbt-1),radius(Nbt-1))
BC(0.62H,coreRadius)
J(0.62H)
CROWN(0,0)
```

Each `BT` is a bounded geometry macro. Strand phase is distributed as `2πj/Nbt` plus small seeded jitter. Use a common envelope `sin(πu)^0.7` so all strand centrelines begin and end on the shared axis. The `BC` core is generated along the same axis and overlaps the strands by a small amount. `J` aligns the grammar turtle to the end of this trunk system so the crown starts where the braided structure ends. No separate strand may stop short of, or float beyond, the common junction.

**Axiom — natural single-leader form**

```text
ST(0.64H,turns,orbit,phase,trunkRadius)
J(0.64H)
CROWN(0,0)
```

`ST` uses a real spatial centreline. One useful generic construction is:

```text
u = sampleIndex / segmentCount
angle(u) = 2π * turns * u + phase
envelope(u) = sin(πu)^0.7
x(u) = orbit * envelope(u) * (cos(angle) + 0.12 sin(2 angle))
z(u) = orbit * envelope(u) * (0.88 sin(angle) + 0.10 sin(2.2 angle))
y(u) = trunkLength * u
radius(u) = trunkRadius * (1 - 0.32u) + smallTipRadius
```

Join consecutive samples with a tapered tube. Since `envelope(0) = envelope(1) = 0`, the trunk meets its root and crown axes cleanly. The asymmetric terms avoid a mechanically perfect spring.

**Crown and branching productions**

```text
CROWN(i,d) -> F(crownLength/Nc, crownRadius(i), crownRadius(i+1), wood)
              [ /(primaryYaw(i)) &(primaryPitch(i)) PRIMARY(0,0) ]
                when i is an upper-crown branch slot and branchBudget allows it
              [ /(terminalYaw(i,j)) &(terminalPitch(i,j)) L("elliptic", length(i,j), width(i,j), petiole, droop, twist) ]
                at the leader's top nodes
              CROWN(i+1,d)
              when i < Nc

CROWN(i,d) -> ε
              when i >= Nc

PRIMARY(i,d) -> F(primaryLength(i), primaryRadius(i), primaryRadius(i+1), wood)
                L("elliptic", leafLength(i,0), leafWidth(i,0), petiole, droop(i,0), twist(i,0))
                L("elliptic", leafLength(i,1), leafWidth(i,1), petiole, droop(i,1), twist(i,1))
                [ thirdLeaf(i) L("elliptic", leafLength(i,2), leafWidth(i,2), petiole, droop(i,2), twist(i,2)) ]
                [ /(secondaryYaw(i)) &(secondaryPitch(i)) SECONDARY(0,d+1) ]
                  when i is a selected secondary slot, d = 0, and branchBudget allows it
                PRIMARY(i+1,d)
                when i < Np

PRIMARY(Np,d) -> terminalLeafCluster ε

SECONDARY(i,d) -> F(secondaryLength(i), secondaryRadius(i), secondaryRadius(i+1), wood)
                  L("elliptic", leafLength(i,0), leafWidth(i,0), petiole, droop(i,0), twist(i,0))
                  L("elliptic", leafLength(i,1), leafWidth(i,1), petiole, droop(i,1), twist(i,1))
                  [ thirdLeaf(i) L("elliptic", leafLength(i,2), leafWidth(i,2), petiole, droop(i,2), twist(i,2)) ]
                  [ /(tertiaryYaw(i)) &(tertiaryPitch(i)) TERTIARY(0,d+1) ]
                    at one internal node, only if d = 1 and branchBudget allows it
                  SECONDARY(i+1,d)
                  when i < Ns

SECONDARY(Ns,d) -> terminalLeafCluster ε

TERTIARY(i,d) -> F(tertiaryLength(i), tertiaryRadius(i), tertiaryRadius(i+1), wood)
                 L("elliptic", leafLength(i,0), leafWidth(i,0), petiole, droop(i,0), twist(i,0))
                 L("elliptic", leafLength(i,1), leafWidth(i,1), petiole, droop(i,1), twist(i,1))
                 [ highDetailThirdLeaf(i) L(...) ]
                 TERTIARY(i+1,d)
                 when i < Nt

TERTIARY(Nt,d) -> terminalLeafCluster ε
```

**Non-negotiable foliage and connection invariants**

1. **Connected trunk:** braided strand centrelines converge into the same root/crown junctions; the central core touches the inner faces of all strands; the first crown internode starts at the shared top junction.
2. **Actual spiral:** `ST` changes the centreline position in X/Z as it rises. A rotation around the turtle's forward axis without lateral displacement does not create a twisted trunk.
3. **No bare twig runs:** every internode of `PRIMARY`, `SECONDARY`, and `TERTIARY` carries two leaf placements in opposing sprays; selected nodes carry a third. Stochastic variation changes leaf size and direction, not whether a node is foliated. Every twig endpoint receives a small terminal cluster.
4. **Budget before detail:** calculate a conservative upper bound for all primary, secondary, and tertiary leaf placements before expansion. Reduce tertiary branching and then secondary branching if the limit would be exceeded; do not stop leaf creation mid-branch. Use separate explicit budgets for branch count, grammar tokens, and terminal foliage modules.
5. **Organic canopy:** use slightly uneven branch lengths, azimuth jitter, asymmetric weeping tips, overlap between leaf clusters, and local leaf-angle variation. Avoid evenly spaced radial spokes and a uniformly solid sphere.
### 7.5 Dwarf umbrella tree — `schefflera_arboricola`

**Architecture:** One or several woody stems with clustered palmate leaf heads. Each compound leaf has multiple elliptic leaflets radiating from a central point, giving the plant a recognisable umbrella-like look.

**Reference parameters**

- Main-axis nodes: 8–16; one to three principal leaders.
- Compound leaves: 12–30 at medium/high quality; fewer when distant.
- Leaflet count: usually 7–9 per compound leaf; permit occasional 6 or 10.
- Leaflet size: 0.07–0.18 m depending on overall scale.

**Axiom**

```text
A(0,0)
```

**Productions**

```text
A(k,d) -> F(mainLength(k), mainRadius(k), mainRadius(k+1), "umbrella-wood")
          [ /(compoundYaw(k)) &(compoundPitch(k))
            U("schefflera-umbrella", leafletCount(k), leafletLength(k), leafletWidth(k), compoundDroop(k)) ]
          [ /(branchYaw(k)) &(branchPitch(k)) B(0,1,k) ]
          A(k+1,d)
          when k < Na
A(k,d) -> ε
          when k >= Na

B(j,d,k) -> F(branchLength(j,d), branchRadius(j,d), branchRadius(j+1,d), "umbrella-wood")
            [ /(compoundYaw(j,d)) &(compoundPitch(j,d)) U("schefflera-umbrella", leafletCount(j), leafletLength(j), leafletWidth(j), compoundDroop(j)) ]
            B(j+1,d,k)
            when j < branchNodeLimit and d <= maxBranchDepth
B(j,d,k) -> ε
            when j >= branchNodeLimit or d > maxBranchDepth
```

The compound leaf is a terminal geometry module; do not rewrite the leaflet fan as a separate tree of branches. Keep fans at different heights, orientations, and stages of openness. The woody framework should be visible between leaf heads, especially near the base.

### 7.6 ZZ plant — `zamioculcas`

**Architecture:** Many fleshy, gently arching stalks emerge independently from the soil line. Each stalk carries rows of glossy elliptic leaflets along a central rachis. The overall form is an irregular clump rather than a woody tree.

**Reference parameters**

- Stalks: 8–16, each with a different target height and lean.
- Leaflets per stalk: 6–12, arranged alternately or in subtly offset pairs.
- Stalk length: 0.35–0.9 m; stalks are thicker at the base and taper upward.
- Leaflets: 0.05–0.14 m, with rounded tips and a substantial, glossy appearance.

**Axiom**

```text
[ /(phase0) &(lean0) Z(0,0) ]
[ /(phase1) &(lean1) Z(0,1) ]
[ /(phase2) &(lean2) Z(0,2) ]
...
[ /(phaseN) &(leanN) Z(0,N) ]
```

`N` is the selected stalk count minus one. Each stalk has a unique phase, lean, height, and internode rhythm.

**Productions**

```text
Z(k,stalk) -> F(stalkSegment(k,stalk), stalkRadius(k,stalk), stalkRadius(k+1,stalk), "zz-rachis")
              [ /(leafYaw(k,stalk)) &(leafPitch(k,stalk))
                L("zz-elliptic", leafletLength(k,stalk), leafletWidth(k,stalk), leafletPetiole(k), leafletDroop(k), leafletTwist(k)) ]
              Z(k+1,stalk)
              when k < leafletNodeCount(stalk)
Z(k,stalk) -> ε
              when k >= leafletNodeCount(stalk)
```

The stalk itself curves outward gradually; it should not be a stack of identical straight segments. Leaves should be attached on alternating sides of the rachis and gently rotate away from one another. Avoid making the whole clump a perfectly symmetric fountain.

## 8. Organ shape library

The following shape names are the required geometry profiles used by the grammars. They describe geometry, not textures, and must be implemented as reusable organ generators.

| Shape key | Required outline/structure | Useful detail |
|---|---|---|
| `kentia-frond` | Curved rachis with many narrow paired leaflets | Drooping outer half; shorter leaflet lengths toward the tip |
| `dracaena-linear` | Long, narrow, pointed blade | Strong lengthwise curve; slight droop; optional muted red edge tint |
| `fiddle` | Broad blade with constricted midsection and wider upper/lower lobes | Thick midrib, gently rippled edge |
| `small-elliptic` | Small oval-to-lanceolate leaf | Pointed tip; modest asymmetry; thin petiole |
| `schefflera-umbrella` | Compound leaf with a short hub and radiating elliptic leaflets | Different leaflet angles/lengths around the hub |
| `zz-elliptic` | Thick, smooth, elliptic leaflet | Rounded tip, subtle convexity, glossy finish |

A small shared outline vocabulary is enough for the first version, provided that the leaf parameterisation supports asymmetry, width-profile changes, curvature, and minor variation. Avoid using a single leaf mesh stretched to fit all shape keys.

## 9. Real-time performance strategy

A lobby can contain several plants in view simultaneously, so performance must be designed into the grammar and renderer rather than optimised only after the fact.

### 9.1 Recommended starting budgets per plant

These are tuning targets, not engine-specific guarantees. A project should profile its actual target hardware and camera distances.

| Quality tier | Leaf/leaflet target | Approximate triangle target | Intended use |
|---|---:|---:|---|
| Low | 40–140 | 1,000–4,000 | Background plants and distant views |
| Medium | 120–400 | 4,000–12,000 | Default lobby placement |
| High | 300–800 | 10,000–25,000 | Close foreground plants or hero shots |

Compound leaves and fronds count by visible leaflet for budgeting purposes, even if emitted through a single `P` or `U` module. These budgets must be reduced for dense scenes or weaker hardware. A branch may be retained as centre-line data while its smallest leaves are omitted at lower LODs.

### 9.2 Generation limits

- Keep plant derivation to a modest, fixed number of passes. The limits in the species definitions normally need fewer than 30 passes.
- Cap branch depth, axis-node count, leaflet count, and total emitted organs independently.
- Cull or skip branches outside the declared plant envelope and branches that would be hidden by a denser inner canopy.
- Estimate organ counts before mesh construction when possible; stop before creating geometry that must immediately be discarded.
- Generate high-quality geometry once, then derive simpler cached LODs from the structural representation or create the target LOD directly using reduced counts.
- Avoid per-leaf scene objects if the engine has substantial object/draw-call overhead. Prefer batched meshes, merged geometry by material, or instancing of shared leaf templates.

### 9.3 LOD behaviour

LOD changes must preserve silhouette before preserving detail. Reduce detail in this order:

1. Remove tiny inner leaves and the finest twigs.
2. Reduce leaflet count along fronds and in compound leaves.
3. Simplify stem cross-sections and leaf triangulation.
4. Reduce surface details, veins, and per-leaf colour variation.
5. Only at the farthest distance, replace the plant with an impostor or a simplified clump mesh if the renderer supports it.

Do not randomly re-seed a plant when its LOD changes. Use the same stable seed and a consistent ordering/priority for organs so that the silhouette remains recognisable and transitions do not look like the plant is rearranging itself.

### 9.4 Sharing and caching

- Cache generated plants by `(archetype, seed, resolved dimensions, quality tier, version)`.
- Reuse leaf mesh templates and shared materials where possible; vary shape parameters or vertex deformation within the supported leaf family.
- For many instances of the same archetype, instance the same base plant or the same organ meshes with stable variation. Use a small set of seeds to avoid obvious duplicates, supplemented by gentle instance-level changes in height, width, and canopy asymmetry.
- Use pooled geometry buffers and defer expensive mesh uploads where the engine supports them.
- If the engine supports background jobs, perform grammar expansion and CPU-side mesh preparation off the render thread. Integrate the finished plant only when its geometry is ready.

## 10. Recommended generation algorithm

The algorithm below is normative at the architectural level; implementations may change data structures as needed.

1. Resolve the archetype's reference parameters against `height`, `widthScale`, `age`, and `quality`.
2. Seed a deterministic random function using the plant seed and stable non-terminal paths.
3. Create the axiom as a compact symbol sequence.
4. Expand non-terminals in parallel, applying guarded productions, until all non-terminals terminate, the definition's pass limit is reached, or a budget guard prunes the remaining buds.
5. Validate bracket balance and ensure that no geometry module has a negative, non-finite, or unreasonably large dimension.
6. Interpret turtle commands to create stem segments, leaf attachment transforms, compound-organ descriptors, and roots.
7. Apply local geometry deformation from each terminal organ's parameters. Keep geometry within the archetype envelope unless an explicitly allowed branch extends it.
8. Merge compatible geometry or instance repeated templates; generate normals and tangents if the renderer requires them.
9. Validate the final bounding box, organ count, triangle count, and material count against the archetype's declared limits.
10. Store the result in the cache and expose a root transform at soil level for placement into a separate planter asset.

The grammar expansion should be pure for a given set of inputs: the same archetype, parameters, and seed produce the same structure and geometry. Any animation or wind deformation should be applied after generation, using a separate low-cost deformation step.

## 11. Validation and visual acceptance criteria

Each archetype should be tested at three camera distances and with at least 8–16 deterministic seeds. A technically valid grammar is not necessarily a visually convincing plant, so use silhouette and organ-placement checks alongside timing and memory measurements.

### Visual checks

- **Silhouette:** The plant is recognisable at thumbnail size and does not collapse into a generic sphere, cone, or bush.
- **Botanical arrangement:** Leaves attach to stems or petioles, fronds attach to a palm crown, and compound leaflets share a hub/rachis.
- **Scale:** Leaf size, branch thickness, total height, and planter footprint are coherent.
- **Asymmetry:** The plant looks grown, not rotationally perfect; major branches and leaf orientations are not repeated mechanically.
- **Overlap:** There are useful gaps between structural layers, but enough foliage density for a mature ornamental plant.
- **Soil boundary:** Stems emerge from the soil line without floating bases, visible gaps, or roots poking outside the planter unless intended.

### Performance checks

- Record generation time separately from steady-state rendering cost.
- Record peak and retained memory, CPU mesh-build time, triangle count, leaf/leaflet count, and draw calls/material batches.
- Test a representative lobby scene containing multiple plants, reflective surfaces, and moving cameras, not only an isolated plant viewer.
- Confirm that the same seed creates the same plant after a scene reload and across quality tiers where shared organs are retained.

A reasonable initial target is that medium-quality plants generate quickly enough to appear during scene loading or asynchronous asset preparation, while steady-state rendering incurs no per-frame grammar work. The actual millisecond threshold should be set from the engine's loading and interaction budgets after profiling.

## 12. Future extensions

Keep these extensions separate from the first version so the base grammar remains small and predictable:

- Seasonal or maturity variants via parameter presets rather than entirely separate grammars.
- Slow wind sway using low-frequency animation on stems and leaf petioles.
- Light-facing leaf bias, used subtly to orient broad leaves away from impossible overlaps.
- Pot-bound root flares and a surface layer of moss, bark, or soil generated by the planter system.
- Additional archetypes: bamboo palm, rubber plant, snake plant, peace lily, or multi-stem ficus.
- Growth-stage derivations if plants later need to grow visibly over time.

## 13. References and morphology sources

These sources support the selected plant architectures and visible traits. They are references for modelling, not a requirement to reproduce any one photographed cultivar exactly.

### Procedural modelling and algorithms

- Prusinkiewicz, Cieslak, Ferraro and Hanan, “Modeling plant development with L-systems” (2018): https://algorithmicbotany.org/papers/modeling-plant-development-with-l-systems.html
- Runions, Lane and Prusinkiewicz, “Modeling Trees with a Space Colonization Algorithm” (2007): https://doi.org/10.2312/NPH/NPH07/063-070
- Reinhardt et al., “Phyllotaxis” (2007): https://doi.org/10.1016/j.tplants.2007.03.004
- “3D modeling and reconstruction of plants and trees: A cross-cutting review across computer graphics, vision, and plant phenotyping” (2022): https://pmc.ncbi.nlm.nih.gov/articles/PMC8987840/
- Deussen et al., “Realistic Modeling and Rendering of Plant Ecosystems” (SIGGRAPH 1998): https://www.graphics.stanford.edu/papers/ecosys/

### Botanical references by archetype

| Archetype | Morphology source |
|---|---|
| Kentia palm — *Howea forsteriana* | [NC State Extension](https://plants.ces.ncsu.edu/plants/howea-forsteriana/) · [RHS](https://www.rhs.org.uk/plants/8845/howea-forsteriana/details) |
| Dracaena marginata | [University of Florida IFAS](https://gardeningsolutions.ifas.ufl.edu/plants/houseplants/dracaena-marginata/) · [RHS Dracaena guide](https://www.rhs.org.uk/plants/dracaena) |
| Fiddle-leaf fig — *Ficus lyrata* | [NC State Extension](https://plants.ces.ncsu.edu/plants/ficus-lyrata/) · [RHS](https://www.rhs.org.uk/plants/7207/ficus-lyrata/details) |
| Weeping fig — *Ficus benjamina* | [Clemson Cooperative Extension](https://hgic.clemson.edu/factsheet/weeping-ficus/) · [University of Florida IFAS](https://ask.ifas.ufl.edu/publication/st251) |
| Dwarf umbrella tree — *Heptapleurum arboricola* (syn. *Schefflera arboricola*) | [NC State Extension](https://plants.ces.ncsu.edu/plants/heptapleurum-arboricola/) · [Kew Plants of the World Online](https://powo.science.kew.org/) |
| ZZ plant — *Zamioculcas zamiifolia* | [Kew Plants of the World Online](https://powo.science.kew.org/taxon/urn:lsid:ipni.org:names:89402-1/general-information) · [RHS](https://www.rhs.org.uk/plants/search-results?query=Zamioculcas%20zamiifolia) |

Photographic examples for visual review are maintained in the separate photo-audit HTML. The audit page is supporting reference material; this document alone defines the normative build rules. If a reference page changes its URL, search the scientific name on the named botanical institution's site.

---

**Normative rule:** Use the stage order in Section 2, species grammars in Section 7, organ definitions in Sections 4 and 8, budgets in Sections 3 and 9, and acceptance criteria in Section 11 as the controlling build definition.
