#ifndef GRASS_RULES_GLSL
#define GRASS_RULES_GLSL
#include "grass_rules.h"

/* The grass's rules (src/grass/grass_rules.h): src/grass/grass.c's functions,
 * step for step in the same float arithmetic, so the GPU's grass is the C
 * reference's (grass_test --gpu checks it). Needs common.glsl's hashing
 * (hash_u32, hash_combine, hash3i, hfloat) and the frame's uniforms, where
 * the species' profiles are (frame.grassSpecies: src/grass/grass_species.h; a
 * species left out has density 0, so keeps no root). Positions are in a
 * patch's own frame: metres from its corner, each in [0, GRASS_PATCH_M). */

#define GRASS_SP(k)         frame.grassSpecies[k]
#define GRASS_PRESENT(k)    (GRASS_SP(k).seg.w != 0)
#define GRASS_TARGET(k)     GRASS_SP(k).a.x
#define GRASS_GRID(k)       GRASS_SP(k).n.x
#define GRASS_TUFT_MIN(k)   GRASS_SP(k).n.y
#define GRASS_TUFT_MAX(k)   GRASS_SP(k).n.z
#define GRASS_TUFT_R(k)     GRASS_SP(k).a.y
#define GRASS_HEIGHT_MIN(k) GRASS_SP(k).a.z
#define GRASS_HEIGHT_MAX(k) GRASS_SP(k).a.w
#define GRASS_WIDTH(k)      GRASS_SP(k).b.x
#define GRASS_LEAN(k)       GRASS_SP(k).b.y
#define GRASS_BEND(k)       GRASS_SP(k).b.z
#define GRASS_CLUMP(k)      GRASS_SP(k).n.w
#define GRASS_BARE(k)       GRASS_SP(k).b.w
#define GRASS_CLUMP_VAR(k)  GRASS_SP(k).c.x
#define GRASS_CLUMP_EDGE(k) GRASS_SP(k).c.y
const float GRASS_TWO_PI = 6.28318531;

// grass.h GrassSite
struct GrassSite { float grass; float mud; float snow; float wet; float ny; float urban; float road; float maintained; int city; };

float grassSat(float x) { return clamp(x, 0.0, 1.0); }
// A smoothstep from e0 to e1, either way round (GLSL's smoothstep is undefined for e0 > e1).
float grassEase(float e0, float e1, float x) {
    float t = grassSat((x - e0) / (e1 - e0));
    return t * t * (3.0 - 2.0 * t);
}

// A root's or clump's stream of draws (src/core/hash.h Rng).
uint grassRng(uint seed) { return hash_u32(seed ^ 0xA511E9B3u); }
uint grassDraw(inout uint state) { state = hash_u32(state + 0x6D2B79F5u); return state; }
float grassDrawf(inout uint state) { return hfloat(grassDraw(state)); }

float grassSuitability(GrassSite s) {
    float base = s.city != 0 ? s.grass : s.grass + float(GRASS_SUIT_MUD) * s.mud;
    float v = min(base, 1.0);
    v *= grassEase(GRASS_SLOPE_NONE, GRASS_SLOPE_FULL, s.ny);
    v *= 1.0 - grassEase(GRASS_SNOW_FROM, GRASS_SNOW_NONE, s.snow);
    if (s.city == 0) v *= 1.0 - grassEase(GRASS_URBAN_FROM, GRASS_URBAN_NONE, s.urban);
    v *= grassEase(GRASS_ROAD_NONE, GRASS_ROAD_FULL, s.road);
    return grassSat(v);
}

void grassSpecies(GrassSite s, out float w[GRASS_SPECIES]) {
    for (int k = 0; k < GRASS_SPECIES; ++k) w[k] = 0.0;
    if (s.city != 0) {
        float lawn = grassSat(s.maintained);
        w[GRASS_LAWN] = lawn;
        w[GRASS_ROUGH] = 1.0 - lawn;
        return;
    }
    float mudShare = s.mud / max(s.grass + s.mud, 1e-4);
    float reeds = grassSat(float(GRASS_REEDS_MUD) * mudShare + float(GRASS_REEDS_WET) * grassEase(GRASS_REEDS_WET0, GRASS_REEDS_WET1, s.wet));
    w[GRASS_REEDS] = reeds;
    w[GRASS_MEADOW] = 1.0 - reeds;
}

float grassExpected(GrassSite s) {
    float w[GRASS_SPECIES];
    float suit = grassSuitability(s), sum = 0.0;
    grassSpecies(s, w);
    for (int k = 0; k < GRASS_SPECIES; ++k) sum += GRASS_TARGET(k) * suit * w[k];
    return sum;
}

