#include "floor_gen.h"
#include "json.h"
#include "plant.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The office floor generator: see floor_gen.h. In order: the column lines and the cells between
 * them; bands of floor along x, with aisles between; a program in each cell; the structure and
 * services round what that placed; plants where they leave the walkways clear; which lights are
 * on in which state. floor_validate then checks the result, knowing none of that. */

const char *const FLOOR_STYLE_NAMES[FS_COUNT] = { "open", "lanes", "collab", "exec", "dense" };
const char *const FLOOR_CORE_NAMES[3] = { "none", "back", "centre" };

const char *const LIGHT_STATE_NAMES[LIGHTS_COUNT] = { "after_hours", "late_shift", "cleaners", "screens" };

FloorEntry FLOOR_CATALOGUE[FLOOR_CATALOGUE_MAX];
int FLOOR_CATALOGUE_COUNT = 0;

static int name_index(const Json *e, const char *key, const char *const *names, int n) {
    const Json *j = json_get(e, key);
    if (!j || j->type != JSON_STRING) return -1;
    for (int i = 0; i < n; ++i) if (!strcmp(j->string, names[i])) return i;
    return -1;
}

static bool number_of(const Json *e, const char *key, double *v) {
    const Json *j = json_get(e, key);
    if (!j || j->type != JSON_NUMBER) return false;
    *v = j->number;
    return true;
}

static const char *entry_read(const Json *e, FloorEntry *out) {
    const Json *name = json_get(e, "name"), *corner = json_get(e, "corner"), *drawn = json_get(e, "drawn");
    double bays, bayW, depth, height, seed;
    if (!name || name->type != JSON_STRING || !name->string[0] || strlen(name->string) >= sizeof(out->name)) return "needs a \"name\", under 24 characters";
    if (!number_of(e, "bays", &bays) || !number_of(e, "bay_width", &bayW) || !number_of(e, "depth", &depth) || !number_of(e, "floor_height", &height) || !number_of(e, "seed", &seed))
        return "needs \"bays\", \"bay_width\", \"depth\", \"floor_height\" and \"seed\"";
    if (bays < 1.0 || bayW <= 0.0 || depth <= 0.0 || height <= 0.0 || seed < 0.0 || seed > 4294967295.0) return "has a number out of range";
    int core = name_index(e, "core", FLOOR_CORE_NAMES, 3), style = name_index(e, "style", FLOOR_STYLE_NAMES, FS_COUNT);
    int state = name_index(e, "light_state", LIGHT_STATE_NAMES, LIGHTS_COUNT);
    if (core < 0 || style < 0 || state < 0) return "has an unknown \"core\", \"style\" or \"light_state\"";
    if ((corner && corner->type != JSON_BOOL) || (drawn && drawn->type != JSON_BOOL)) return "has a \"corner\" or \"drawn\" that isn't true or false";
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", name->string);
    out->p = (FloorParams){ (int)bays, (float)bayW, (float)depth, (float)height, core, corner && corner->number != 0.0, style, (uint32_t)seed };
    out->state = state;
    out->drawn = !drawn || drawn->number != 0.0;
    return NULL;
}

bool floor_catalogue_load(const char *assets) {
    static bool loaded = false;
    if (loaded) return true;
    char path[1100], err[256];
    snprintf(path, sizeof(path), "%s/interiors/catalogue.json", assets);
    Json *doc = json_load(path, err, sizeof(err));
    if (!doc) { fprintf(stderr, "%s: %s\n", path, err); return false; }
    const Json *mods = json_get(doc, "modules");
    const char *bad = mods && mods->type == JSON_ARRAY ? NULL : "no \"modules\" list";
    int line = 0, n = 0;
    for (int i = 0; !bad && i < json_count(mods); ++i) {
        const Json *e = json_at(mods, i);
        line = e->line;
        if (n >= FLOOR_CATALOGUE_MAX) bad = "more modules than the baker holds";
        else if (e->type != JSON_OBJECT) bad = "a module must be an object";
        else bad = entry_read(e, &FLOOR_CATALOGUE[n]);
        for (int k = 0; !bad && k < n; ++k) if (!strcmp(FLOOR_CATALOGUE[k].name, FLOOR_CATALOGUE[n].name)) bad = "names a module twice";
        n++;
    }
    json_free(doc);
    if (bad) { fprintf(stderr, "%s: line %d: %s\n", path, line, bad); return false; }
    FLOOR_CATALOGUE_COUNT = n;
    loaded = true;
    return true;
}

const char *floor_zone_name(int kind) {
    static const char *const N[FZ_COUNT] = { "work", "room", "lounge", "store" };
    return kind >= 0 && kind < FZ_COUNT ? N[kind] : "?";
}

#define WALK_R     0.40f   /* half a person in an aisle or a lane: 0.8 m clear */
#define REACH_R    0.35f   /* half a person squeezing to a seat */
#define REACH_NEAR 0.40f   /* a place counts as reached from this near it */
#define GRID       0.10f
#define LANE       1.20f   /* from a column line to the cell on its lane side: its half width and 0.9 m */
#define TIGHT      0.35f   /* to the cell on its other side */
#define AISLE      1.20f
#define DESK_D     1.65f   /* a desk and its chair, back edge to behind the chair */
#define PI_F_      3.14159265f

/* ------------------------------------------------------------------ small --- */

typedef enum { P_BENCH = 0, P_SINGLE, P_ZRUN, P_ROOM, P_LOUNGE, P_FILING, P_COUNT } Program;
typedef struct Band { float z0, z1; int prog; } Band;
typedef struct Cell { float x0, x1; } Cell;

typedef struct G {
    FloorPlan *P;
    uint32_t   rng;
    float      dw;              /* a desk's width */
    Band       band[32];  int nband;
    Cell       cell[FLOOR_MAX_LINES];  int ncell;
    FloorPoint spot[256]; int nspot;   /* where a plant might go */
} G;

static float draw(G *g) {   /* xorshift32; one draw a statement */
    uint32_t s = g->rng;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    g->rng = s;
    return (float)(s >> 8) * (1.0f / 16777216.0f);
}

static uint32_t hmix(uint32_t h, uint32_t v) {
    h ^= v * 0x9E3779B1u;
    h = (h << 13) | (h >> 19);
    return h * 5u + 0xE6546B64u;
}

/* A number in 0..1 from an item's seed and a name: stateless, so a light's state can't shift the layout. */
static float hr(uint32_t seed, uint32_t tag) {
    uint32_t h = hmix(hmix(seed, tag), 0x51EDu);
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return (float)(h >> 8) * (1.0f / 16777216.0f);
}

static float minf_(float a, float b) { return a < b ? a : b; }
static float maxf_(float a, float b) { return a > b ? a : b; }

static FloorItem *item_add(G *g, int kind, int zone, int dir, float x0, float z0, float x1, float z1) {
    FloorPlan *P = g->P;
    if (P->nitem >= FLOOR_MAX_ITEMS) return &P->item[FLOOR_MAX_ITEMS - 1];
    FloorItem *it = &P->item[P->nitem];
    memset(it, 0, sizeof(*it));
    it->kind = kind; it->zone = zone; it->dir = dir;
    it->x0 = x0; it->z0 = z0; it->x1 = x1; it->z1 = z1;
    it->ox = (x0 + x1) * 0.5f; it->oz = (z0 + z1) * 0.5f;
    it->seed = hmix(hmix(P->p.seed, 0x17E4u), (uint32_t)P->nitem);
    P->nitem++;
    return it;
}

static void solid_add(G *g, float x0, float z0, float x1, float z1) {
    FloorPlan *P = g->P;
    if (P->nsolid >= FLOOR_MAX_RECTS) return;
    FloorRect *r = &P->solid[P->nsolid++];
    r->x0 = x0; r->z0 = z0; r->x1 = x1; r->z1 = z1; r->item = P->nitem - 1;
}

static void reach_add(G *g, float x, float z) {
    FloorPlan *P = g->P;
    if (P->nreach < FLOOR_MAX_POINTS) { P->reach[P->nreach].x = x; P->reach[P->nreach].z = z; P->nreach++; }
}

static void walk_add(G *g, float x, float z) {
    FloorPlan *P = g->P;
    if (P->nwalk < FLOOR_MAX_POINTS) { P->walk[P->nwalk].x = x; P->walk[P->nwalk].z = z; P->nwalk++; }
}

