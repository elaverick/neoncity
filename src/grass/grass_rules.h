#ifndef GRASS_RULES_H
#define GRASS_RULES_H

/* The grass's rules (src/grass/grass.c, shaders/grass/rules.glsl): where
 * grass grows, how thick, of which species, in which clumps and tufts. The
 * constants both copies read, as #defines in syntax C and GLSL both accept
 * (as terrain_clip.h). A *_LIST is one value per species in species order:
 * C braces it ({ GRASS_TARGET_LIST }), GLSL wraps it (float[](GRASS_TARGET_LIST)).
 * Species profiles move to asset files in GR3; these are their defaults.
 *
 * Identity. The world is cut into GRASS_PATCH_M squares, patch (px, pz)
 * covering x in [px, px + 1) GRASS_PATCH_M and the same in z, in the
 * terrain's frame (the clipmap's: level 0's samples lie on whole metres, so a
 * patch is 8 x 8 of them), never the camera's: grass doesn't crawl. Each
 * species cuts a patch into GRASS_GRID_LIST cells a side, one candidate root
 * in each, anywhere in it (stratified: the root grid leaves no peak in the
 * roots' spectrum), named by the cell's world index (patch * grid + i, j).
 * A clump is a cell of a coarser grid of GRASS_CLUMP_LIST a side per patch,
 * its seed jittered in it; a point belongs to its nearest seed (Voronoi).
 *
 * Density. A root of species k is kept where
 *     its hash < w_k        (the weighted choice of species: grass_species)
 *     another   < s d_k c   (retention, spec 4.4)
 * with s the suitability there, c its clump's density there (in [0, 1],
 * mean GRASS_CLUMP_MEAN(k) over the ground) and d_k the species' density
 * scale, chosen so that the effective density is
 *     blades / m^2 = GRASS_TARGET_LIST[k] s w_k
 * that is d_k = target / (roots per m^2 x mean blades per tuft x clump mean).
 * Every factor is at most 1, so nothing clamps and the formula holds exactly
 * on average (grass_test measures it). */

#define GRASS_PATCH_M    8.0          /* a patch's side (m): whole metres, so level 0's samples tile it */
#define GRASS_SPECIES    4
#define GRASS_MEADOW     0            /* the land's grass */
#define GRASS_REEDS      1            /* by water and in mud, taller and sparser */
#define GRASS_LAWN       2            /* the city's parks (GR6) */
#define GRASS_ROUGH      3            /* the city's fields and edge verge (GR6) */

/* Effective blades per m^2 where every rule says yes (GR0's targets). */
#define GRASS_TARGET_LIST      150.0, 40.0, 250.0, 120.0
/* Root cells per patch side: 0.167, 0.286, 0.125 and 0.167 m apart. */
#define GRASS_GRID_LIST        48, 28, 64, 48
/* Blades per tuft, fewest and most (uniform between). */
#define GRASS_TUFT_MIN_LIST    5, 6, 4, 5
#define GRASS_TUFT_MAX_LIST    9, 10, 6, 7
/* How far a tuft's blades stand from its root, at most (m). */
#define GRASS_TUFT_R_LIST      0.06, 0.10, 0.03, 0.05
/* A root's blade height, least and most (m), before its clump and the ground scale it. */
#define GRASS_HEIGHT_MIN_LIST  0.35, 0.80, 0.08, 0.25
#define GRASS_HEIGHT_MAX_LIST  0.65, 1.50, 0.14, 0.45
/* A blade's width at its base as a share of its height. */
#define GRASS_WIDTH_LIST       0.020, 0.012, 0.040, 0.025
/* Lean from the normal (rad) and the rest bend of the tip (a share of the height), typical. */
#define GRASS_LEAN_LIST        0.25, 0.10, 0.15, 0.35
#define GRASS_BEND_LIST        0.35, 0.20, 0.25, 0.45

/* Clumps: cells per patch side (2.67, 2, 4 and 2.67 m); the chance a clump is
 * a bare spot (its density then GRASS_BARE_KEEP); how much clumps differ in
 * height and density (a clump's factor is 1 - var u, u uniform in [0, 1)
 * for density, 1 + var (2u - 1) for height); and how far the roots thin
 * towards a clump's border with the next (1 - edge x edgeness, below). A
 * mown lawn is nearly even. */
#define GRASS_CLUMP_LIST       3, 4, 2, 3
#define GRASS_BARE_LIST        0.08, 0.15, 0.0, 0.12
#define GRASS_CLUMP_VAR_LIST   0.30, 0.35, 0.08, 0.35
#define GRASS_CLUMP_EDGE_LIST  0.55, 0.60, 0.10, 0.50
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

/* Levels of detail by a blade's projected height (px, GR4). */
#define GRASS_LOD_FULL_PX      12.0
#define GRASS_LOD_MID_PX       4.0
#define GRASS_LOD_CLUMP_PX     1.5

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
#define GRASS_BLADE_VERTS      6            /* GR2's stand-in: a flat strip, two triangles */
/* GrassFrame.info.z: the prepare pass's modes (grass_test). */
#define GRASS_MODE_ALL_IN_VIEW 1            /* no frustum test */
#define GRASS_MODE_ALL_FULL    2            /* every root full, every blade: the count test */
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
#define GRASS_C_STATS          24
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

#endif
