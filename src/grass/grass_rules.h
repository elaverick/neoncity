#ifndef GRASS_RULES_H
#define GRASS_RULES_H

/* The grass's rules (src/grass/grass.c, shaders/grass/rules.glsl): where
 * grass grows, how thick, of which species, in which clumps and tufts. The
 * constants both copies read, as #defines in syntax C and GLSL both accept
 * (as terrain_clip.h). Each species' own numbers are its profile, an asset
 * file (assets/grass/<name>.json, src/grass/grass_species.h), which C reads
 * and the GPU sees in the frame's uniforms (FrameUBO.grassSpecies).
 *
 * Identity. The world is cut into GRASS_PATCH_M squares, patch (px, pz)
 * covering x in [px, px + 1) GRASS_PATCH_M and the same in z, in the
 * terrain's frame (the clipmap's: level 0's samples lie on whole metres, so a
 * patch is 8 x 8 of them), never the camera's: grass doesn't crawl. Each
 * species cuts a patch into its profile's root_grid cells a side, one candidate root
 * in each, anywhere in it (stratified: the root grid leaves no peak in the
 * roots' spectrum), named by the cell's world index (patch * grid + i, j).
 * A clump is a cell of a coarser grid of its clumps' cells a side per patch,
 * its seed jittered in it; a point belongs to its nearest seed (Voronoi).
 *
 * Density. A root of species k is kept where
 *     its hash < w_k        (the weighted choice of species: grass_species)
 *     another   < s d_k c   (retention, spec 4.4)
 * with s the suitability there, c its clump's density there (in [0, 1],
 * mean GRASS_CLUMP_MEAN(k) over the ground) and d_k the species' density
 * scale, chosen so that the effective density is
 *     blades / m^2 = target_k s w_k   (the profile's "density")
 * that is d_k = target / (roots per m^2 x mean blades per tuft x clump mean).
 * Every factor is at most 1, so nothing clamps and the formula holds exactly
 * on average (grass_test measures it). */

#define GRASS_PATCH_M    8.0          /* a patch's side (m): whole metres, so level 0's samples tile it */
#define GRASS_SPECIES    4            /* in this order; each profile's file is its name (grass_species.h) */
#define GRASS_MEADOW     0            /* the land's grass */
#define GRASS_REEDS      1            /* by water and in mud, taller and sparser */
#define GRASS_LAWN       2            /* the city's parks (GR6) */
#define GRASS_ROUGH      3            /* the city's fields and edge verge (GR6) */

/* Clumps (each profile's "clumps": cells per patch side, the chance a clump is
 * a bare spot, how much clumps differ in height and density, how far the
 * roots thin towards a clump's border): a bare spot keeps GRASS_BARE_KEEP of
 * its roots. */
#define GRASS_BARE_KEEP        0.1
/* A clump's seed lies anywhere in its cell (1: as Worley noise's). One seed
 * to a cell still leaves the borders' mean a little periodic with the cells
 * (a peak in the roots' spectrum at the clumps' grid), so the point a clump
 * is looked up for is first moved by a smooth noise over the cells (a node
 * every four: its slope stays under 1, so it doesn't fold the clumps), up to GRASS_CLUMP_WARP / 2 cells either way: the clumps'
 * outlines are no longer the cells' and the lattice leaves no trace. */
#define GRASS_CLUMP_JITTER     1.0
#define GRASS_CLUMP_WARP       2.0
/* Edgeness: 1 on the border between two clumps, 0 from this share of a cell in
 * (F2 - F1, the gap between the nearest seed's distance and the next's). */
#define GRASS_EDGE_BAND        0.35
/* The mean edgeness over the ground (it doesn't depend on the cell's size):
 * measured by grass_test, which fails if the rules drift from it. */
#define GRASS_EDGE_MEAN        0.3872

/* Suitability (grass_suitability): the cover's grass, and mud at this weight
 * (the reeds' ground); thinning on slopes from ny FROM to none by FULL;
 * under the snow from SNOW_FROM to none by SNOW_FULL; into the city's ground
 * (plan_urban, 1 where the city draws its own) from URBAN_FROM to none by
 * URBAN_FULL; and none on a road or its gravel: grass from ROAD_FROM beyond
 * the paved edge, all of it by ROAD_FULL (the gravel runs 0.8 m +- 0.4 m on,
 * then gives way over 0.5 m: shaders/terrain/roads.glsl). */