static void spot_add(G *g, float x, float z) {
    if (g->nspot < 256) { g->spot[g->nspot].x = x; g->spot[g->nspot].z = z; g->nspot++; }
}

static int zone_add(G *g, int kind, float x0, float z0, float x1, float z1) {
    FloorPlan *P = g->P;
    if (P->nzone >= FLOOR_MAX_ZONES) return FLOOR_MAX_ZONES - 1;
    FloorZone *z = &P->zone[P->nzone];
    z->kind = kind; z->x0 = x0; z->z0 = z0; z->x1 = x1; z->z1 = z1;
    return P->nzone++;
}

/* ------------------------------------------------------------- programs --- */

/* A desk with its back edge's centre at (ox, oz), its user towards dir. */
static void desk_add(G *g, int zone, float ox, float oz, int dir, float w) {
    float x0, z0, x1, z1, h = w * 0.5f;
    switch (dir) {
    case 0:  x0 = ox - h; x1 = ox + h; z0 = oz; z1 = oz + DESK_D; break;
    case 2:  x0 = ox - h; x1 = ox + h; z0 = oz - DESK_D; z1 = oz; break;
    case 1:  z0 = oz - h; z1 = oz + h; x0 = ox; x1 = ox + DESK_D; break;
    default: z0 = oz - h; z1 = oz + h; x0 = ox - DESK_D; x1 = ox; break;
    }
    FloorItem *it = item_add(g, FI_DESK, zone, dir, x0, z0, x1, z1);
    it->ox = ox; it->oz = oz; it->a = w;
    float r = draw(g);
    it->variant = r < 0.1f ? 0 : r < 0.4f ? 2 : 1;
    r = draw(g);
    it->b = it->variant == 0 ? 0.0f : r < 0.36f ? -1.0f : r < 0.72f ? 1.0f : 0.0f;
    solid_add(g, x0, z0, x1, z1);
    static const float DX[4] = { 0, 1, 0, -1 }, DZ[4] = { 1, 0, -1, 0 };
    reach_add(g, ox + DX[dir] * (DESK_D + 0.3f), oz + DZ[dir] * (DESK_D + 0.3f));
}

static void pendant_add(G *g, int zone, float x, float z, int alongZ) {
    FloorItem *it = item_add(g, FI_PENDANT, zone, alongZ, x - 0.8f, z - 0.06f, x + 0.8f, z + 0.06f);
    if (alongZ) { it->x0 = x - 0.06f; it->x1 = x + 0.06f; it->z0 = z - 0.8f; it->z1 = z + 0.8f; }
    it->ox = x; it->oz = z;
}

/* Back-to-back desks along x, either side of a screen. */
static bool fill_bench(G *g, float x0, float z0, float x1, float z1) {
    int n = (int)floorf((x1 - x0) / g->dw + 1e-3f);
    if (n < 1 || z1 - z0 < DESK_D * 2.0f + 0.02f) return false;
    float used = (float)n * g->dw, slack = x1 - x0 - used, xs = x0 + slack * draw(g), zc = (z0 + z1) * 0.5f;
    int zone = zone_add(g, FZ_WORK, xs, zc - DESK_D, xs + used, zc + DESK_D);
    item_add(g, FI_PANEL, zone, 0, xs + 0.05f, zc - 0.01f, xs + used - 0.05f, zc + 0.01f);
    solid_add(g, xs + 0.05f, zc - 0.01f, xs + used - 0.05f, zc + 0.01f);
    for (int i = 0; i < n; ++i) {
        float x = xs + ((float)i + 0.5f) * g->dw;
        desk_add(g, zone, x, zc - 0.01f, 2, g->dw);
        desk_add(g, zone, x, zc + 0.01f, 0, g->dw);
    }
    for (int i = 0; i < n; i += 2) pendant_add(g, zone, xs + ((float)i + minf_((float)(n - i), 2.0f) * 0.5f) * g->dw, zc, 0);
    if (xs - x0 > 0.6f) spot_add(g, (x0 + xs) * 0.5f, zc);
    if (x1 - (xs + used) > 0.6f) spot_add(g, (xs + used + x1) * 0.5f, zc);
    return true;
}

/* One row of desks with a backlit screen behind them. */
static bool fill_single(G *g, float x0, float z0, float x1, float z1) {
    int n = (int)floorf((x1 - x0) / g->dw + 1e-3f);
    if (n < 1 || z1 - z0 < DESK_D + 0.02f) return false;
    float used = (float)n * g->dw, slack = x1 - x0 - used, xs = x0 + slack * draw(g);
    float back = draw(g) < 0.75f ? z1 - 0.02f : z0 + 0.02f;
    int dir = back > (z0 + z1) * 0.5f ? 2 : 0;
    float zu = dir == 2 ? back - DESK_D : back + DESK_D;
    int zone = zone_add(g, FZ_WORK, xs, minf_(back, zu), xs + used, maxf_(back, zu));
    item_add(g, FI_PANEL, zone, 0, xs + 0.05f, back - 0.01f, xs + used - 0.05f, back + 0.01f);
    solid_add(g, xs + 0.05f, back - 0.01f, xs + used - 0.05f, back + 0.01f);
    for (int i = 0; i < n; ++i) desk_add(g, zone, xs + ((float)i + 0.5f) * g->dw, back + (dir == 2 ? -0.01f : 0.01f), dir, g->dw);
    for (int i = 0; i < n; i += 2) pendant_add(g, zone, xs + ((float)i + minf_((float)(n - i), 2.0f) * 0.5f) * g->dw, (back + zu) * 0.5f, 0);
    return true;
}

/* Runs of desks into the building, back to back along z: the sight lines between them. */
static bool fill_zrun(G *g, float x0, float z0, float x1, float z1) {
    const float pair = DESK_D * 2.0f + 0.02f, between = 1.0f;
    int np = (int)floorf((x1 - x0 + between) / (pair + between) + 1e-3f), m = (int)floorf((z1 - z0) / g->dw + 1e-3f);
    if (np < 1 || m < 2) return false;
    float used = (float)np * pair + (float)(np - 1) * between, side = (x1 - x0 - used) * 0.5f;
    float zs = z0 + (z1 - z0 - (float)m * g->dw) * 0.5f, zl = (float)m * g->dw;
    for (int p = 0; p < np; ++p) {
        float xc = x0 + side + pair * 0.5f + (float)p * (pair + between);
        int zone = zone_add(g, FZ_WORK, xc - pair * 0.5f, zs, xc + pair * 0.5f, zs + zl);
        item_add(g, FI_PANEL, zone, 1, xc - 0.01f, zs + 0.05f, xc + 0.01f, zs + zl - 0.05f);
        solid_add(g, xc - 0.01f, zs + 0.05f, xc + 0.01f, zs + zl - 0.05f);
        for (int i = 0; i < m; ++i) {
            float z = zs + ((float)i + 0.5f) * g->dw;
            desk_add(g, zone, xc + 0.01f, z, 1, g->dw);
            desk_add(g, zone, xc - 0.01f, z, 3, g->dw);
        }
        for (int i = 0; i < m; i += 2) pendant_add(g, zone, xc, zs + ((float)i + minf_((float)(m - i), 2.0f) * 0.5f) * g->dw, 1);
        spot_add(g, xc, zs - 0.32f);
        spot_add(g, xc, zs + zl + 0.32f);
    }
    return true;
}

FloorTable floor_room_table(const FloorItem *room) {
    FloorTable t;
    float cz = (room->z0 + room->z1) * 0.5f;
    t.x0 = room->x0 + 1.0f; t.x1 = room->x1 - 1.0f;
    t.z0 = cz - 0.5f; t.z1 = cz + 0.5f;
    return t;
}

