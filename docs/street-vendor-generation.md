# Rule-Based Procedural Generation of Street Vendors, Noodle Bars, Container Shops, Market Stalls and Barrows

**Status:** Proposed high-level design, prototyped in a browser toy (`Vendor Grammar`)

## 1. Purpose

Street vendors fill the gaps between buildings. They are small, crowded, brightly lit and seen at close range, which makes them the opposite of the tower grammar: a tower is read as a silhouette from a distance, while a vendor is read as a handful of objects, a sign and a glowing interior from three or four metres away.

The goal is to generate many small stalls at runtime, such as a canopy noodle bar with a counter and stools, a hatch kiosk, a lean-to against a wall, a converted shipping container with a serving hatch, a traditional trestle market stall under a striped cloth, or a hand barrow with a hood or parasol. They should look stocked and lived in. Two vendors may share a structure, but their stock, dressing and signage must differ.

Two constraints shape the design:

- **Poly budget.** A street may hold dozens of vendors. Each must cost on the order of a thousand triangles and a handful of draw calls, yet still feel dense. The document therefore treats *faking* detail (section 9) as a first-class part of the grammar, not an afterthought.
- **Uniqueness.** Dressing, not structure, is what makes a vendor feel different. Section 8 defines a fingerprint and a similarity limit so neighbours cannot be near-copies.

The look is inspired by the noodle bars and pop-up shops of dense, rainy, neon-lit cities: barrel or flat canopies, counters with stools, steam, hanging lanterns, hand-lettered signs, cluttered shelves. No specific film sign, logo or brand is reproduced. The sign script is an invented pseudo-glyph alphabet.

This is a separate document from the building grammar because the problems differ: small footprint, close viewing distance, an interior that must read through an opening, and an explicit budget model. It reuses the same method (typed parts, bounded quantities, deterministic seeds, reject/simplify/retry), and the same conventions for the log and for districts.

## 2. Structure of the grammar

A vendor is generated in five stages. Each stage only reads what earlier stages have written.

1. **Archetype and trade.** Pick a structure type and a trade (what is sold), weighted by an affinity table.
2. **Structure.** Build the shell: body, roof or canopy, counter, openings, supports.
3. **Dressing.** Choose palette, finish, feature wall, signature piece and decorations, and check them against neighbours.
4. **Fill.** Place counter props, hanging items, figures, signs and steam emitters into the free slots the structure left.
5. **Budget and validation.** Measure the result, simplify if over budget, validate against the bounded rules, and retry if needed.

### 2.1 Archetypes

| Archetype | Description | Key parts |
|---|---|---|
| Canopy noodle bar | A freestanding barrel or flat canopy on pillars over a counter, often a U shape, with stools | Pillars, roof slab with lip, counter, 2–6 stools, hood and flue |
| Hatch kiosk | A small box with one serving hatch | Walls, hatch opening, hinged flap with struts, shelf, roof sign |
| Lean-to stall | Corrugated roof leaning against a host wall, open at the front, side cloths | Roof sheet, two posts, rear wall, side cloths, counter |
| Container shop | A 20 ft or 40 ft shipping container with openings cut in a long wall | Container shell, 1–3 openings, flap or cloth awning, deck, corner posts |
| Container pair | A container plus a perpendicular 10 or 20 ft container, with a covered tarp court between them | Two shells, tarp roof, stools or benches in the court |
| Trestle market stall | A traditional open stall: poles, a cloth or tarp canopy, a trestle table with tiered display, and a painted back room | 4+ poles, scalloped stripe, tarp or parasol canopy, rear cloth, skirted or open table, 1–3 display tiers, optional side cloths and hang rail |
| Hand barrow | A wheeled cart pushed by its handles, either open, with a rail, under a parasol, or with a back cabinet and hood | Bed, two wheels, handles, a variant (open, rail, parasol, hood), optional back cabinet with painted shelves, vendor at the handles |

A trade's affinity table decides which archetypes it favours. A noodle bar is six times more likely as a canopy bar than as a lean-to, while a hardware stall prefers lean-tos and containers. Market stalls and barrows have low base weights in the automatic archetype mix (about 0.2 and 0.16 beside the older archetypes), so they appear as an occasional traditional vendor among the shacks. Adding them changes the automatic archetype choice for a given seed, so a seed saved before this change may now give a different archetype when archetype is left on Auto. Seeds with the archetype fixed are unaffected by the weights.

