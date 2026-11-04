#include "grass.h"
#include "grass_species.h"
#include "hash.h"
#include <math.h>
#include <string.h>

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

/* ---- levels of detail ---------------------------------------------------- */

float grass_typical(int k, float suit) {
    return 0.5f * (HEIGHT_MIN(k) + HEIGHT_MAX(k)) * ((float)GRASS_SHORT + (1.0f - (float)GRASS_SHORT) * suit);
}

float grass_end(float typical, float dist, float fy, float radius) {
    float px = typical * fy / fmaxf(dist, 1e-3f);
    return ease((float)GRASS_LOD_CLUMP_PX, (float)GRASS_LOD_END_PX, px) * (1.0f - ease((float)GRASS_RADIUS_FADE * radius, radius, dist));
}

void grass_lod(float px, int n, int sub, GrassLod *o) {
    float kmid = (float)((n + 1) / 2), N = (float)n, ns;
    if (px >= (float)GRASS_LOD_FULL_PX) {
        float e = ease((float)GRASS_LOD_FULL_PX, (float)(GRASS_LOD_FULL_PX * GRASS_LOD_BAND), px);
        o->bucket = GRASS_LOD_FULL;
        o->keep = kmid + (N - kmid) * e;
        o->morph = 1.0f - e;
        ns = 1.0f;
    } else if (px >= (float)GRASS_LOD_MID_PX) {
        float e = ease((float)GRASS_LOD_MID_PX, (float)(GRASS_LOD_MID_PX * GRASS_LOD_BAND), px);
        o->bucket = GRASS_LOD_MID;
        o->keep = 1.0f + (kmid - 1.0f) * e;
        o->morph = 1.0f - e;
        ns = e;
    } else {
        o->bucket = GRASS_LOD_CLUMP;
        o->keep = 1.0f;
        o->morph = 0.0f;
        ns = 0.0f;
    }
    /* a root in one cell of four stands for the three beside it as they go */
    o->root = sub ? 1.0f : ns;
    o->widen = N * (sub ? 1.0f + 3.0f * (1.0f - ns) : 1.0f) / o->keep;
    o->count = o->root > 0.0f ? (int)ceilf(o->keep - 1e-3f) : 0;
}

void grass_lod_all(int n, GrassLod *o) {
    o->bucket = GRASS_LOD_FULL; o->count = n; o->keep = (float)n; o->root = 1.0f; o->widen = 1.0f; o->morph = 0.0f;
}

float grass_lod_width(const GrassLod *l, int b) { return sat(l->keep - (float)b) * l->widen * l->root; }

/* ---- the field from afar ------------------------------------------------- */