#define GRASS_SUIT_MUD         0.7
#define GRASS_SLOPE_FULL       0.82     /* ny: full under about 35 degrees... */
#define GRASS_SLOPE_NONE       0.70     /* ...none from about 46 */
#define GRASS_SNOW_FROM        0.15
#define GRASS_SNOW_NONE        0.60
#define GRASS_URBAN_FROM       0.998    /* about 40 m beyond the city's built edge (plan_urban_weight)... */
#define GRASS_URBAN_NONE       1.0      /* ...none where the city's ground is */
#define GRASS_ROAD_NONE        1.2
#define GRASS_ROAD_FULL        2.0
#define GRASS_ROAD_FAR         1.0e4    /* no road near */

/* Species (grass_species): reeds where the ground is mud (by its share of the
 * grass and mud) and right by the water (the cover's wet, from REEDS_WET0 to
 * all of it by REEDS_WET1), meadow on the rest of the land; in the city lawn
 * where it is maintained, rough where not. */
#define GRASS_REEDS_MUD        1.5
#define GRASS_REEDS_WET0       0.80
#define GRASS_REEDS_WET1       0.95
#define GRASS_REEDS_WET        0.7

/* A root's blades are shorter on poor ground: x (SHORT + (1 - SHORT) s). */
#define GRASS_SHORT            0.75

/* Levels of detail (GR4: grass_lod, shaders/grass/lod.glsl), by a root's
 * height on screen, px = H f_y / distance:
 *   above FULL_PX x BAND   full: every blade of its tuft;
 *   FULL_PX .. x BAND      still full, its blades but the first ceil(n/2)
 *                          narrowing away and those widening to keep the
 *                          cover, its ribbon morphing to the mid one's shape;
 *   MID_PX x BAND .. FULL  mid: ceil(n/2) blades, widened n / ceil(n/2);
 *   MID_PX .. x BAND       still mid, down to one blade, and the roots not
 *                          in one cell of four (both indices even) narrowing
 *                          away, the ones that are widening for them;
 *   below MID_PX           clump: one blade a root in four, as wide as their
 *                          blades together (at most WIDE_MAX of its height).
 * A blade fades by narrowing, not by a dither: at every hand-off it is far
 * under a pixel wide, so the samples it covers go with its width. Segments
 * change by a geomorph (blade.vert), so no shape pops. Then the ground takes
 * over (grass/field.glsl): every blade of species k narrows to nothing as its
 * typical root (grass_typical: not the root's own height, so the ground can
 * know it) goes from END_PX to CLUMP_PX on screen, and as the distance goes
 * from RADIUS_FADE of the radius to it; the ground under them takes the
 * field's colour by the cover they leave. */
#define GRASS_LOD_FULL_PX      12.0
#define GRASS_LOD_MID_PX       4.0
#define GRASS_LOD_CLUMP_PX     1.5
#define GRASS_LOD_BAND         1.5
#define GRASS_LOD_END_PX       2.5
#define GRASS_RADIUS_FADE      0.85
#define GRASS_WIDE_MAX         0.8
#define GRASS_MORPH_STEPS      15           /* the morph in the instance: 4 bits */
/* The field from afar (grass.h grass_field, grass/field.glsl): its blades in
 * layers by height to TOP x the species' middle height, and its colour seen
 * through as much as TAU_FAR of it, the top of the shaders' fit. */
#define GRASS_FIELD_LAYERS     12
#define GRASS_FIELD_TOP        1.6
#define GRASS_FIELD_TAU_FAR    12.0

/* The GPU path (src/render/grass_gpu.c, shaders/grass/prepare.comp). Blades
 * grow within GRASS_RADIUS of the camera, in a square of GRASS_PATCH_GRID
 * patches a side round its patch (one workgroup each, GRASS_WG threads). Each
 * kept root goes to a bucket by its level of detail (GRASS_LOD_*: by its
 * height's size on screen at its distance, so turning doesn't change it):
 * full, all its tuft's blades; mid, the first half of them; clump, its first
 * blade, and only for roots in one cell in four (both cell indices even).
 * Each bucket has GRASS_CAP_* instances of room, its own, so a far bucket
 * never takes a near one's; past that its far blades give way: the finish
 * pass counts the blades each bucket wanted by distance (GRASS_BINS bins of
 * GRASS_BIN_M) and, once they overflow the room, sets the distance the next
 * frame keeps them to so they fit in GRASS_CAP_FILL of it. */