### 2.2 Trades

Nineteen trades are defined. The original thirteen are noodle bar, skewer grill, dumpling steamers, seafood, produce and herbs, apothecary, electronics, records and print, cloth and tailoring, curios, drinks stall, hardware and bio-plants. Six traditional market trades were added with the market stall and barrow archetypes: greengrocer, flower seller, baker, roast chestnuts (a cooking trade with a charcoal roaster prop), butcher and second-hand books. The original trades also gained market and barrow affinities, so a curios dealer or a drinks seller can now appear on a barrow, and the greengrocer favours stalls and barrows over containers.

Each trade carries:

- **Shelf stock**: a weighted list of painted stock items (bowl stacks, bottles, jars, steamer towers, tanks, fish racks, drawers, CRT screens, parts bins, record sleeves, cloth bolts, masks, clocks, tools, plants and so on). About twenty painters exist.
- **Counter props**: real low-poly objects on the counter (pot, bowl, grill, steamer, scale, crate, radio and others).
- **Hanging items**: lanterns, dried goods, ducks, cables, garments, bunting, tools.
- **Neon motifs**: serpent, bowl, fish, cup, bottle, eye, lotus, sunburst, weighted by trade.
- **Lamp colour**: the interior light that tints the whole stall.
- **Cooking level**: 0 (none), or 2 (full cooking, which enables steam, a hood and a flue).

## 3. Building graph

Each vendor carries a small typed graph, the same idea as the tower grammar's. Nodes are elements (`shell`, `roof`, `counter`, `pillar`, `flap`, `stool`, `sign`, `prop` and so on). Edges say how they relate: `supports`, `mounted_on`, `attached_to`, `open_into`, `seats_at`. The graph is not drawn. It is used to place things (a hanging lantern must hang from a rail that exists) and to explain a result in the validation log.

## 4. Bounded rules

Every quantity that could go wrong has a hard bound. These are the values the prototype uses.

| Rule | Bound |
|---|---|
| Unbraced canopy, roof sheet or tarp span | ≤ 4.5 m |
| Cantilever beyond the last support | ≤ 1.2 m |
| Hinged flap depth (with struts) | ≤ 1.5 m |
| Cloth awning depth (with posts) | ≤ 2.0 m |
| Clear height where people stand | ≥ 2.3 m |
| Hanging item clearance above the footway | ≥ 1.95 m |
| Counter height | 0.95–1.1 m |
| Counter depth | 0.5–0.8 m |
| Stool pitch | ≥ 0.62 m |
| Blade sign projection | ≤ 1.0 m, clearance ≥ 2.2 m |
| Roof sign area | ≤ 2.5 m² |
| Flue height above roof | ≥ 0.6 m |
| Gas bottles | ≤ 2 |
| Container openings (total) | ≤ 80% of the wall length |
| Solid corner post left at each end | ≥ 0.6 m |
| Opening sill / head rail | sill ≥ 0.9 m, head rail ≥ 0.3 m |
| Container deck height | 0.9–1.5 m |
| Covered tarp court | ≤ 4.5 m across |
| Site depth | 8.0 m, minus a 1.5 m footway, so footprint depth ≤ 6.5 m |
| Gap between neighbours | ≥ 1.0 m |
| Steam emitters per vendor | ≤ 3 |
| Market table height | 0.8–1.0 m |
| Market table depth | 0.7–1.1 m |
| Market canopy clear height at the front | ≥ 2.1 m |
| Market pole spacing | ≤ 4.5 m (extra poles are added beyond that) |
| Canopy pitch (market, barrow hood) | ≥ 0.25 m fall so rain sheds |
| Parasol radius | ≤ 1.6 m, rim clearance ≥ 2.0 m |
| Barrow bed | 1.2–2.0 m long, 0.7–1.0 m wide, 0.65–0.9 m high |
| Barrow wheel radius | 0.3–0.45 m, wheel top no more than 0.15 m above the bed |
| Barrow handles | 0.6–1.1 m long |
| Barrow overall length (with handles) | ≤ 3.2 m |
| Barrow hood or rail clearance | ≥ 1.9 m for the hood lip, rail at 2.2–2.4 m |

Container sizes use real dimensions (20 ft is 6.058 m, 40 ft is 12.192 m, 2.438 m wide, 2.591 m high). A shipping container is a strong object: cutting an opening removes structure, so the grammar keeps corner posts and a head rail rather than treating the wall as paper.

