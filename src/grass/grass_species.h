#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "grass_rules.h"

/* The grass's species profiles (GR3, docs/procedural_grass_rendering_implementation.md
 * 5.4): one asset file each, assets/grass/<name>.json, <name> GRASS_SPECIES_NAMES[k],
 * read at run time. JSON with comments; every key is required:
 *
 *   {
 *     "density":     150,          // effective blades per m^2 where every rule says yes
 *     "root_grid":   48,           // candidate roots per patch side (GRASS_PATCH_M)
 *     "tuft":        [5, 9],       // blades per tuft, fewest and most (uniform between)
 *     "tuft_radius": 0.06,         // how far a tuft's blades stand from its root, at most (m)
 *     "height":      [0.35, 0.65], // a root's blade height, least and most (m), before its clump and the ground scale it
 *     "width":       0.020,        // a blade's width at its base, a share of its height
 *     "lean":        0.25,         // its lean from the ground's normal, typical (rad)
 *     "bend":        0.35,         // the rest bend of its tip over the lean, typical (a share of its height)
 *     "curl":        0.6,          // where the bend gathers: 0 evenly up the blade .. 1 all near the tip
 *     "taper":       0.8,          // the width's taper exponent p: w(t) = w0 (1 - t)^p
 *     "segments":    [5, 3, 2],    // the ribbon's segments in each bucket (full, mid, clump), 1 .. GRASS_SEG_MAX
 *     "clumps": {
 *       "cells": 3,                // clump cells per patch side
 *       "bare":  0.08,             // the chance a clump is a bare spot
 *       "vary":  0.30,             // how much clumps differ: density x (1 - vary u), height x (1 + vary (2u - 1))
 *       "edge":  0.55              // how far the roots thin towards a clump's border (1 - edge x edgeness)
 *     },
 *     "stiffness":   1.0,          // against the wind (GR5)
 *     "interaction": 1.0,          // how far a passer-by pushes it (GR8)
 *     "colour": {                  // linear albedo
 *       "root": [r, g, b], "tip": [r, g, b],
 *       "dry":  [r, g, b],         // the driest clump's colour (tip; the root darkens as the lush one's)
 *       "dry_share": 0.3           // how far towards dry the driest clump goes, 0 .. 1
 *     },
 *     "fullness":    0.35,         // the normal bent across the blade's width (shading only, spec 5.5)
 *     "light": {                   // how it takes the light (GR7, spec 10.1-10.3: shaders/grass/blade.frag)
 *       "root_shade":   0.35,      // darker into the tuft: albedo x (1 - root_shade (1 - t)^2), t 0 at the root
 *       "transmission": 0.35,      // the light through a blade from behind it, a share of what its face would take
 *       "rough":        0.55       // its face's roughness dry (GGX's, alpha its square); wet, smoother
 *     }
 *   }
 *
 * A species whose file is missing or wrong is left out (no root of it is
 * kept) and the loader says so. grass.c's rules read the table, and the GPU's
 * copy (shaders/grass/rules.glsl) reads it from the frame's uniforms
 * (grass_profiles_gpu: FrameUBO.grassSpecies). */

typedef struct GrassProfile {
    int   present;                  /* its file was read */
    float density;
    int   rootGrid;
    int   tuft[2];
    float tuftRadius;
    float height[2];
    float width, lean, bend, curl, taper;
    int   segments[GRASS_BUCKETS];
    int   clumpCells;
    float bare, clumpVary, clumpEdge;
    float stiffness, interaction;
    float root[3], tip[3], dry[3];
    float dryShare, fullness;
    float rootShade, transmission, rough;
} GrassProfile;

/* A profile as the shaders see it (std140; common.glsl GrassSpecies). */
typedef struct GrassSpeciesGPU {
    float   a[4];     /* density (blades / m^2), tuft radius, height least, most */
    float   b[4];     /* width, lean, bend, bare chance */
    float   c[4];     /* clump vary, clump edge, taper, curl */
    int32_t n[4];     /* root grid, tuft fewest, most, clump cells */
    int32_t seg[4];   /* segments full, mid, clump; present */
    float   root[4];  /* rgb, stiffness */
    float   tip[4];   /* rgb, interaction */
    float   dry[4];   /* rgb, dry share */
    float   misc[4];  /* fullness; the field's cover a blade, upright and lying (grass.h grass_field, filled by grass_field_gpu); 0 */
    float   field[4]; /* rgb the field's albedo from afar, seen through little of it (grass_field_gpu); 0 */
    float   fieldTop[4]; /* rgb seen through much of it (its top), w the scale of tau it goes over in */
    float   light[4]; /* root shade, transmission, dry roughness; 0 */
} GrassSpeciesGPU;
_Static_assert(sizeof(GrassSpeciesGPU) == 12 * 16, "GrassSpecies's std140 layout");

extern const char *const GRASS_SPECIES_NAMES[GRASS_SPECIES];

/* Reads one profile. On error (unreadable, malformed, a key missing, unknown
 * or out of range) returns false with the reason in err. */
bool grass_profile_load(const char *path, GrassProfile *p, char *err, size_t errSize);
/* Reads every species' profile from dir (dir/<name>.json) into the table the
 * rules read, saying which couldn't be; returns how many were read. */
int  grass_profiles_load(const char *dir);
/* The same from the first of the asset roots (src/core/assets.h) holding a
 * grass/ directory, or from root if it isn't NULL. */
int  grass_profiles_load_assets(const char *root);
/* Species k's profile (present 0 if it wasn't read). */
const GrassProfile *grass_profile(int k);
/* The table for the shaders. */
void grass_profiles_gpu(GrassSpeciesGPU out[GRASS_SPECIES]);
/* The most segments any species read asks of bucket b (1 if none). */
int  grass_bucket_segments(int b);