/* A glass room with a long table; its door opens on an aisle, near the lane. */
static bool fill_room(G *g, float x0, float z0, float x1, float z1) {
    float w = x1 - x0, dz = z1 - z0;
    if (w < 3.8f || dz < 3.9f) return false;
    float rw = minf_(w, 4.2f + draw(g) * 2.2f);
    if (w - rw < 1.2f) rw = w;
    float xa = g->P->lane > 0.0f ? x0 : x1 - rw, xb = xa + rw;
    int zone = zone_add(g, FZ_ROOM, xa, z0, xb, z1), dir = draw(g) < 0.5f ? 0 : 2;
    float door = g->P->lane > 0.0f ? xa + 0.8f : xb - 0.8f, zd = dir == 0 ? z0 : z1, zo = dir == 0 ? z1 : z0, g2 = 0.03f;
    FloorItem *it = item_add(g, FI_ROOM, zone, dir, xa, z0, xb, z1);
    it->a = door;
    /* four walls, the door a gap of 1 m in the one on the aisle */
    solid_add(g, xa - g2, zo - g2, xb + g2, zo + g2);
    solid_add(g, xa - g2, z0, xa + g2, z1);
    solid_add(g, xb - g2, z0, xb + g2, z1);
    solid_add(g, xa - g2, zd - g2, door - 0.5f, zd + g2);
    solid_add(g, door + 0.5f, zd - g2, xb + g2, zd + g2);
    FloorTable t = floor_room_table(it);
    solid_add(g, t.x0, t.z0 - 0.75f, t.x1, t.z1 + 0.75f);
    float sign = dir == 0 ? 1.0f : -1.0f;
    reach_add(g, door, zd - sign * 0.5f);
    reach_add(g, door, zd + sign * 0.5f);
    pendant_add(g, zone, (t.x0 + t.x1) * 0.5f, (t.z0 + t.z1) * 0.5f, 0);
    if (w - rw >= 1.2f) spot_add(g, g->P->lane > 0.0f ? (xb + x1) * 0.5f : (x0 + xa) * 0.5f, (z0 + z1) * 0.5f);
    return true;
}

/* A sofa on a rug, with a low table, a side table and an orb lamp on its plinth; two armchairs, a
 * feature wall behind with a canvas, and a big plant if there's room. */
static bool fill_lounge(G *g, float x0, float z0, float x1, float z1) {
    float w = x1 - x0, dz = z1 - z0;
    if (w < 4.1f || dz < 3.0f) return false;
    float cx = x0 + (w - 4.1f) * draw(g) + 2.05f, cz = (z0 + z1) * 0.5f;
    int dir = draw(g) < 0.7f ? 2 : 0;
    float f = dir == 2 ? -1.0f : 1.0f;   /* the way it faces, along z */
    int zone = zone_add(g, FZ_LOUNGE, cx - 2.05f, z0, cx + 2.05f, z1);
    FloorItem *it = item_add(g, FI_LOUNGE, zone, dir, cx - 1.97f, cz - 1.1f, cx + 1.97f, cz + 1.1f);
    it->ox = cx; it->oz = cz;
    it->variant = draw(g) < 0.5f ? 1 : 0;
    int chairs = it->variant;
    float a, b;
    a = minf_(cz - f * 0.15f, cz - f * 1.05f); b = maxf_(cz - f * 0.15f, cz - f * 1.05f);
    solid_add(g, cx - 1.12f, a, cx + 1.12f, b);                     /* the sofa */
    a = minf_(cz + f * 0.15f, cz + f * 0.55f); b = maxf_(cz + f * 0.15f, cz + f * 0.55f);
    solid_add(g, cx - 0.45f, a, cx + 0.45f, b);                     /* the low table */
    a = minf_(cz - f * 0.35f, cz - f * 0.85f); b = maxf_(cz - f * 0.35f, cz - f * 0.85f);
    solid_add(g, cx + 1.45f, a, cx + 1.95f, b);                     /* the side table */
    solid_add(g, cx - 1.97f, a, cx - 1.47f, b);                     /* the orb's plinth */
    if (chairs) {
        a = minf_(cz + f * 0.7f, cz + f * 1.4f); b = maxf_(cz + f * 0.7f, cz + f * 1.4f);
        solid_add(g, cx - 1.25f, a, cx - 0.55f, b);
        solid_add(g, cx + 0.55f, a, cx + 1.25f, b);
    }
    float wall0 = cz - f * 1.2f, wall1 = cz - f * 1.35f;
    int style = g->P->p.style;
    bool feature = (style == FS_EXEC || style == FS_COLLAB) && dz >= 3.0f && draw(g) < 0.6f;
    if (feature) {
        float za = minf_(wall0, wall1), zb = maxf_(wall0, wall1), hw = minf_(1.9f, (w - 0.2f) * 0.5f);
        FloorItem *fw = item_add(g, FI_WALL, zone, dir, cx - hw, za, cx + hw, zb);
        fw->b = minf_(2.6f, g->P->ceil - 0.25f);
        solid_add(g, cx - hw, za, cx + hw, zb);
        float awide = 1.6f + draw(g) * 0.6f;
        FloorItem *art = item_add(g, FI_ART, zone, dir, cx - awide * 0.5f, za, cx + awide * 0.5f, zb);
        art->ox = cx; art->oz = dir == 2 ? za : zb; art->a = awide; art->b = 0.9f + draw(g) * 0.3f;
    }
    reach_add(g, cx, cz + f * (chairs ? 1.75f : 1.0f));
    spot_add(g, cx + 2.5f, cz);
    spot_add(g, cx - 2.5f, cz);
    return true;
}

/* A run of cabinets, back to back where the band is deep enough, and a printer on its end; a pendant over it. */
static bool fill_filing(G *g, float x0, float z0, float x1, float z1) {
    const float cab = 0.8f;
    int n = (int)floorf(minf_(x1 - x0 - 0.8f, 5.6f) / cab + 1e-3f);
    if (n < 2 || z1 - z0 < 0.55f) return false;
    bool two = z1 - z0 >= 1.04f;
    float used = (float)n * cab, xs = x0 + (x1 - x0 - used - 0.75f) * draw(g), zc = (z0 + z1) * 0.5f, hd = two ? 0.5f : 0.25f;
    int zone = zone_add(g, FZ_STORE, xs, zc - hd, xs + used + 0.75f, zc + hd);
    FloorItem *it = item_add(g, FI_FILING, zone, draw(g) < 0.5f ? 0 : 2, xs, zc - hd, xs + used, zc + hd);
    it->variant = draw(g) < 0.35f ? 1 : 0;
    it->b = two ? 1.0f : 0.0f;
    solid_add(g, xs, zc - hd, xs + used, zc + hd);
    item_add(g, FI_PRINTER, zone, 0, xs + used + 0.1f, zc - 0.3f, xs + used + 0.75f, zc + 0.3f);
    solid_add(g, xs + used + 0.1f, zc - 0.3f, xs + used + 0.75f, zc + 0.3f);
    reach_add(g, xs + used * 0.5f, zc - hd - 0.4f);
    reach_add(g, xs + used * 0.5f, zc + hd + 0.4f);
    pendant_add(g, zone, xs + used * 0.5f, zc, 0);
    return true;
}

static bool fill(G *g, int prog, float x0, float z0, float x1, float z1) {
    /* what's asked for, and if it doesn't fit, what will */
    switch (prog) {
    case P_ROOM:   if (fill_room(g, x0, z0, x1, z1)) return true; break;
    case P_LOUNGE: if (fill_lounge(g, x0, z0, x1, z1)) return true; break;
    case P_ZRUN:   if (fill_zrun(g, x0, z0, x1, z1)) return true; break;
    case P_BENCH:  if (fill_bench(g, x0, z0, x1, z1)) return true; break;
    case P_SINGLE: if (fill_single(g, x0, z0, x1, z1)) return true; break;
    case P_FILING: if (fill_filing(g, x0, z0, x1, z1)) return true; break;
    }
    return fill_bench(g, x0, z0, x1, z1) || fill_single(g, x0, z0, x1, z1) || fill_filing(g, x0, z0, x1, z1);
}

/* ---------------------------------------------------------------- bands --- */

static const float WEIGHTS[FS_COUNT][P_COUNT] = {
    /*            bench single zrun  room  lounge filing */
    /* open   */ { 0.70f, 0.05f, 0.05f, 0.08f, 0.10f, 0.02f },
    /* lanes  */ { 0.05f, 0.02f, 0.75f, 0.05f, 0.10f, 0.03f },
    /* collab */ { 0.22f, 0.03f, 0.10f, 0.35f, 0.25f, 0.05f },
    /* exec   */ { 0.15f, 0.05f, 0.05f, 0.25f, 0.45f, 0.05f },
    /* dense  */ { 0.65f, 0.05f, 0.15f, 0.02f, 0.00f, 0.13f },
};