## 5. Structure rules

**Canopy noodle bar.** The roof span is split into bays so that no bay exceeds 4.5 m. Pillars go at the ends and at bay joints. The counter forms a straight run or a U, with stools spaced at the stool pitch along the customer side. A hood and flue appear above any cooking prop and must rise at least 0.6 m above the roof.

**Hatch kiosk.** The hatch is cut with a sill at counter height. A hinged flap opens upward and is held by struts. It may not exceed 1.5 m deep.

**Lean-to.** A roof sheet leans against a host wall. It is only legal if the host wall exists and the span is under 4.5 m. Side cloths hang from the roof edge to the ground or stop at a rail.

**Container shop.** Openings are planned along the long wall with the corner post, sill and head rail rules above. Each opening gets either a hinged flap or a cloth awning, and may get a deck in front of it.

**Container pair.** A second container is placed at right angles. A tarp roof spans the court between them, but only if the court width is within the span limit. If not, the generator narrows the court or falls back to a single container.

**Trestle market stall.** Poles stand at the corners, with extra poles so no bay exceeds 4.5 m. The canopy is one of three kinds: a scalloped stripe cloth, a plain tarp, or a parasol. The front lip must clear 2.1 m and the canopy has a fall of at least 0.25 m. A table 0.8–1.0 m high and 0.7–1.1 m deep carries 1–3 display tiers, either skirted with cloth or an open trestle. Behind the table a rear cloth closes the stall, and the painted impostor room shows through it as the "back wall" (tarp, shelves, crates or boards). Optional side cloths and a hang rail on the canopy front hold bunting and hanging goods. A cooking trade may add a stove pipe that rises at least 0.6 m above the canopy.

**Hand barrow.** The bed (1.2–2.0 m by 0.7–1.0 m) sits 0.65–0.9 m up on two wheels, and the handles stick out behind, keeping the whole vehicle within 3.2 m. Four variants:

- *Hood.* A back cabinet with a painted shelf impostor and a scalloped awning above it, with a lip at least 1.9 m up. A cooking trade also gets a flue. For cooking trades the hood is chosen far more often than the open or parasol variants.
- *Parasol.* A parasol of radius up to 1.6 m, rim at least 2.0 m up, with or without a small cabinet.
- *Rail.* A hanging rail at 2.2–2.4 m on two uprights, for garments, books or dried goods.
- *Open.* A bare bed with a sign pole and an optional cabinet.

The vendor figure stands at the handles. A barrow has no blade sign and no window, since it has no wall to hold them. Its sign is a pole or hood sign.

## 6. Interiors without interior geometry

A vendor is mostly opening: a counter, a hatch, a dark interior. Real interior geometry is expensive, so the interior is faked with a **painted room seen through each opening**.

Each opening gets a quad. (For a market stall the "opening" is the gap behind the table. For a barrow it is the back cabinet, and an open or parasol barrow without a cabinet has no impostor at all.) A shader ray-traces a virtual room box behind it against a single 1024×512 canvas atlas holding:

- the back wall (shelves, stock, the feature wall),
- the side wall,
- the floor and ceiling (the ceiling carries a strip-light patch),
- a **far card**: a flat cut-out with the prep table and the vendor,
- a **near card**: a flat cut-out with hanging items and a bulb.

The cards are alpha cut-outs placed at fixed depths inside the room, so moving the camera produces real parallax between back wall, cards and front opening. The result reads as a deep, cluttered interior from any angle that sees into the opening, for the cost of one quad and one 2 MB texture. Emissive pixels (neon, screens, lamp) are detected by brightness and glow, and the lamp light falls off across the room.

The limits are honest ones: a painted room cannot be walked into, it breaks if the camera passes through the opening, and a very oblique view shows the stretching of a box shader. The toy includes a **Depth fake** toggle that flattens it, so the saving can be compared.

## 7. Stock, props and decoration

Each stall has three layers of content, from cheapest to most expensive:

1. **Painted stock** in the atlas: shelves of jars, bottles, boxes, steamers, sleeves, screens, masks. Stocked by trade and painted per vendor from its palette, so no two stalls share a texture.
2. **Real props** on the counter and in front: pots, bowls, crates, scales, radios, bottles. These are small low-poly meshes that give depth and parallax at the front, where the eye lands first.
3. **Hanging items** on a rail: lantern, dried fish, cable, garment, bunting. Thin geometry that sways in the vertex shader.