float grassRootsM2(int k) { float n = float(GRASS_GRID(k)) / float(GRASS_PATCH_M); return n * n; }
float grassTuftMean(int k) { return 0.5 * float(GRASS_TUFT_MIN(k) + GRASS_TUFT_MAX(k)); }
float grassClumpMean(int k) {
    return (1.0 - GRASS_BARE(k) * (1.0 - float(GRASS_BARE_KEEP))) * (1.0 - 0.5 * GRASS_CLUMP_VAR(k)) * (1.0 - GRASS_CLUMP_EDGE(k) * float(GRASS_EDGE_MEAN));
}
float grassDensityScale(int k) { return GRASS_TARGET(k) / (grassRootsM2(k) * grassTuftMean(k) * grassClumpMean(k)); }

// grass.h GrassClump
struct GrassClump { uint id; float edge; float density; float height; float lean; float stiffness; float tint; bool bare; };

// The clumps' warp (grass_rules.h GRASS_CLUMP_WARP): a value noise over the
// world's clump cells (g, the point's; f, where in it), a node every four
// cells, in cells, either way of 0.
float grassWarp(int k, ivec2 g, vec2 f, uint salt) {
    int lx = g.x >> 2, lz = g.y >> 2;
    float tx = (float(g.x & 3) + f.x) * 0.25, tz = (float(g.y & 3) + f.y) * 0.25;
    tx = tx * tx * (3.0 - 2.0 * tx);
    tz = tz * tz * (3.0 - 2.0 * tz);
    float a = hfloat(hash3i(salt, lx, lz, k)), b = hfloat(hash3i(salt, lx + 1, lz, k));
    float c = hfloat(hash3i(salt, lx, lz + 1, k)), d = hfloat(hash3i(salt, lx + 1, lz + 1, k));
    float ab = a + (b - a) * tx, cd = c + (d - c) * tx;
    return float(GRASS_CLUMP_WARP) * (ab + (cd - ab) * tz - 0.5);
}

GrassClump grassClump(int k, ivec2 gpatch, vec2 l) {
    int n = GRASS_CLUMP(k);
    float cs = float(GRASS_PATCH_M) / float(n);
    // in cells: the point's cell (relative to the patch's corner and in the
    // world) and where in it, then warped
    float qx = l.x / cs, qz = l.y / cs;
    int bx = int(floor(qx)), bz = int(floor(qz));
    ivec2 g = ivec2(gpatch.x * n + bx, gpatch.y * n + bz);
    vec2 f = vec2(qx - float(bx), qz - float(bz));
    float ux = f.x + grassWarp(k, g, f, GRASS_SALT_WARP);
    float uz = f.y + grassWarp(k, g, f, GRASS_SALT_WARP + 1u);
    int cx = int(floor(ux)), cz = int(floor(uz));
    float f1 = 1e9, f2 = 1e9;
    uint best = 0u;
    for (int dj = -1; dj <= 1; ++dj)
        for (int di = -1; di <= 1; ++di) {
            int i = cx + di, j = cz + dj;   // relative to the point's cell
            uint h = hash3i(GRASS_SALT_CLUMP, g.x + i, g.y + j, k), st = h;
            float jx = grassDrawf(st);
            float jz = grassDrawf(st);
            float sx = float(i) + 0.5 + float(GRASS_CLUMP_JITTER) * (jx - 0.5);
            float sz = float(j) + 0.5 + float(GRASS_CLUMP_JITTER) * (jz - 0.5);
            float dx = sx - ux, dz = sz - uz, d = sqrt(dx * dx + dz * dz);
            if (d < f1) { f2 = f1; f1 = d; best = h; }
            else if (d < f2) f2 = d;
        }
    uint st = best;
    grassDraw(st); grassDraw(st);   // past the seed's jitter
    float uBare = grassDrawf(st);
    float uDens = grassDrawf(st);
    float uHeight = grassDrawf(st);
    float uLean = grassDrawf(st);
    float uStiff = grassDrawf(st);
    float uTint = grassDrawf(st);
    GrassClump c;
    c.id = best;
    c.edge = 1.0 - grassEase(0.0, GRASS_EDGE_BAND, f2 - f1);
    c.bare = uBare < GRASS_BARE(k);
    c.density = (1.0 - GRASS_CLUMP_VAR(k) * uDens) * (1.0 - GRASS_CLUMP_EDGE(k) * c.edge) * (c.bare ? float(GRASS_BARE_KEEP) : 1.0);
    c.height = 1.0 + GRASS_CLUMP_VAR(k) * (2.0 * uHeight - 1.0);
    c.lean = GRASS_TWO_PI * uLean;
    c.stiffness = 0.7 + 0.6 * uStiff;
    c.tint = uTint - 0.5;
    return c;
}