#define GRASS_RADIUS           200.0
#define GRASS_PATCH_GRID       52           /* 2 * ceil(GRASS_RADIUS / GRASS_PATCH_M) + 2 */
#define GRASS_WG               128
#define GRASS_BUCKETS          3
#define GRASS_LOD_FULL         0
#define GRASS_LOD_MID          1
#define GRASS_LOD_CLUMP        2
#define GRASS_CAP_FULL         196608u      /* meadow at walking height wants about 120k */
#define GRASS_CAP_MID          524288u
#define GRASS_CAP_CLUMP        262144u
#define GRASS_CAP_FILL         0.9
#define GRASS_BINS             128
#define GRASS_BIN_M            2.0          /* GRASS_BINS * GRASS_BIN_M >= GRASS_RADIUS + its height */
#define GRASS_INSTANCE_WORDS   8            /* a blade instance: 32 bytes (shaders/grass/instance.glsl) */
/* A blade is a ribbon of s segments (a profile's "segments", per bucket, 1 ..
 * GRASS_SEG_MAX): 2 s + 1 vertices, a pair a row from the root and one at the
 * tip, and 6 s - 3 indices (two triangles a segment, one at the tip). The
 * index buffer holds every topology, s's from GRASS_SEG_FIRST(s); a bucket
 * draws the most segments any species asks of it, and a species asking fewer
 * folds its spare rows into its tip. */
#define GRASS_SEG_MAX          7
#define GRASS_SEG_FIRST(s)     (3 * ((s) - 1) * ((s) - 1))   /* the sum of 6 j - 3 for j < s */
#define GRASS_SEG_INDICES      GRASS_SEG_FIRST(GRASS_SEG_MAX + 1)
#define GRASS_DRAW_WORDS       5            /* a VkDrawIndexedIndirectCommand */
/* The city's grass (GR6): its parks', fields' and edge verge's ground, as
 * the chunks draw it, cut along the patches (src/city/grass_city.c). Each
 * frame the renderer hands the GPU the pieces of the patches within
 * GRASS_RADIUS (src/render/chunks.c chunks_city_grass), a record a patch of
 * a chunk, of at most GRASS_CITY_RUN triangles (a longer run is split), and
 * shaders/grass/prepare_city.comp grows lawn and rough on them, a workgroup a
 * record, from the same root cells, rules and levels of detail as the land's. */
#define GRASS_CITY_PATCHES     8192         /* records a frame */
#define GRASS_CITY_TRIS        65536        /* their triangles */
#define GRASS_CITY_RUN         32
/* A blade's draw chunk (its chunk's record in the view's draws: its light
 * table and origin), in the instance's own word beside it; none on the land. */
#define GRASS_CHUNK_NONE       0xFFFFFFFFu
/* GrassFrame.info.z: the prepare pass's modes (grass_test). */
#define GRASS_MODE_ALL_IN_VIEW 1            /* no frustum test */
#define GRASS_MODE_ALL_FULL    2            /* every root full, every blade, none narrowed: the count test */
/* The counters (GrassCounters, src/render/grass_gpu.h), words: what grass_test and the
 * --shot log read back, then the bins. */