static float prog_depth(G *g, int prog, float rem) {
    switch (prog) {
    case P_BENCH:  return DESK_D * 2.0f + 0.04f;
    case P_SINGLE: return DESK_D + 0.06f;
    case P_ZRUN: {
        int m = g->P->p.style == FS_LANES ? 3 + (int)(draw(g) * 3.0f) : 2 + (int)(draw(g) * 2.0f);
        while (m > 2 && (float)m * g->dw > rem) m--;
        return (float)m * g->dw;
    }
    case P_ROOM:   { float d = 4.0f + draw(g) * 0.9f; return d > rem ? 4.0f : d; }
    case P_LOUNGE: { float d = 3.1f + draw(g) * 0.8f; return d > rem ? 3.1f : d; }
    default:       return 1.06f;
    }
}

/* Bands from z0 to z1, each with an aisle after it; slack goes to the aisles. */
static void bands_fill(G *g, float z0, float z1, bool nearGlass) {
    const FloorPlan *P = g->P;
    int first = g->nband;
    float z = z0;
    while (g->nband < 32) {
        float rem = z1 - z, w[P_COUNT], sum = 0.0f;
        if (rem < 1.06f) break;
        for (int i = 0; i < P_COUNT; ++i) {
            float need = i == P_BENCH ? DESK_D * 2.0f + 0.04f : i == P_SINGLE ? DESK_D + 0.06f : i == P_ZRUN ? g->dw * 2.0f : i == P_ROOM ? 4.0f : i == P_LOUNGE ? 3.1f : 1.06f;
            w[i] = need <= rem ? WEIGHTS[P->p.style][i] : 0.0f;
            /* a small floor has room for one thing of each: one that's only filing, or only a room, is no office */
            if (i == P_FILING && rem > 2.6f) w[i] *= 0.3f;
            sum += w[i];
        }
        /* some room left, none of the style's programs fit: the smallest things do */
        if (sum <= 0.0f) { w[P_SINGLE] = DESK_D + 0.06f <= rem ? 1.0f : 0.0f; w[P_FILING] = 1.0f; sum = w[P_SINGLE] + 1.0f; }
        float r = draw(g) * sum;
        int prog = 0;
        while (prog < P_COUNT - 1 && r >= w[prog]) { r -= w[prog]; prog++; }
        if (w[prog] <= 0.0f) break;
        /* the first band on collaborative floors tends to be a lounge at the glass, as the reference's */
        float lg = draw(g);
        if (nearGlass && g->nband == first && rem >= 3.1f && (P->p.style == FS_EXEC || (P->p.style == FS_COLLAB && lg < 0.55f))) prog = P_LOUNGE;
        float d = prog_depth(g, prog, rem);
        g->band[g->nband].z0 = z; g->band[g->nband].z1 = z + d; g->band[g->nband].prog = prog;
        g->nband++;
        z += d + AISLE;
    }
    int n = g->nband - first;
    if (!n) return;
    /* the slack, over the aisles and the two ends */
    float last = g->band[g->nband - 1].z1, slack = maxf_(z1 - last, 0.0f), each = slack / (float)(n + 1);
    for (int i = 0; i < n; ++i) {
        float s = each * (float)(i + 1);
        g->band[first + i].z0 += s; g->band[first + i].z1 += s;
    }
}

/* -------------------------------------------------------------- structure --- */

/* Where a column row near z may stand: inside a band, not across an aisle. */
static float column_z(const G *g, float z, float zMaxRow) {
    float best = z, bestD = 1e9f;
    for (int i = 0; i < g->nband; ++i) {
        float a = g->band[i].z0 + 0.3f, b = g->band[i].z1 - 0.3f;
        if (b < a) { a = b = (g->band[i].z0 + g->band[i].z1) * 0.5f; }
        float c = z < a ? a : z > b ? b : z, d = fabsf(c - z);
        if (d < bestD && c < zMaxRow) { bestD = d; best = c; }
    }
    return best;
}

static bool rect_free(const FloorPlan *P, float x0, float z0, float x1, float z1, float margin) {
    bool periodic = !P->p.corner;
    for (int i = 0; i < P->nsolid; ++i) {
        const FloorRect *r = &P->solid[i];
        for (int k = periodic ? -1 : 0; k <= (periodic ? 1 : 0); ++k) {
            float s = (float)k * P->W;
            if (x0 - margin < r->x1 + s && x1 + margin > r->x0 + s && z0 - margin < r->z1 && z1 + margin > r->z0) return false;
        }
    }
    return true;
}

static int zones_of(const FloorPlan *P, int kind) {
    int n = 0;
    for (int z = 0; z < P->nzone; ++z) n += P->zone[z].kind == kind;
    return n;
}

static void core_doors(G *g, int zone, float x0, float x1, float z, int dir) {
    /* doors along a face, with a canvas in the widest gap */
    float prev = x0, x = x0 + 1.0f + draw(g) * 2.0f, bestGap = 0.0f, bestX = 0.0f;
    while (x < x1 - 1.3f) {
        FloorItem *d = item_add(g, FI_DOOR, zone, dir, x, z, x + 0.95f, z);
        d->ox = x + 0.475f; d->oz = z; d->a = 0.95f;
        reach_add(g, x + 0.475f, z + (dir == 0 ? 0.5f : -0.5f));
        if (x - prev > bestGap) { bestGap = x - prev; bestX = (x + prev) * 0.5f; }
        prev = x + 0.95f;
        x = prev + 2.5f + draw(g) * 3.0f;
    }
    if (x1 - prev > bestGap) { bestGap = x1 - prev; bestX = (x1 + prev) * 0.5f; }
    if (bestGap > 2.0f) {
        float w = minf_(1.7f, bestGap - 0.6f);
        FloorItem *a = item_add(g, FI_ART, zone, dir, bestX - w * 0.5f, z, bestX + w * 0.5f, z);
        a->ox = bestX; a->oz = z; a->a = w; a->b = 0.95f;
    }
}

/* ---------------------------------------------------------------- lights --- */

#define BIT(s) ((uint8_t)(1u << (s)))

static bool in_cluster(const FloorPlan *P, float cx, float cz, float x, float z) {
    float dx = x - cx, dz = z - cz;
    if (!P->p.corner) dx -= P->W * floorf(dx / P->W + 0.5f);
    return dx * dx + dz * dz < 6.5f * 6.5f;
}

static void light_list(FloorPlan *P, float pendantY) {
    P->nlight = 0;
    for (int i = 0; i < P->nitem && P->nlight < FLOOR_MAX_LIGHTS - 4; ++i) {
        const FloorItem *it = &P->item[i];
        FloorLight l;
        l.item = i;
#define PUSH(X, Y, Z, LM, B) do { l.x = (X); l.y = (Y); l.z = (Z); l.lumens = (LM); l.bits = (B); if (l.bits) P->light[P->nlight++] = l; } while (0)
        switch (it->kind) {
        case FI_DESK: {
            static const float DX[4] = { 0, 1, 0, -1 }, DZ[4] = { 1, 0, -1, 0 };
            float dx = DX[it->dir], dz = DZ[it->dir];
            PUSH(it->ox + dx * 0.15f + dz * it->b * it->a * 0.4f, 1.1f, it->oz + dz * 0.15f - dx * it->b * it->a * 0.4f, LM_LAMP, it->bit[2]);
            PUSH(it->ox + dx * 0.12f, 0.71f, it->oz + dz * 0.12f, LM_STRIP, it->bit[3]);
            break;
        }
        case FI_PENDANT:
            PUSH(it->ox, pendantY, it->oz, LM_PENDANT, it->bit[0]);
            break;
        case FI_TROFFER:
            PUSH(it->ox, P->ceil - 0.6f, it->oz, LM_TROFFER, it->bit[0]);
            break;
        case FI_LOUNGE: {
            float f = it->dir == 2 ? -1.0f : 1.0f;
            PUSH(it->ox - 1.72f, 0.67f, it->oz - f * 0.6f, LM_ORB, it->bit[0]);
            break;
        }
        case FI_PANEL:
            PUSH(it->ox, FLOOR_PANEL_H, it->oz, LM_COVE_M * maxf_(it->x1 - it->x0, it->z1 - it->z0), it->bit[0]);
            break;
        case FI_ROOM:
            PUSH(it->ox, 2.68f, it->z0, LM_COVE_M * (it->x1 - it->x0), it->bit[0]);
            break;
        case FI_WALL:
            PUSH(it->ox, it->b, it->oz, LM_COVE_M * (it->x1 - it->x0), it->bit[0]);
            break;
        case FI_CORE:
            PUSH(it->ox, P->ceil - 0.3f, it->z0, LM_COVE_M * minf_(it->x1 - it->x0, 12.0f), it->bit[0]);
            break;
        default: break;
        }
#undef PUSH
    }
}

