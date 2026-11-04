#include "grass.h"
#include "grass_species.h"
#include "hash.h"
#include <math.h>

/* The grass's rules (grass.h, grass_rules.h). Every step is float arithmetic,
 * in the order shaders/grass/rules.glsl takes it: change one, change both.
 * Each species' numbers are its profile (grass_species.h); a species left out
 * keeps no root. */

#define SP(k) grass_profile(k)
#define TARGET(k)     (SP(k)->present ? SP(k)->density : 0.0f)
#define GRID(k)       (SP(k)->present ? SP(k)->rootGrid : 1)
#define TUFT_MIN(k)   SP(k)->tuft[0]
#define TUFT_MAX(k)   SP(k)->tuft[1]
#define TUFT_R(k)     SP(k)->tuftRadius
#define HEIGHT_MIN(k) SP(k)->height[0]
#define HEIGHT_MAX(k) SP(k)->height[1]
#define WIDTH(k)      SP(k)->width
#define LEAN(k)       SP(k)->lean
#define BEND(k)       SP(k)->bend
#define CLUMP(k)      (SP(k)->present ? SP(k)->clumpCells : 1)
#define BARE(k)       SP(k)->bare
#define CLUMP_VAR(k)  SP(k)->clumpVary
#define CLUMP_EDGE(k) SP(k)->clumpEdge

static const float TWO_PI = 6.28318531f;