**Market and barrow stock.** Loose stock is the main idea of these archetypes, so it is handled in two cheap ways. First, new real props: fruit and veg boxes, buckets, loaves, a charcoal roaster, butcher's blocks and book stacks. Second, **heaps**: small octahedra and boxes scattered over each table tier and barrow bed in the colours of the trade (apples and oranges for a greengrocer, blooms for a flower seller, loaves for a baker, cuts for a butcher, spines for books). A heap is a few triangles, not a modelled object. It is capped by tier area, and it avoids any real prop's footprint so nothing interpenetrates. A separate **back-wall painter** draws the market room behind the table (tarp, shelves, crates or boards), and new painted stock covers fruit, veg, flowers, loaves, joints and books. New hanging kinds (`bunch` for dried herbs and flowers, `joint` for hung meat) go on the canopy rail.

Decorations are placed in sized **slots** (wall, ground, roof). A decoration only fits if the slot is at least its footprint, so a 0.7 m menu board is rejected from a 0.4 m gap. About 25 decorations are defined, such as posters, stickers, speakers, cameras, extractor fans, pipes, AC units, crates, barrels, bags, coolers, benches, bins, gas bottles, roof strings, antennae, water tanks, lantern strings, dishes and vent fans.

Each vendor also gets one **signature piece**: lucky-cat statue, incense shrine, hanging fish rack, parked cargo bike, hanging umbrellas, painted mural, half-dead neon, wall of screens, strings of pennants, birdcage, roof planters or inflatable mascot. This is the object a player remembers.

## 8. Uniqueness

Structure is shared; dressing carries identity. A vendor's **fingerprint** is nine axes:

`trade`, `palette`, `layout`, `shelf mix`, `feature wall`, `neon motif`, `signature piece`, `finish`, `decorations`

When a vendor is generated beside others (the `ctx` list), its fingerprint is compared with each neighbour's. The rules:

- A neighbour may match on **at most 3** of the 9 axes.
- If a vendor is too similar, its dressing is **re-rolled up to 4 times**.
- If it still matches, a **palette or signature change is forced**.
- The signature piece must differ from every neighbour's. This one is absolute.

Twelve palettes (body, trim, two accent colours and two neon colours each) mean even identical structures read as different. Wall finishes (painted steel, corrugated, tile, plank, brick, rust) are applied in the fragment shader, not as textures. The Street row view in the toy generates five vendors in one pass with a shared `ctx`, so you can check this by choosing one archetype and a single trade.

## 9. Poly-budget techniques

| Technique | What it replaces | Cost |
|---|---|---|
| **Interior-mapping impostor** (section 6) | A modelled interior | 1 quad per opening, one 1024×512 atlas |
| **Alpha cut-out cards** | Modelled vendor, hanging items, prep table | Flat, painted, inside the impostor |
| **Painted stock** | Hundreds of small shelf objects | Pixels in the atlas |
| **Baked lighting in vertex colour** | Real-time lights per vendor | CPU bake from each vendor's lamp and neon list; no light uniforms in the shell shader |
| **Fragment-shader surface patterns** | Texture sets and normal maps | Corrugation, tile, plank and brick procedurally; grime and wet sheen added |
| **Vertex-shader sway** | Animated meshes | Cloth, lanterns and the mascot move with a sine in the vertex shader |
| **Additive neon quads** | Tube geometry, emissive meshes | One quad per sign, painted to canvas, with flicker modes (steady, buzz, dying, pulse) |
| **Ground light pools** | Per-vendor ground decals and shadows | A ground shader takes up to 32 light pools and a puddle mask for wet reflection |
| **GPU-simulated steam** | CPU particles | Section 10 |
| **Single batched shell** | Many draw calls | One mesh per vendor: position, normal, albedo, baked light, aux (sway, emissive, pattern) |
| **Budget tiers** | One quality for all | Lean, Standard, Hero |

### 9.1 Budget tiers

| Tier | Triangles | Steam particles | Hanging | Neon | Figures | Props | Decorations |
|---|---|---|---|---|---|---|---|
| Lean (distant) | 800 | 48 | 5 | 3 | 1 | 8 | 2 |
| Standard | 1700 | 100 | 12 | 5 | 3 | 16 | 5 |
| Hero (close-up) | 2600 | 180 | 20 | 7 | 5 | 26 | 8 |