static float lum3(const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

/* The blades of species k on suitable ground, as blade.vert shapes them
 * (on level ground) and blade.frag colours them: each sampled along its
 * length, each element by its area (width x length), the face light's mean
 * (0.8 + 0.2 max(N.n, 0), its up face seen half the time) folded in. Roots
 * weighed by their chance of being kept (their clump's density). */
void grass_field(int k, GrassField *f) {
    const GrassProfile *p = SP(k);
    memset(f, 0, sizeof(*f));
    if (!p->present) return;
    double up = 0.0, lie = 0.0, area = 0.0, col[3] = { 0, 0, 0 }, blades = 0.0;
    double lA[GRASS_FIELD_LAYERS] = { 0 }, lC[GRASS_FIELD_LAYERS][3] = { { 0 } };
    const int T = 16;
    float top = (float)GRASS_FIELD_TOP * 0.5f * (HEIGHT_MIN(k) + HEIGHT_MAX(k));   /* the layers' span */
    float rootShare = lum3(p->root) / fmaxf(lum3(p->tip), 1e-4f);
    for (int j = 0; j < 48; ++j)
        for (int i = 0; i < 48; ++i) {
            GrassCandidate c;
            grass_candidate(k, 7 + i / GRID(k), -3 + j / GRID(k), i % GRID(k), j % GRID(k), &c);
            GrassRoot r;
            r.at = c;
            grass_clump(k, c.px, c.pz, c.x, c.z, &r.clump);
            uint32_t st = c.state;
            (void)drawf(&st); (void)drawf(&st);
            float uHeight = drawf(&st);
            uint32_t uTuft = draw(&st);
            r.height = (HEIGHT_MIN(k) + (HEIGHT_MAX(k) - HEIGHT_MIN(k)) * uHeight) * r.clump.height;
            r.blades = TUFT_MIN(k) + (int)(uTuft % (uint32_t)(TUFT_MAX(k) - TUFT_MIN(k) + 1));
            float wr = r.clump.density;
            blades += wr * r.blades;
            for (int b = 0; b < r.blades; ++b) {
                GrassBlade g;
                grass_blade(&r, b, &g);
                /* blade.vert's centreline on level ground (n = y) */
                float H = g.height, bend = g.bend * H, cl = cosf(g.lean), sl = sinf(g.lean);
                float Ux = g.leanX * sl, Uy = cl, Uz = g.leanZ * sl;
                float q1[3] = { Ux * H / 3.0f, Uy * H / 3.0f, Uz * H / 3.0f };
                float q2[3] = { Ux * 0.72f * H + g.leanX * 0.5f * bend * (1.0f - p->curl), Uy * 0.72f * H, Uz * 0.72f * H + g.leanZ * 0.5f * bend * (1.0f - p->curl) };
                float q3[3] = { Ux * H + g.leanX * bend, Uy * H, Uz * H + g.leanZ * bend };
                #define LEN3(a, b, c) sqrtf((a) * (a) + (b) * (b) + (c) * (c))
                float len = 0.5f * (LEN3(q3[0], q3[1], q3[2]) + LEN3(q1[0], q1[1], q1[2]) + LEN3(q2[0] - q1[0], q2[1] - q1[1], q2[2] - q1[2])
                                    + LEN3(q3[0] - q2[0], q3[1] - q2[1], q3[2] - q2[2]));
                float sc = H / fmaxf(len, 1e-5f);
                float dryness = sat(g.tint + 0.5f) * p->dryShare;
                for (int s = 0; s < T; ++s) {
                    float t = ((float)s + 0.5f) / (float)T, u = 1.0f - t;
                    float d[3];
                    for (int a = 0; a < 3; ++a) d[a] = sc * (3.0f * u * u * q1[a] + 6.0f * u * t * (q2[a] - q1[a]) + 3.0f * t * t * (q3[a] - q2[a]));
                    float dl = LEN3(d[0], d[1], d[2]) / (float)T;
                    #undef LEN3
                    float cosT = d[1] / fmaxf(dl * (float)T, 1e-6f), sinT = sqrtf(fmaxf(1.0f - cosT * cosT, 0.0f));
                    float w = g.width * powf(u, p->taper);
                    float a = (float)(wr * w * dl);
                    up += wr * (2.0 / 3.14159265) * w * cosT * dl;
                    lie += wr * w * sinT * dl;
                    float ut = powf(t, 0.7f), face = 0.8f + 0.1f * sinT;
                    float y = sc * (3.0f * u * u * t * q1[1] + 3.0f * u * t * t * q2[1] + t * t * t * q3[1]);
                    int li = (int)(y / top * (float)GRASS_FIELD_LAYERS);
                    li = li < 0 ? 0 : li >= GRASS_FIELD_LAYERS ? GRASS_FIELD_LAYERS - 1 : li;
                    for (int q = 0; q < 3; ++q) {
                        float lush = p->root[q] + (p->tip[q] - p->root[q]) * ut;
                        float dry = p->dry[q] * (rootShare + (1.0f - rootShare) * ut);
                        float cq = a * (lush + (dry - lush) * dryness) * (1.0f + 0.3f * g.tint) * face;
                        col[q] += cq;
                        lC[li][q] += cq;
                    }
                    area += a;
                    lA[li] += a;
                }
            }
        }
    /* per blade: the mean over the roots kept */
    for (int q = 0; q < 3; ++q) f->albedo[q] = (float)(col[q] / fmax(area, 1e-12));
    f->upright = (float)(up / fmax(blades, 1e-12));
    f->lying = (float)(lie / fmax(blades, 1e-12));
    for (int i = 0; i < GRASS_FIELD_LAYERS; ++i) {
        f->layerArea[i] = (float)(lA[i] / fmax(area, 1e-12));
        for (int q = 0; q < 3; ++q) f->layerAlbedo[i][q] = (float)(lC[i][q] / fmax(lA[i], 1e-12));
    }
    /* the shaders' fit: albedo + (top - albedo)(1 - exp(-tau / scale)), top
     * the field's at GRASS_FIELD_TAU_FAR, scale by least squares on the
     * luminance over tau 0.1 .. GRASS_FIELD_TAU_FAR */
    float far[3];
    grass_field_seen(f, (float)GRASS_FIELD_TAU_FAR, far);
    for (int q = 0; q < 3; ++q) f->top[q] = far[q];
    float l0 = lum3(f->albedo), l1 = lum3(far), best = 1e30f;
    f->scale = 1.0f;
    for (float sc = 0.05f; sc < 8.0f; sc *= 1.02f) {
        float err = 0.0f;
        for (float tau = 0.1f; tau <= (float)GRASS_FIELD_TAU_FAR; tau *= 1.25f) {
            float c[3];
            grass_field_seen(f, tau, c);
            float e = l0 + (l1 - l0) * (1.0f - expf(-tau / sc)) - lum3(c);
            err += e * e;
        }
        if (err < best) { best = err; f->scale = sc; }
    }
}

void grass_field_seen(const GrassField *f, float tau, float out[3]) {
    double w = 0.0, c[3] = { 0, 0, 0 }, above = 0.0;
    for (int i = GRASS_FIELD_LAYERS - 1; i >= 0; --i) {
        double a = f->layerArea[i], v = a * exp(-tau * (above + 0.5 * a));
        above += a;
        w += v;
        for (int q = 0; q < 3; ++q) c[q] += v * f->layerAlbedo[i][q];
    }
    for (int q = 0; q < 3; ++q) out[q] = (float)(c[q] / fmax(w, 1e-12));
}

/* Worked out once (the profiles are read once, before the first frame). */
void grass_field_gpu(GrassSpeciesGPU out[GRASS_SPECIES]) {
    static GrassField fields[GRASS_SPECIES];
    static int done;
    if (!done) {
        for (int k = 0; k < GRASS_SPECIES; ++k) grass_field(k, &fields[k]);
        done = 1;
    }
    for (int k = 0; k < GRASS_SPECIES; ++k) {
        const GrassField f = fields[k];
        for (int q = 0; q < 3; ++q) { out[k].field[q] = f.albedo[q]; out[k].fieldTop[q] = f.top[q]; }
        out[k].field[3] = 0.0f;
        out[k].fieldTop[3] = f.scale;
        out[k].misc[1] = f.upright;
        out[k].misc[2] = f.lying;
    }
}