/* Which state each light is on in: drawn per item from its seed, so that they don't touch the layout. */
static void lights_assign(G *g) {
    FloorPlan *P = g->P;
    int work = 0;
    for (int z = 0; z < P->nzone; ++z) work += P->zone[z].kind == FZ_WORK;
    /* the late shift works round one desk zone */
    float cx = 0.0f, cz = 0.0f;
    int pick = work ? (int)(hr(P->p.seed, 0xC1u) * (float)work) : 0;
    for (int z = 0, w = 0; z < P->nzone; ++z) {
        if (P->zone[z].kind != FZ_WORK) continue;
        if (w++ == pick) { cx = (P->zone[z].x0 + P->zone[z].x1) * 0.5f; cz = (P->zone[z].z0 + P->zone[z].z1) * 0.5f; }
    }
    for (int i = 0; i < P->nitem; ++i) {
        FloorItem *it = &P->item[i];
        uint32_t s = it->seed;
        float busy = hr(P->p.seed ^ 0xB5u, (uint32_t)it->zone);   /* some zones are busier after hours */
        float x = (it->x0 + it->x1) * 0.5f, z = (it->z0 + it->z1) * 0.5f;
        bool in = in_cluster(P, cx, cz, x, z);
        memset(it->bit, 0, sizeof(it->bit));
        switch (it->kind) {
        case FI_DESK:
            for (int k = 0; k < it->variant; ++k) {
                uint32_t t = (uint32_t)k * 16u;
                if (hr(s, t + 1u) < 0.30f + 0.5f * busy) it->bit[k] |= BIT(LIGHTS_AFTER_HOURS);
                if (hr(s, t + 2u) < (in ? 0.95f : 0.08f)) it->bit[k] |= BIT(LIGHTS_LATE_SHIFT);
                if (hr(s, t + 3u) < 0.06f) it->bit[k] |= BIT(LIGHTS_CLEANERS);
                if (hr(s, t + 4u) < 0.50f) it->bit[k] |= BIT(LIGHTS_SCREENS);
            }
            if (it->b != 0.0f) {
                if (hr(s, 5u) < 0.55f + 0.45f * busy) it->bit[2] |= BIT(LIGHTS_AFTER_HOURS);
                if (in || hr(s, 6u) < 0.05f) it->bit[2] |= BIT(LIGHTS_LATE_SHIFT);
            }
            if (it->variant) {
                if (hr(s, 7u) < 0.30f + 0.50f * busy) it->bit[3] |= BIT(LIGHTS_AFTER_HOURS);
                if (in) it->bit[3] |= BIT(LIGHTS_LATE_SHIFT);
            }
            break;
        case FI_PENDANT:
            if (hr(s, 8u) < (P->zone[it->zone].kind == FZ_ROOM ? 0.65f : 0.35f + 0.50f * busy)) it->bit[0] |= BIT(LIGHTS_AFTER_HOURS);
            if (in) it->bit[0] |= BIT(LIGHTS_LATE_SHIFT);
            break;
        case FI_PANEL:
            if (hr(s, 9u) < 0.60f) it->bit[0] |= BIT(LIGHTS_AFTER_HOURS);
            if (in) it->bit[0] |= BIT(LIGHTS_LATE_SHIFT);
            break;
        case FI_ROOM:
            if (hr(s, 10u) < 0.55f) it->bit[0] |= BIT(LIGHTS_AFTER_HOURS);
            if (in) it->bit[0] |= BIT(LIGHTS_LATE_SHIFT);
            break;
        case FI_LOUNGE:
            if (hr(s, 11u) < 0.9f) it->bit[0] |= BIT(LIGHTS_AFTER_HOURS);
            if (in || hr(s, 12u) < 0.5f) it->bit[0] |= BIT(LIGHTS_LATE_SHIFT);
            break;
        case FI_WALL:
            if (hr(s, 13u) < 0.7f) it->bit[0] |= BIT(LIGHTS_AFTER_HOURS);
            if (in) it->bit[0] |= BIT(LIGHTS_LATE_SHIFT);
            break;
        case FI_CORE:
            it->bit[0] = BIT(LIGHTS_AFTER_HOURS) | BIT(LIGHTS_LATE_SHIFT);
            break;
        case FI_TROFFER:
            it->bit[0] = BIT(LIGHTS_CLEANERS);
            break;
        default: break;
        }
    }
    /* every zone keeps something lit after hours, and a screen on in the screens state */
    for (int z = 0; z < P->nzone; ++z) {
        int lit = 0, screens = 0, firstFit = -1, firstDesk = -1;
        for (int i = 0; i < P->nitem; ++i) {
            FloorItem *it = &P->item[i];
            if (it->zone != z || it->kind == FI_TROFFER || it->kind == FI_CORE) continue;
            if (it->kind == FI_PENDANT || it->kind == FI_LOUNGE) { if (firstFit < 0) firstFit = i; lit += it->bit[0] & BIT(LIGHTS_AFTER_HOURS); }
            if (it->kind == FI_DESK) {
                lit += it->bit[2] & BIT(LIGHTS_AFTER_HOURS);
                screens += (it->bit[0] | it->bit[1]) & BIT(LIGHTS_SCREENS);
                if (firstDesk < 0 && it->variant) firstDesk = i;
            }
        }
        if (!lit && firstFit >= 0) P->item[firstFit].bit[0] |= BIT(LIGHTS_AFTER_HOURS);
        if (!screens && firstDesk >= 0) P->item[firstDesk].bit[0] |= BIT(LIGHTS_SCREENS);
    }
    light_list(P, minf_(2.62f, P->ceil - 0.78f));
}

/* ------------------------------------------------------------------ plan --- */

/* How far a ray from (x, z) at this yaw (radians from +z) runs before something tall stops it. */
static float sight(const FloorPlan *P, float x, float z, float yaw) {
    float dx = sinf(yaw), dz = cosf(yaw), best = 25.0f;
    if (P->p.corner && dx < 0.0f) best = minf_(best, x / -dx);   /* the side glass: that's the view out, not the floor */
    for (int i = 0; i < P->nitem; ++i) {
        const FloorItem *it = &P->item[i];
        if (it->kind != FI_COLUMN && it->kind != FI_CORE && it->kind != FI_WALL && it->kind != FI_PLANT && it->kind != FI_FILING) continue;
        for (int k = P->p.corner ? 0 : -2; k <= (P->p.corner ? 0 : 2); ++k) {
            float s = (float)k * P->W, t0 = 0.0f, t1 = 1e9f;
            float a = (it->x0 + s - x) / (fabsf(dx) > 1e-4f ? dx : 1e-4f), b = (it->x1 + s - x) / (fabsf(dx) > 1e-4f ? dx : 1e-4f);
            t0 = maxf_(t0, minf_(a, b)); t1 = minf_(t1, maxf_(a, b));
            a = (it->z0 - z) / dz; b = (it->z1 - z) / dz;
            t0 = maxf_(t0, minf_(a, b)); t1 = minf_(t1, maxf_(a, b));
            if (t0 <= t1 && t0 < best) best = t0;
        }
    }
    return best;
}

/* A place for a still (--beauty auto): just inside the glass, where the most of the floor can be seen. */
static void view_choose(FloorPlan *P) {
    float bestScore = -1.0f;
    P->view.z = 0.5f;
    for (float x = 0.5f; x < (P->p.corner ? P->W * 2.0f : P->W); x += 0.5f)
        for (int y = -2; y <= 2; ++y) {
            float yaw = (float)y * 6.0f * PI_F_ / 180.0f, score = 0.0f;
            for (int r = -4; r <= 4; ++r) score += sight(P, x, 0.5f, yaw + (float)r * 6.0f * PI_F_ / 180.0f);
            if (score > bestScore) { bestScore = score; P->view.x = x; P->viewYaw = (float)y * 6.0f; }
        }
}

static const int PLANTS_BY_STYLE[FS_COUNT] = { 3, 2, 4, 6, 1 };