#define GRASS_C_PATCHES_RANGE  0            /* in range and big enough on screen */
#define GRASS_C_PATCHES_VIEW   1            /* ...and in the view */
#define GRASS_C_PATCHES_GRASS  2            /* ...where grass may grow */
#define GRASS_C_CANDIDATES     3            /* candidate roots in those patches */
#define GRASS_C_EVALUATED      4            /* ...whose rules were evaluated */
#define GRASS_C_KEPT           5            /* ...kept, with a bucket */
#define GRASS_C_WANT           6            /* + bucket: blades reserved (within the distance kept to) */
#define GRASS_C_CUT            9            /* + bucket: blades past the distance kept to */
#define GRASS_C_DRAWN          12           /* + bucket: instances drawn (finish) */
#define GRASS_C_OVER           15           /* + bucket: reserved past the room (finish) */
#define GRASS_C_KEEP_TO        18           /* + bucket: the distance the next frame keeps to (float bits, finish) */
#define GRASS_C_CITY_RANGE     21           /* the city's patch records in range and big enough on screen */
#define GRASS_C_CITY_VIEW      22           /* ...and in the view */
#define GRASS_C_CITY_KEPT      23           /* the city's roots kept (in GRASS_C_KEPT too) */
#define GRASS_C_SP_EVAL        24           /* + species: its candidates evaluated (their cells' ground) */
#define GRASS_C_SP_BLADES      28           /* + species: the blades of its roots kept there, every one (as if all were full) */
#define GRASS_C_STATS          32
#define GRASS_C_HIST           32           /* + bucket * GRASS_BINS + bin: blades wanted by distance */
#define GRASS_C_WORDS          (GRASS_C_HIST + GRASS_BUCKETS * GRASS_BINS)
/* The patch states (--grass-debug cull). */
#define GRASS_PATCH_OUT        0            /* out of range, or too small on screen */
#define GRASS_PATCH_HIDDEN     1            /* out of view */
#define GRASS_PATCH_BARE       2            /* nothing may grow there */
#define GRASS_PATCH_NONE       3            /* evaluated, no root kept */
#define GRASS_PATCH_DRAWN      4

/* Hash salts. */
#define GRASS_SALT_ROOT        0x6A55E1u
#define GRASS_SALT_CLUMP       0x6C0B95u
#define GRASS_SALT_BLADE       0x6B1ADEu
#define GRASS_SALT_WARP        0x6A3B70u   /* and the next, for the warp's z */

/* --grass-debug views (FrameUBO.grass.z): the rules painted on the land. */
#define GRASS_DEBUG_OFF        0
#define GRASS_DEBUG_SUIT       1
#define GRASS_DEBUG_DENSITY    2
#define GRASS_DEBUG_SPECIES    3
#define GRASS_DEBUG_CLUMP      4
#define GRASS_DEBUG_LOD        5            /* blades by bucket; the land by a meadow root's bucket there */
#define GRASS_DEBUG_CULL       6            /* blades by patch; the land by its patch's state */
#define GRASS_DEBUG_WIND       7            /* blades and land by the wind's strength (shaders/grass/wind.glsl) */
#define GRASS_DEBUG_WAKE       8            /* blades and land by the player's wake (shaders/grass/wake.glsl) */
#define GRASS_DEBUG_ALBEDO     9            /* blades, the land and the city's ground by their albedo, unlit and unhazed (GR9) */
#define GRASS_DEBUG_ALBEDO_GAIN 6.0         /* that view's albedo x this (the grass's is about 0.04) */

/* The player's wake (GR8; src/grass/grass_wake.c keeps the trail, shaders/grass/wake.glsl
 * bends the blades by it, FrameUBO.wake carries it). The trail is the feet now and the
 * points laid behind them, GRASS_WAKE_STEP apart; a blade within GRASS_WAKE_RADIUS of the
 * path bends away from it by (1 - r^2)^2 and recovers as a damped spring released when the
 * feet passed, e^(-l a) (cos w a + l / w sin w a) at age a (spec 9.1-9.2). */
#define GRASS_WAKE_POINTS      32           /* the feet and 31 laid points, newest first */
#define GRASS_WAKE_STEP        0.8          /* m between laid points (a chord on a walking turn is 3 cm off the arc) */
#define GRASS_WAKE_RADIUS      0.6          /* m from the path: past it a blade is untouched */
#define GRASS_WAKE_DAMP        0.9          /* 1/s: the recovery's damping (1% left after about 5 s) */
#define GRASS_WAKE_OMEGA       2.0          /* rad/s: its spring (it swings back past upright by about a quarter at 1.6 s) */
#define GRASS_WAKE_LIFE        6.0          /* s: a laid point fades out over the last 1.5 s of this, then goes */
#define GRASS_WAKE_BEND        0.85         /* the tip's push at the path, a share of the height, at interaction 1 */
#define GRASS_WAKE_JUMP        5.0          /* m: feet this far from the newest point start a new trail (a teleport, a landing) */

#endif