static float sat(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
/* A smoothstep from e0 to e1, either way round (rules.glsl grassEase). */
static float ease(float e0, float e1, float x) {
    float t = sat((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

/* A root's or clump's stream of draws (hash.h's Rng, as rules.glsl grassDraw). */
static uint32_t draw(uint32_t *state) { *state = hash_u32(*state + 0x6D2B79F5U); return *state; }
static float drawf(uint32_t *state) { return hash_float(draw(state)); }

float grass_suitability(const GrassSite *s) {
    float base = s->city ? s->grass : s->grass + (float)GRASS_SUIT_MUD * s->mud;
    float v = fminf(base, 1.0f);
    v *= ease((float)GRASS_SLOPE_NONE, (float)GRASS_SLOPE_FULL, s->ny);
    v *= 1.0f - ease((float)GRASS_SNOW_FROM, (float)GRASS_SNOW_NONE, s->snow);
    if (!s->city) v *= 1.0f - ease((float)GRASS_URBAN_FROM, (float)GRASS_URBAN_NONE, s->urban);
    v *= ease((float)GRASS_ROAD_NONE, (float)GRASS_ROAD_FULL, s->road);
    return sat(v);
}

void grass_species(const GrassSite *s, float w[GRASS_SPECIES]) {
    for (int k = 0; k < GRASS_SPECIES; ++k) w[k] = 0.0f;
    if (s->city) {
        float lawn = sat(s->maintained);
        w[GRASS_LAWN] = lawn;
        w[GRASS_ROUGH] = 1.0f - lawn;
        return;
    }
    float mudShare = s->mud / fmaxf(s->grass + s->mud, 1e-4f);
    float reeds = sat((float)GRASS_REEDS_MUD * mudShare + (float)GRASS_REEDS_WET * ease((float)GRASS_REEDS_WET0, (float)GRASS_REEDS_WET1, s->wet));
    w[GRASS_REEDS] = reeds;
    w[GRASS_MEADOW] = 1.0f - reeds;
}

float grass_expected(const GrassSite *s) {
    float w[GRASS_SPECIES], suit = grass_suitability(s), sum = 0.0f;
    grass_species(s, w);
    for (int k = 0; k < GRASS_SPECIES; ++k) sum += TARGET(k) * suit * w[k];
    return sum;
}

float grass_target(int k) { return TARGET(k); }
int   grass_grid(int k) { return GRID(k); }
float grass_roots_m2(int k) { float n = (float)GRID(k) / (float)GRASS_PATCH_M; return n * n; }
float grass_tuft_mean(int k) { return 0.5f * (float)(TUFT_MIN(k) + TUFT_MAX(k)); }
float grass_clump_mean(int k) {
    return (1.0f - BARE(k) * (1.0f - (float)GRASS_BARE_KEEP)) * (1.0f - 0.5f * CLUMP_VAR(k)) * (1.0f - CLUMP_EDGE(k) * (float)GRASS_EDGE_MEAN);
}
float grass_density_scale(int k) { return !SP(k)->present ? 0.0f : TARGET(k) /(grass_roots_m2(k) * grass_tuft_mean(k) * grass_clump_mean(k)); }

/* The clumps' warp (grass_rules.h GRASS_CLUMP_WARP): a value noise over the
 * world's clump cells (gx, gz the point's; fx, fz where in it), a node every
 * four cells, in cells, either way of 0. */
static float warp(int k, int32_t gx, int32_t gz, float fx, float fz, uint32_t salt) {
    int32_t lx = gx >> 2, lz = gz >> 2;   /* arithmetic shifts: rounding down, as GLSL's */
    float tx = ((float)(gx & 3) + fx) * 0.25f, tz = ((float)(gz & 3) + fz) * 0.25f;
    tx = tx * tx * (3.0f - 2.0f * tx);
    tz = tz * tz * (3.0f - 2.0f * tz);
    float a = hash_float(hash3i(salt, lx, lz, k)), b = hash_float(hash3i(salt, lx + 1, lz, k));
    float c = hash_float(hash3i(salt, lx, lz + 1, k)), d = hash_float(hash3i(salt, lx + 1, lz + 1, k));
    float ab = a + (b - a) * tx, cd = c + (d - c) * tx;
    return (float)GRASS_CLUMP_WARP * (ab + (cd - ab) * tz - 0.5f);
}

void grass_clump(int k, int32_t px, int32_t pz, float lx, float lz, GrassClump *c) {
    int n = CLUMP(k);
    float cs = (float)GRASS_PATCH_M / (float)n;
    /* in cells: the point's cell (relative to the patch's corner and in the
     * world) and where in it, then warped */
    float qx = lx / cs, qz = lz / cs;
    int bx = (int)floorf(qx), bz = (int)floorf(qz);
    int32_t gx = px * n + bx, gz = pz * n + bz;
    float fx = qx - (float)bx, fz = qz - (float)bz;
    float ux = fx + warp(k, gx, gz, fx, fz, GRASS_SALT_WARP);
    float uz = fz + warp(k, gx, gz, fx, fz, GRASS_SALT_WARP + 1u);
    int cx = (int)floorf(ux), cz = (int)floorf(uz);
    float f1 = 1e9f, f2 = 1e9f;
    uint32_t best = 0;
    for (int dj = -1; dj <= 1; ++dj)
        for (int di = -1; di <= 1; ++di) {
            int i = cx + di, j = cz + dj;   /* relative to the point's cell */
            uint32_t h = hash3i(GRASS_SALT_CLUMP, gx + i, gz + j, k), st = h;
            float jx = drawf(&st), jz = drawf(&st);
            float sx = (float)i + 0.5f + (float)GRASS_CLUMP_JITTER * (jx - 0.5f);
            float sz = (float)j + 0.5f + (float)GRASS_CLUMP_JITTER * (jz - 0.5f);
            float dx = sx - ux, dz = sz - uz, d = sqrtf(dx * dx + dz * dz);
            if (d < f1) { f2 = f1; f1 = d; best = h; }
            else if (d < f2) f2 = d;
        }
    uint32_t st = best;
    (void)draw(&st); (void)draw(&st);   /* past the seed's jitter */
    float uBare = drawf(&st), uDens = drawf(&st), uHeight = drawf(&st), uLean = drawf(&st), uStiff = drawf(&st), uTint = drawf(&st);
    c->id = best;
    c->edge = 1.0f - ease(0.0f, (float)GRASS_EDGE_BAND, f2 - f1);
    c->bare = uBare < BARE(k);
    c->density = (1.0f - CLUMP_VAR(k) * uDens) * (1.0f - CLUMP_EDGE(k) * c->edge) * (c->bare ? (float)GRASS_BARE_KEEP : 1.0f);
    c->height = 1.0f + CLUMP_VAR(k) * (2.0f * uHeight - 1.0f);
    c->lean = TWO_PI * uLean;
    c->stiffness = 0.7f + 0.6f * uStiff;
    c->tint = uTint - 0.5f;
}

void grass_candidate(int k, int32_t px, int32_t pz, int i, int j, GrassCandidate *c) {
    int n = GRID(k);
    float cs = (float)GRASS_PATCH_M / (float)n;
    c->id = hash3i(GRASS_SALT_ROOT, px * n + i, pz * n + j, k);
    c->state = rng_make(c->id).state;
    float jx = drawf(&c->state), jz = drawf(&c->state);
    c->px = px; c->pz = pz; c->species = k;
    c->x = ((float)i + jx) * cs;   /* anywhere in its cell: stratified */
    c->z = ((float)j + jz) * cs;
}

int grass_keep(const GrassCandidate *c, const GrassSite *s, GrassRoot *r) {
    int k = c->species;
    uint32_t st = c->state;
    float uSpecies = drawf(&st), uKeep = drawf(&st), uHeight = drawf(&st);
    uint32_t uTuft = draw(&st);
    float w[GRASS_SPECIES];
    grass_species(s, w);
    if (!(uSpecies < w[k])) return 0;
    float suit = grass_suitability(s);
    if (!(suit > 0.0f)) return 0;
    GrassClump cl;
    grass_clump(k, c->px, c->pz, c->x, c->z, &cl);
    if (!(uKeep < suit * grass_density_scale(k) * cl.density)) return 0;
    r->at = *c;
    r->clump = cl;
    r->height = (HEIGHT_MIN(k) + (HEIGHT_MAX(k) - HEIGHT_MIN(k)) * uHeight) * cl.height * ((float)GRASS_SHORT + (1.0f - (float)GRASS_SHORT) * suit);
    r->blades = TUFT_MIN(k) + (int)(uTuft % (uint32_t)(TUFT_MAX(k) - TUFT_MIN(k) + 1));
    return 1;
}

void grass_blade(const GrassRoot *r, int b, GrassBlade *o) {
    int k = r->at.species;
    uint32_t st = rng_make(hash_combine(r->at.id ^ GRASS_SALT_BLADE, (uint32_t)b)).state;
    float u1 = drawf(&st), u2 = drawf(&st), u3 = drawf(&st), u4 = drawf(&st), u5 = drawf(&st);
    float u6 = drawf(&st), u7 = drawf(&st), u8 = drawf(&st), u9 = drawf(&st), u10 = drawf(&st);
    float rr = sqrtf(u1), a = TWO_PI * u2;   /* uniform over the tuft's disc */
    float ox = cosf(a), oz = sinf(a);
    o->x = r->at.x + TUFT_R(k) * rr * ox;
    o->z = r->at.z + TUFT_R(k) * rr * oz;
    o->height = r->height * (0.75f + 0.5f * u3) * (1.0f - 0.25f * rr);   /* the outer blades shorter */
    o->width = o->height * WIDTH(k) * (0.8f + 0.4f * u4);
    o->facing = TWO_PI * u5;
    /* leaning the clump's way, splayed out from the tuft's middle, and a little at random */
    float g = TWO_PI * u6;
    float lx = 0.6f * cosf(r->clump.lean) + rr * ox + 0.4f * cosf(g);
    float lz = 0.6f * sinf(r->clump.lean) + rr * oz + 0.4f * sinf(g);
    float ll = sqrtf(lx * lx + lz * lz);
    o->leanX = ll > 1e-4f ? lx / ll : 1.0f;
    o->leanZ = ll > 1e-4f ? lz / ll : 0.0f;
    o->lean = LEAN(k) * (0.5f + u7);
    o->bend = BEND(k) * (0.6f + 0.8f * u8);
    o->phase = u9;
    o->tint = 0.7f * r->clump.tint + 0.3f * (u10 - 0.5f);
    o->seed = st;
}