Draw calls are capped at 8 per vendor. Texture memory is accounted at about 2 MB per interior atlas and 2 MB per sign atlas. In the prototype a standard vendor measured between about 450 and 1700 triangles and 3 to 5 draw calls, depending on archetype. Over 100 seeds the new archetypes at the standard tier gave a median of about 1,100 triangles for a market stall (maximum about 1,630) and about 1,000 for a barrow (maximum about 1,340). Both are cheap because the interior is painted and the bulk of the stock is heaps.

### 9.2 Reduction order

If a vendor is over its triangle budget, the generator removes content in a fixed order of least importance and logs each step: drop a decoration, drop a patron, remove two hanging items, remove a counter item, lower the detail level, drop a stool, drop the signature (but never the half-dead neon), remove all hanging items. The tier is chosen by the caller from the camera distance, not by the vendor.

## 10. Steam and smoke

Steam is the single most atmospheric thing on a stall, and the cheapest if it is done on the GPU.

### 10.1 Emitter descriptor

The grammar does not simulate particles. It emits a small **descriptor per source**, which any renderer can turn into particles:

```
emitter {
  pos        : vec3      // world position of the source
  kind       : steam | smoke | vent | incense | mist
  count      : int       // particles, from the tier budget
  life       : s         // 2.4 steam, 3.2 smoke, 3.6 vent
  size0,size1: m         // start and end radius
  rise       : m/s       // initial upward speed, slowing with age
  spread     : m         // lateral drift per life
  turbulence : m         // sinusoidal wander
  opacity    : 0..1      // peak alpha; fades in then out
  ceiling    : y         // height at which the plume is clamped and spreads sideways
  tint       : rgb       // sampled from nearby neon and lamp lights
}
```

Sources are chosen by the grammar: one emitter per cooking prop (a pot or steamer gives `steam`, a grill or a chestnut roaster gives `smoke`), one per flue (`vent`, or `smoke` for a grill), a rising thread for an incense shrine, a low `mist` over a seafood ice tray. At most three per vendor, and the tier's particle count is shared among them in proportion to a weight.

### 10.2 Per-kind values used by the prototype

| Kind | Life (s) | Start/end size (m) | Rise (m/s) | Peak opacity |
|---|---|---|---|---|
| steam | 2.4 | 0.11 → 0.6 | 0.55 | 0.55 |
| smoke | 3.2 | 0.12 → 0.85 | 0.7 | 0.42 |
| vent | 3.6 | 0.2 → 1.1 | 1.2 | 0.5 |
| incense | 3.2 | 0.035 → 0.2 | 0.45 | 0.38 |
| mist | 2.8 | 0.15 → 0.5 | 0.12 | 0.26 |

### 10.3 GPU simulation

No particle state is stored or updated on the CPU. Each particle holds a seed, its life and its source parameters. The vertex shader works out where it is from time alone:

```
phase  = fract(time / life + seed)          // 0..1 through its life
height = rise * phase * life * (1 - 0.35*phase)
offset = lateral drift + turbulence + wind
if height passes ceiling: clamp y and push the excess sideways
size   = mix(size0, size1, phase)
alpha  = opacity * fadeIn(phase) * (1 - phase)^1.4
```

Each particle is an **instanced camera-facing quad**, not a point sprite, because some GPU back ends clamp point size to a few dozen pixels, which breaks a large soft puff. Blending is alpha over, drawn after opaque geometry and neon with depth test on and depth write off, so steam is hidden by a counter but does not hide the sign behind it. The tint is baked once per emitter from nearby lights so a puff beside a pink sign turns pink; in daylight it is near-white.

The prototype's steam is a few hundred quads per street in total; one instanced draw call covers every vendor.

### 10.4 Mapping to the game engine's steam generator

I have not seen the engine's steam generator or its API, so this section is a mapping plan, not a verified integration. The descriptor above is deliberately engine-agnostic. The likely steps are:

1. Treat each descriptor as a **call to the existing generator**: position from `pos`, a preset chosen by `kind`.
2. Map `life`, `size0/size1`, `rise`, `spread`, `turbulence` and `opacity` onto whichever corresponding parameters the generator exposes. Where it exposes fewer, drop `turbulence` first, then `spread`.
3. If the generator is CPU-driven and per-emitter cost is high, use the tier's `count` as a cap, and prefer one emitter per flue and per cooking prop rather than one per vendor.
4. Apply `tint` by modulating the generator's colour, or by a per-emitter light-sample if it samples scene lights itself.
5. Keep the `ceiling` rule: stall steam that pours through a canopy looks wrong, so clamp it or let it spread beneath.