// grass.h GrassCandidate
struct GrassCandidate { uint id; uint state; ivec2 gpatch; int species; vec2 p; };

GrassCandidate grassCandidate(int k, ivec2 gpatch, ivec2 cell) {
    int n = GRASS_GRID(k);
    float cs = float(GRASS_PATCH_M) / float(n);
    GrassCandidate c;
    c.id = hash3i(GRASS_SALT_ROOT, gpatch.x * n + cell.x, gpatch.y * n + cell.y, k);
    c.state = grassRng(c.id);
    float jx = grassDrawf(c.state);
    float jz = grassDrawf(c.state);
    c.gpatch = gpatch; c.species = k;
    c.p = vec2((float(cell.x) + jx) * cs, (float(cell.y) + jz) * cs);
    return c;
}

// grass.h GrassRoot
struct GrassRoot { GrassCandidate at; float height; int blades; GrassClump clump; };

bool grassKeep(GrassCandidate c, GrassSite s, out GrassRoot r) {
    int k = c.species;
    uint st = c.state;
    float uSpecies = grassDrawf(st);
    float uKeep = grassDrawf(st);
    float uHeight = grassDrawf(st);
    uint uTuft = grassDraw(st);
    r.at = c; r.height = 0.0; r.blades = 0;
    float w[GRASS_SPECIES];
    grassSpecies(s, w);
    if (!(uSpecies < w[k])) return false;
    float suit = grassSuitability(s);
    if (!(suit > 0.0)) return false;
    GrassClump cl = grassClump(k, c.gpatch, c.p);
    r.clump = cl;
    if (!(uKeep < suit * grassDensityScale(k) * cl.density)) return false;
    r.height = (GRASS_HEIGHT_MIN(k) + (GRASS_HEIGHT_MAX(k) - GRASS_HEIGHT_MIN(k)) * uHeight) * cl.height * (float(GRASS_SHORT) + (1.0 - float(GRASS_SHORT)) * suit);
    r.blades = GRASS_TUFT_MIN(k) + int(uTuft % uint(GRASS_TUFT_MAX(k) - GRASS_TUFT_MIN(k) + 1));
    return true;
}

// grass.h GrassBlade
struct GrassBlade { vec2 p; float height; float width; float facing; vec2 leanDir; float lean; float bend; float phase; float tint; uint seed; };

GrassBlade grassBlade(GrassRoot r, int b) {
    int k = r.at.species;
    uint st = grassRng(hash_combine(r.at.id ^ GRASS_SALT_BLADE, uint(b)));
    float u1 = grassDrawf(st);
    float u2 = grassDrawf(st);
    float u3 = grassDrawf(st);
    float u4 = grassDrawf(st);
    float u5 = grassDrawf(st);
    float u6 = grassDrawf(st);
    float u7 = grassDrawf(st);
    float u8 = grassDrawf(st);
    float u9 = grassDrawf(st);
    float u10 = grassDrawf(st);
    float rr = sqrt(u1), a = GRASS_TWO_PI * u2;
    vec2 o = vec2(cos(a), sin(a));
    GrassBlade g;
    g.p = r.at.p + GRASS_TUFT_R(k) * rr * o;
    g.height = r.height * (0.75 + 0.5 * u3) * (1.0 - 0.25 * rr);
    g.width = g.height * GRASS_WIDTH(k) * (0.8 + 0.4 * u4);
    g.facing = GRASS_TWO_PI * u5;
    float ga = GRASS_TWO_PI * u6;
    vec2 lean = 0.6 * vec2(cos(r.clump.lean), sin(r.clump.lean)) + rr * o + 0.4 * vec2(cos(ga), sin(ga));
    float ll = length(lean);
    g.leanDir = ll > 1e-4 ? lean / ll : vec2(1.0, 0.0);
    g.lean = GRASS_LEAN(k) * (0.5 + u7);
    g.bend = GRASS_BEND(k) * (0.6 + 0.8 * u8);
    g.phase = u9;
    g.tint = 0.7 * r.clump.tint + 0.3 * (u10 - 0.5);
    g.seed = st;
    return g;
}

#endif