static void plan_once(FloorPlan *P, const FloorParams *p, uint32_t seed) {
    memset(P, 0, sizeof(*P));
    P->p = *p;
    G gs;
    G *g = &gs;
    memset(g, 0, sizeof(*g));
    g->P = P;
    g->rng = hmix(seed, 0xF100u) | 1u;
    for (int i = 0; i < 4; ++i) (void)draw(g);
    P->W = (float)p->bays * p->bayW;
    P->ceil = p->H - 0.6f;
    P->xMax = p->corner ? P->W * 3.0f : P->W;
    P->zMax = p->core == FLOOR_CORE_BACK ? p->D - 0.5f : p->D;
    P->lane = p->corner || draw(g) < 0.5f ? 1.0f : -1.0f;
    float rs = draw(g);
    g->dw = p->style == FS_EXEC ? 1.9f : p->style == FS_DENSE ? 1.25f : p->style == FS_OPEN ? (rs < 0.5f ? 1.4f : 1.6f) : 1.6f;

    /* the column lines, and the cells between them */
    int structBays = p->bays % 2 == 0 ? 2 : p->bays;
    if (p->style == FS_EXEC && draw(g) < 0.4f) structBays = p->bays;
    float Sx = (float)structBays * p->bayW;
    if (p->corner) {
        for (float x = Sx; x < P->xMax - 1.5f && P->ncolX < FLOOR_MAX_LINES; x += Sx) P->colX[P->ncolX++] = x;
        float prev = -1e9f;
        for (int i = 0; i <= P->ncolX; ++i) {
            Cell *c = &g->cell[g->ncell++];
            c->x0 = i == 0 ? 1.0f : prev + LANE;
            c->x1 = i == P->ncolX ? P->xMax - 1.0f : P->colX[i] - TIGHT;
            prev = i < P->ncolX ? P->colX[i] : prev;
        }
    } else {
        for (float x = 0.0f; x < P->W - 0.1f && P->ncolX < FLOOR_MAX_LINES; x += Sx) P->colX[P->ncolX++] = x;
        for (int i = 0; i < P->ncolX; ++i) {
            Cell *c = &g->cell[g->ncell++];
            c->x0 = P->colX[i] + (P->lane > 0.0f ? LANE : TIGHT);
            c->x1 = P->colX[i] + Sx - (P->lane > 0.0f ? TIGHT : LANE);
        }
    }

    /* the core, and the bands either side of it */
    float zStart = p->style == FS_EXEC ? 2.0f : 1.0f, zEnd = p->core == FLOOR_CORE_BACK ? P->zMax - 1.0f : P->zMax - 1.0f;
    float coreZ0 = 0.0f, coreZ1 = 0.0f;
    int coreCell = -1;
    if (p->core == FLOOR_CORE_CENTRE) {
        float d = 5.5f + draw(g) * 2.0f;
        coreZ0 = floorf((p->D - d) * 0.5f * 10.0f) / 10.0f;
        coreZ1 = coreZ0 + d;
        coreCell = (int)(draw(g) * (float)g->ncell);
        bands_fill(g, zStart, coreZ0 - AISLE, true);
        g->band[g->nband].z0 = coreZ0; g->band[g->nband].z1 = coreZ1; g->band[g->nband].prog = P_ZRUN;
        g->nband++;
        bands_fill(g, coreZ1 + AISLE, zEnd, false);
    } else {
        bands_fill(g, zStart, zEnd, true);
    }

    /* the columns first: the programs' own rects must keep clear of them, and the rows sit inside bands */
    float Sz = 6.0f + draw(g) * 2.5f;
    for (float z = Sz * 0.7f; z < P->zMax - 1.0f && P->ncolZ < FLOOR_MAX_LINES; z += Sz) {
        float c = column_z(g, z, P->zMax - 1.0f);
        if (P->ncolZ && c - P->colZ[P->ncolZ - 1] < 3.0f) continue;
        P->colZ[P->ncolZ++] = c;
    }
    for (int r = 0; r < P->ncolZ; ++r)
        for (int c = 0; c < P->ncolX; ++c) {
            float x = P->colX[c], z = P->colZ[r];
            item_add(g, FI_COLUMN, -1, 0, x - 0.3f, z - 0.3f, x + 0.3f, z + 0.3f);
            solid_add(g, x - 0.3f, z - 0.3f, x + 0.3f, z + 0.3f);
        }

    /* the programs, cell by cell */
    int deepest = 0;
    for (int b = 1; b < g->nband; ++b) if (g->band[b].z1 - g->band[b].z0 > g->band[deepest].z1 - g->band[deepest].z0) deepest = b;
    static const int CAP_L[FS_COUNT] = { 1, 1, 2, 2, 0 }, CAP_R[FS_COUNT] = { 1, 1, 3, 2, 1 };
    int deep = p->D >= 24.0f ? 1 : 0, capLounge = CAP_L[p->style] ? CAP_L[p->style] + deep : 0, capRoom = CAP_R[p->style] + deep;
    for (int b = 0; b < g->nband; ++b) {
        const Band *bd = &g->band[b];
        bool isCore = coreCell >= 0 && fabsf(bd->z0 - coreZ0) < 1e-3f;
        for (int c = 0; c < g->ncell; ++c) {
            const Cell *cl = &g->cell[c];
            if (isCore && c == coreCell) {
                int zone = zone_add(g, FZ_STORE, cl->x0, coreZ0, cl->x1, coreZ1);
                float w = cl->x1 - cl->x0, cw = minf_(w, maxf_(4.0f, w * 0.55f)), xa = cl->x0 + (w - cw) * draw(g), xb = xa + cw;
                item_add(g, FI_CORE, zone, 0, xa, coreZ0, xb, coreZ1);
                solid_add(g, xa, coreZ0, xb, coreZ1);
                core_doors(g, zone, xa, xb, coreZ0, 2);
                core_doors(g, zone, xa, xb, coreZ1, 0);
                continue;
            }
            /* different programs side by side, on the floors that mix them */
            int prog = bd->prog;
            float r = draw(g), dz = bd->z1 - bd->z0;
            if ((p->style == FS_COLLAB || p->style == FS_EXEC) && r < 0.4f && !isCore) {
                int alt[4], na = 0;
                if (dz >= 4.0f) alt[na++] = P_ROOM;
                if (dz >= 3.1f) alt[na++] = P_LOUNGE;
                if (dz >= DESK_D * 2.0f + 0.02f) alt[na++] = P_BENCH;
                if (na) { float r2 = draw(g); prog = alt[(int)(r2 * (float)na)]; }
            }
            if (isCore) prog = dz >= 4.0f && p->style == FS_COLLAB ? P_ROOM : P_ZRUN;   /* beside the core, whatever fits */
            /* a floor has so many rooms and lounges, and desks in its deepest band if nothing has given it any */
            if (prog == P_LOUNGE && zones_of(P, FZ_LOUNGE) >= capLounge) prog = dz >= 4.0f && zones_of(P, FZ_ROOM) < capRoom ? P_ROOM : P_BENCH;
            if (prog == P_ROOM && zones_of(P, FZ_ROOM) >= capRoom) prog = P_BENCH;
            /* a wide cell takes a room or lounge at one end and desks at the other */
            float x0 = cl->x0, x1 = cl->x1;
            bool wide = (prog == P_ROOM || prog == P_LOUNGE) && x1 - x0 >= 8.0f;
            if (!wide && b == deepest && c == g->ncell - 1 && !zones_of(P, FZ_WORK)) prog = P_BENCH;
            /* a run or two of cabinets is enough: past that the band is left as open floor */
            if (prog == P_FILING) {
                int runs = 0;
                for (int k = 0; k < P->nitem; ++k) runs += P->item[k].kind == FI_FILING;
                if (runs >= 2 && dz < DESK_D + 0.02f) continue;
                if (runs >= 2) prog = P_SINGLE;
            }
            if (wide) {
                float split = prog == P_ROOM ? 5.2f : 4.3f;
                bool atStart = draw(g) < 0.5f;
                if (atStart) {
                    fill(g, prog, x0, bd->z0, x0 + split, bd->z1);
                    fill(g, P_BENCH, x0 + split + 1.0f, bd->z0, x1, bd->z1);
                } else {
                    fill(g, P_BENCH, x0, bd->z0, x1 - split - 1.0f, bd->z1);
                    fill(g, prog, x1 - split, bd->z0, x1, bd->z1);
                }
                continue;
            }
            fill(g, prog, x0, bd->z0, x1, bd->z1);
        }
    }

    /* the core wall across the back, the end wall of a corner, and what stands on them */
    if (p->core == FLOOR_CORE_BACK) {
        int zone = zone_add(g, FZ_STORE, 0.0f, P->zMax - 2.2f, P->xMax, P->zMax);
        float a = p->corner ? 0.0f : -0.5f, b = p->corner ? P->xMax : P->W + 0.5f;
        item_add(g, FI_CORE, zone, 0, a, P->zMax, b, P->zMax + 2.0f);
        solid_add(g, a, P->zMax, b, P->zMax + 2.0f);
        core_doors(g, zone, p->corner ? 1.0f : 0.5f, p->corner ? P->xMax - 1.0f : P->W - 0.5f, P->zMax, 2);
    }
    if (p->corner) {
        item_add(g, FI_CORE, -1, 3, P->xMax, -2.0f, P->xMax + 1.5f, p->D + 2.0f);
        solid_add(g, P->xMax, -2.0f, P->xMax + 1.5f, p->D + 2.0f);
    }

    /* where a person should be able to walk: every aisle and lane, whatever stands near them */
    for (int b = 0; b <= g->nband; ++b) {
        float za = b == 0 ? 0.0f : g->band[b - 1].z1, zb = b == g->nband ? P->zMax : g->band[b].z0, zc = (za + zb) * 0.5f;
        for (float x = p->corner ? 1.0f : 0.5f; x < P->xMax - 0.5f; x += 1.5f) walk_add(g, x, zc);
    }
    for (int c = 0; c < P->ncolX; ++c)
        for (float z = 1.0f; z < P->zMax - 1.0f; z += 1.5f) walk_add(g, P->colX[c] + P->lane * 0.75f, z);

    /* services: ducts along z, trays over the aisles, panels hung on a grid clear of ducts and walls */
    P->ductBottom = maxf_(P->ceil - 0.95f, 2.45f);
    for (int c = 0; c < P->ncolX; ++c)
        if (P->nduct < FLOOR_MAX_LINES) P->ductX[P->nduct++] = P->colX[c] + Sx * 0.5f;
    for (int b = 0; b + 1 < g->nband && P->ntray < FLOOR_MAX_LINES; ++b) {
        if (b % 2 == 0 || g->nband < 4) P->trayZ[P->ntray++] = (g->band[b].z1 + g->band[b + 1].z0) * 0.5f;
    }
    int nx = (int)floorf(P->xMax / 3.0f + 0.5f);
    if (nx < 1) nx = 1;
    float stepX = P->xMax / (float)nx;
    for (float z = 1.8f; z < P->zMax - 0.7f; z += 3.6f)
        for (int i = 0; i < nx; ++i) {
            float x = ((float)i + 0.5f) * stepX;
            bool clear = true;
            for (int d = 0; d < P->nduct && clear; ++d) if (fabsf(x - P->ductX[d]) < 1.0f) clear = false;
            for (int k = 0; k < P->nitem && clear; ++k) {
                const FloorItem *it = &P->item[k];
                bool wall = it->kind == FI_ROOM || it->kind == FI_CORE || it->kind == FI_WALL;
                if (wall && x + 0.7f > it->x0 && x - 0.7f < it->x1 && z + 0.7f > it->z0 && z - 0.7f < it->z1 &&
                    !(it->kind == FI_ROOM && x - 0.7f > it->x0 && x + 0.7f < it->x1 && z - 0.7f > it->z0 && z + 0.7f < it->z1)) clear = false;
            }
            if (clear) item_add(g, FI_TROFFER, -1, 0, x - 0.62f, z - 0.62f, x + 0.62f, z + 0.62f);
        }

    /* plants, where a pot and a person both still fit */
    if (p->style == FS_EXEC)
        for (int c = 0; c < P->ncolX; ++c) spot_add(g, P->colX[c] + (P->lane > 0.0f ? -1.0f : 1.0f) * 0.9f, 0.8f);
    int want = PLANTS_BY_STYLE[p->style], placed = 0;
    for (int i = g->nspot - 1; i > 0; --i) {   /* shuffled */
        float r = draw(g);
        int j = (int)(r * (float)(i + 1));
        FloorPoint t = g->spot[i]; g->spot[i] = g->spot[j]; g->spot[j] = t;
    }
    char why[8];
    for (int i = 0; i < g->nspot && placed < want; ++i) {
        float x = g->spot[i].x, z = g->spot[i].z, r = 0.26f;
        if (z < 0.65f || z > P->zMax - 0.65f || x < (p->corner ? 0.65f : -1e6f) || x > (p->corner ? P->xMax - 0.65f : 1e6f)) continue;
        if (!rect_free(P, x - r, z - r, x + r, z + r, 0.03f)) continue;
        int nitem = P->nitem, nsolid = P->nsolid;
        FloorItem *it = item_add(g, FI_PLANT, -1, 0, x - r, z - r, x + r, z + r);
        solid_add(g, x - r, z - r, x + r, z + r);
        float s = draw(g), h = draw(g);
        it->variant = (int)(s * (float)PS_COUNT) % PS_COUNT;
        it->a = 1.2f + h * 0.75f;
        it->ox = x; it->oz = z;
        if (!floor_validate(P, why, 0, NULL)) {
            P->nitem = nitem; P->nsolid = nsolid;   /* it would block something */
            continue;
        }
        placed++;
    }

    lights_assign(g);

    view_choose(P);
}