If the engine's generator already runs on the GPU, the tier budget and ceiling rule are the only parts that need adding. If you can share its interface or parameter list, I can tighten this mapping to exact calls.

## 11. Rain, ground and atmosphere

These are not part of the grammar but are needed to judge it. The prototype's rain is GPU lines driven by time. The ground is a tile texture with a puddle mask and up to 32 light pools drawn from every vendor's lamp and neon, producing the coloured pools beneath a stall. The backdrop is a brick alley with lit windows, pipes and a kerb. Post-processing is MSAA, a bright-pass and blur bloom, vignette and grain. All of it can be switched off from the panel.

## 12. Determinism and retry

Every random decision comes from a stream keyed by `seed`, a name and an attempt number, so a seed always gives the same vendor, and changing one rule's attempt number does not disturb unrelated parts.

The generator retries at three levels:

- **Level 0.** Full richness. Place everything the trade and tier allow, with extra counter props.
- **Level 1.** A simpler pass with fewer counter props.
- **Level 2.** The simplest pass, which keeps only what the bounded rules require.

In the prototype, 2,250 generations (5 archetypes × 3 tiers × 150 seeds) all passed validation with no retries after the opening planner was fixed. After the market stall and barrow were added, 100 seeds of each at all three tiers gave no validation errors. At the lean tier a few generations (2 per 100) needed the level 1 retry to fit the 800 triangle cap, which is what the retry path is for; standard and hero needed none. The retry path exists for edits to the rules, not for ordinary use.

A vendor that passes validation at its level is accepted. A vendor that fails is logged with the exact failed rule (for example `footprint is 6.7 m deep, leaving under 1.5 m of footway`) and retried at the next level. The log is the validation log shown in the toy, in the same reject / simplify / ok / info vocabulary as the tower grammar.

## 13. Simplified algorithm sketch

```
generateVendor(seed, archetype?, trade?, tier, neighbours):
  for level in 0..2:
    pick trade and archetype by affinity (unless fixed)
    build structure for archetype        // shell, roof, counter, openings
    dress(seed, neighbours):             // up to 4 re-rolls, then force change
      choose palette, finish, feature wall, signature, decorations
      reject if fingerprint matches a neighbour on more than 3 of 9 axes
    place props, hangings, figures       // into free slots, within bounds
    place signs, steam emitters
    bake lights into vertex colour
    measure triangles and draws
    reduce in the fixed order until within tier budget
    validate bounded rules
    if no errors: return vendor with its log
  return the level 2 result with its errors logged
```

## 14. Notes for the NeonCity and voxel engine

- **Impostor interiors.** If the engine ray-marches voxels, a stall's interior could be real voxels at hero range and the painted room at distance. The grammar is the same either way: the opening, the room box and the slot list already exist.
- **Descriptors, not meshes.** The vendor is a plain data object (elements, openings, lights, steam emitters, signs). The browser builder is one consumer; an engine-side builder can consume the same data.
- **Particles from the grammar.** Emitter descriptors let the grammar drive the engine's own steam generator, so smoke and steam stay consistent with the rest of the city.
- **Lighting.** The baked vertex light stands in for a handful of real lights per stall. In an engine with proper light culling, the same `lights` list can feed real lights at hero range.
- **Mobile vendors.** A barrow is the one archetype with no host structure, so it can be placed by any street logic (kerb, plaza, a market lane) and since it is a wheeled object the engine could animate it, though the prototype does not move it.
- **Streets.** Rows of vendors need only the gap, the footway and the neighbour fingerprints. The toy's Street row view is a small version of this.

## 15. Expected result

A street can hold dozens of vendors that read as different businesses, each at roughly a thousand triangles, a few draw calls and one or two small textures. Interiors look deep and cluttered because they are painted rooms; signs glow and flicker because they are additive quads; steam is a single instanced draw. Every vendor can be reproduced from its seed, and every rejection explains itself.

What the prototype has not shown (and, for the market stall and barrow, what was only checked in a handful of screenshots, not every seed and every variant): behaviour on real GPUs (it was checked with a software renderer), frame times for a full street, and how the painted interiors hold up at very oblique angles or if the camera enters an opening. Those need a test in the real engine.