/* ------------------------------------------------------------- validation --- */

/* Marks every grid cell within r of a solid, the glass or an end. */
static void raster(const FloorPlan *P, float r, uint8_t *blocked, int nx, int nz) {
    bool periodic = !P->p.corner;
    memset(blocked, 0, (size_t)nx * nz);
    for (int j = 0; j < nz; ++j) {
        float z = ((float)j + 0.5f) * GRID;
        if (z < r || z > P->zMax - r) memset(blocked + (size_t)j * nx, 1, (size_t)nx);
    }
    if (!periodic)
        for (int j = 0; j < nz; ++j)
            for (int i = 0; i < nx; ++i) {
                float x = ((float)i + 0.5f) * GRID;
                if (x < r || x > P->xMax - r) blocked[(size_t)j * nx + i] = 1;
            }
    for (int k = 0; k < P->nsolid; ++k) {
        const FloorRect *s = &P->solid[k];
        int j0 = (int)floorf((s->z0 - r) / GRID), j1 = (int)ceilf((s->z1 + r) / GRID);
        int i0 = (int)floorf((s->x0 - r) / GRID), i1 = (int)ceilf((s->x1 + r) / GRID);
        if (j0 < 0) j0 = 0;
        if (j1 > nz) j1 = nz;
        for (int j = j0; j < j1; ++j) {
            float z = ((float)j + 0.5f) * GRID;
            if (z < s->z0 - r || z > s->z1 + r) continue;
            for (int i = i0; i < i1; ++i) {
                float x = ((float)i + 0.5f) * GRID;
                if (x < s->x0 - r || x > s->x1 + r) continue;
                int w = i;
                if (periodic) { w %= nx; if (w < 0) w += nx; }
                else if (w < 0 || w >= nx) continue;
                blocked[(size_t)j * nx + w] = 1;
            }
        }
    }
}

/* Floods the free cells from the start; returns how many it reached. out: 2 where reached. */
static int flood(const FloorPlan *P, uint8_t *cells, int nx, int nz, float sx, float sz) {
    bool periodic = !P->p.corner;
    int i0 = (int)(sx / GRID), j0 = (int)(sz / GRID);
    if (periodic) { i0 %= nx; if (i0 < 0) i0 += nx; }
    /* the nearest free cell to the start */
    int best = -1;
    float bd = 1e9f;
    for (int j = 0; j < nz; ++j)
        for (int i = 0; i < nx; ++i)
            if (!cells[(size_t)j * nx + i]) {
                float dx = (float)(i - i0), dz = (float)(j - j0), d = dx * dx + dz * dz;
                if (d < bd) { bd = d; best = j * nx + i; }
            }
    if (best < 0) return 0;
    int *stack = malloc(sizeof(int) * (size_t)nx * nz), sp = 0, count = 0;
    if (!stack) return 0;
    stack[sp++] = best;
    cells[best] = 2;
    while (sp) {
        int c = stack[--sp], i = c % nx, j = c / nx;
        count++;
        int nb[4] = { -1, -1, j > 0 ? c - nx : -1, j < nz - 1 ? c + nx : -1 };
        if (i > 0) nb[0] = c - 1; else if (periodic) nb[0] = c + nx - 1;
        if (i < nx - 1) nb[1] = c + 1; else if (periodic) nb[1] = c - nx + 1;
        for (int k = 0; k < 4; ++k)
            if (nb[k] >= 0 && !cells[nb[k]]) { cells[nb[k]] = 2; stack[sp++] = nb[k]; }
    }
    free(stack);
    return count;
}

static bool near_flooded(const FloorPlan *P, const uint8_t *cells, int nx, int nz, float x, float z, float within) {
    bool periodic = !P->p.corner;
    int rad = (int)ceilf(within / GRID), ic = (int)floorf(x / GRID), jc = (int)floorf(z / GRID);
    for (int j = jc - rad; j <= jc + rad; ++j) {
        if (j < 0 || j >= nz) continue;
        for (int i = ic - rad; i <= ic + rad; ++i) {
            int w = i;
            if (periodic) { w %= nx; if (w < 0) w += nx; }
            else if (w < 0 || w >= nx) continue;
            if (cells[(size_t)j * nx + w] == 2) return true;
        }
    }
    return false;
}

float floor_lux(const FloorPlan *P, int state, float x, float z) {
    float E = 0.0f;
    for (int i = 0; i < P->nlight; ++i) {
        const FloorLight *l = &P->light[i];
        if (!(l->bits & (1u << state))) continue;
        for (int k = P->p.corner ? 0 : -1; k <= (P->p.corner ? 0 : 1); ++k) {
            float dx = l->x + (float)k * P->W - x, dz = l->z - z, dy = l->y, d2 = dx * dx + dy * dy + dz * dz;
            E += l->lumens / PI_F_ * dy * dy / (d2 * d2);
        }
    }
    return E;
}

#define COMPLAIN(...) do { if (why && whySize > 0) snprintf(why, (size_t)whySize, __VA_ARGS__); ok = false; goto done; } while (0)

/* The screens on in a state within d metres of a point. */
static int screens_near(const FloorPlan *P, int state, float x, float z, float d) {
    int n = 0;
    for (int i = 0; i < P->nitem; ++i) {
        const FloorItem *it = &P->item[i];
        if (it->kind != FI_DESK || !((it->bit[0] | it->bit[1]) & (1u << state))) continue;
        float dx = it->ox - x, dz = it->oz - z;
        if (!P->p.corner) dx -= P->W * floorf(dx / P->W + 0.5f);
        n += dx * dx + dz * dz < d * d;
    }
    return n;
}

bool floor_validate(const FloorPlan *P, char *why, int whySize, uint8_t *reached) {
    bool ok = true, periodic = !P->p.corner, lightsReady = P->nlight > 0;
    int nx = (int)floorf(P->xMax / GRID), nz = (int)floorf(P->zMax / GRID);
    uint8_t *cells = malloc((size_t)nx * nz);
    if (!cells) return false;
    if (P->p.D < 7.0f || P->W <= 0.0f || P->ceil < 2.8f) COMPLAIN("the floor's size is out of range");
    if (P->ductBottom < 2.3f) COMPLAIN("the ducts hang %.2f m over the floor", P->ductBottom);

    /* nothing intersects */
    for (int a = 0; a < P->nsolid; ++a) {
        const FloorRect *s = &P->solid[a];
        for (int b = a + 1; b < P->nsolid; ++b) {
            const FloorRect *t = &P->solid[b];
            if (s->item == t->item) continue;
            for (int k = periodic ? -1 : 0; k <= (periodic ? 1 : 0); ++k) {
                float d = (float)k * P->W;
                float ox = minf_(s->x1, t->x1 + d) - maxf_(s->x0, t->x0 + d), oz = minf_(s->z1, t->z1) - maxf_(s->z0, t->z0);
                /* a pair of desks may touch at the screen between them: 1 mm is the tolerance */
                if (ox > 0.001f && oz > 0.001f) {
                    const FloorItem *A = &P->item[s->item], *B = &P->item[t->item];
                    COMPLAIN("kinds %d and %d intersect by %.2f x %.2f m at (%.1f, %.1f)", A->kind, B->kind, ox, oz, s->x0, s->z0);
                }
            }
        }
    }

    /* the walkways: every lane and aisle at 0.8 m wide; every seat, door and front at 0.7 m */
    if (periodic || P->ncolX) {
        raster(P, WALK_R, cells, nx, nz);
        flood(P, cells, nx, nz, P->ncolX ? P->colX[0] + P->lane * 0.75f : 1.0f, 0.5f);
        int missed = 0;
        for (int i = 0; i < P->nwalk; ++i) missed += !near_flooded(P, cells, nx, nz, P->walk[i].x, P->walk[i].z, 0.75f);
        if (missed * 20 > P->nwalk) COMPLAIN("%d of %d walkway points are cut off", missed, P->nwalk);
    }
    raster(P, REACH_R, cells, nx, nz);
    int total = 0;
    for (size_t i = 0; i < (size_t)nx * nz; ++i) total += !cells[i];
    int reachedN = flood(P, cells, nx, nz, P->ncolX ? P->colX[0] + P->lane * 0.75f : 1.0f, 0.5f);
    for (int i = 0; i < P->nreach; ++i)
        if (!near_flooded(P, cells, nx, nz, P->reach[i].x, P->reach[i].z, REACH_NEAR))
            COMPLAIN("no way to (%.1f, %.1f)", P->reach[i].x, P->reach[i].z);
    if (total && reachedN * 100 < total * 80) COMPLAIN("only %d%% of the free floor can be walked to", reachedN * 100 / total);
    if (reached) memcpy(reached, cells, (size_t)nx * nz);

    /* light: only once the plan has its lights (the plants are placed before them) */
    if (lightsReady) {
        int work = 0;
        for (int z = 0; z < P->nzone; ++z) work += P->zone[z].kind == FZ_WORK;
        if (!work) COMPLAIN("no desks");
        int desks = 0;
        for (int i = 0; i < P->nitem; ++i) desks += P->item[i].kind == FI_DESK;
        float perDesk = P->p.style == FS_EXEC ? 50.0f : 30.0f;   /* square metres of floor, at most: it's an office */
        if ((float)desks * perDesk < P->xMax * P->zMax) COMPLAIN("only %d desks on %.0f square metres", desks, P->xMax * P->zMax);
        for (int z = 0; z < P->nzone; ++z) {
            const FloorZone *Z = &P->zone[z];
            float cx = (Z->x0 + Z->x1) * 0.5f, cz = (Z->z0 + Z->z1) * 0.5f;
            bool byGlass = cz < 8.0f || (P->p.core != FLOOR_CORE_BACK && P->zMax - cz < 8.0f) || (P->p.corner && cx < 8.0f);
            if (floor_lux(P, LIGHTS_AFTER_HOURS, cx, cz) < 8.0f) COMPLAIN("a %s zone at (%.1f, %.1f) is dark after hours", floor_zone_name(Z->kind), cx, cz);
            if (floor_lux(P, LIGHTS_CLEANERS, cx, cz) < 20.0f) COMPLAIN("a %s zone at (%.1f, %.1f) is dark for the cleaners", floor_zone_name(Z->kind), cx, cz);
            if (!byGlass && floor_lux(P, LIGHTS_LATE_SHIFT, cx, cz) < 2.0f && !screens_near(P, LIGHTS_LATE_SHIFT, cx, cz, 10.0f))
                COMPLAIN("a %s zone at (%.1f, %.1f) is dark on the late shift", floor_zone_name(Z->kind), cx, cz);
            if (!byGlass && !screens_near(P, LIGHTS_SCREENS, cx, cz, 10.0f))
                COMPLAIN("a %s zone at (%.1f, %.1f) has no glow in the screens state", floor_zone_name(Z->kind), cx, cz);
        }
    }
done:
    free(cells);
    return ok;
}

bool floor_plan(const FloorParams *p, FloorPlan *P) {
    char why[200] = "";
    for (int a = 0; a < 8; ++a) {
        uint32_t seed = hmix(p->seed, (uint32_t)a);
        plan_once(P, p, seed);
        P->attempts = a + 1;
        if (floor_validate(P, why, sizeof(why), NULL)) { P->ok = true; P->why[0] = 0; return true; }
    }
    P->ok = false;
    snprintf(P->why, sizeof(P->why), "%s", why);
    return false;
}

uint64_t floor_hash(const FloorPlan *P) {
    uint64_t h = 14695981039346656037ull;
    const uint8_t *b = (const uint8_t *)P->item;
    size_t n = sizeof(FloorItem) * (size_t)P->nitem;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
