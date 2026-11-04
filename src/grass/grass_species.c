#include "grass_species.h"
#include "json.h"
#include "assets.h"
#include <stdio.h>
#include <string.h>

/* The grass's species profiles (grass_species.h). */

const char *const GRASS_SPECIES_NAMES[GRASS_SPECIES] = { "meadow", "reeds", "lawn", "rough" };

static GrassProfile g_profiles[GRASS_SPECIES];

const GrassProfile *grass_profile(int k) { return &g_profiles[k]; }

/* n numbers from an array of exactly n (or, where n is 1, a number). */
static bool nums(const Json *j, float *v, int n, char *err, size_t errSize) {
    if (n == 1 && j->type == JSON_NUMBER) { v[0] = (float)j->number; return true; }
    if (j->type != JSON_ARRAY || json_count(j) != n) {
        if (n == 1) snprintf(err, errSize, "line %d: \"%s\" must be a number", j->line, j->key);
        else snprintf(err, errSize, "line %d: \"%s\" must be %d numbers", j->line, j->key, n);
        return false;
    }
    for (int i = 0; i < n; ++i) {
        const Json *e = json_at(j, i);
        if (e->type != JSON_NUMBER) { snprintf(err, errSize, "line %d: \"%s\" holds %s", e->line, j->key, json_type_name(e->type)); return false; }
        v[i] = (float)e->number;
    }
    return true;
}

static bool ints(const Json *j, int *v, int n, char *err, size_t errSize) {
    float f[8];
    if (!nums(j, f, n, err, errSize)) return false;
    for (int i = 0; i < n; ++i) {
        v[i] = (int)f[i];
        if ((float)v[i] != f[i]) { snprintf(err, errSize, "line %d: \"%s\" must be whole numbers", j->line, j->key); return false; }
    }
    return true;
}

typedef struct Key { const char *name; int kind; int n; size_t at; } Key;   /* kind 0 floats, 1 ints */
#define F(name, n, field) { name, 0, n, offsetof(GrassProfile, field) }
#define I(name, n, field) { name, 1, n, offsetof(GrassProfile, field) }

/* The members of obj, each one of keys (all required). */
static bool members(const Json *obj, const char *what, const Key *keys, int nk, GrassProfile *p, char *err, size_t errSize) {
    if (obj->type != JSON_OBJECT) { snprintf(err, errSize, "line %d: \"%s\" must be an object", obj->line, what); return false; }
    unsigned seen = 0;
    for (const Json *m = obj->child; m; m = m->next) {
        int k = 0;
        while (k < nk && strcmp(m->key, keys[k].name) != 0) k++;
        if (k == nk) continue;   /* the caller's: nested objects */
        void *at = (char *)p + keys[k].at;
        if (!(keys[k].kind ? ints(m, (int *)at, keys[k].n, err, errSize) : nums(m, (float *)at, keys[k].n, err, errSize))) return false;
        seen |= 1u << k;
    }
    for (int k = 0; k < nk; ++k)
        if (!(seen & (1u << k))) { snprintf(err, errSize, "no \"%s\"%s%s", keys[k].name, obj->key ? " in " : "", obj->key ? obj->key : ""); return false; }
    return true;
}

static bool range(bool ok, const char *what, char *err, size_t errSize) {
    if (!ok) snprintf(err, errSize, "\"%s\" is out of range", what);
    return ok;
}

bool grass_profile_load(const char *path, GrassProfile *p, char *err, size_t errSize) {
    static const Key TOP[] = {
        F("density", 1, density), I("root_grid", 1, rootGrid), I("tuft", 2, tuft), F("tuft_radius", 1, tuftRadius),
        F("height", 2, height), F("width", 1, width), F("lean", 1, lean), F("bend", 1, bend), F("curl", 1, curl),
        F("taper", 1, taper), I("segments", GRASS_BUCKETS, segments), F("stiffness", 1, stiffness),
        F("interaction", 1, interaction), F("fullness", 1, fullness),
    };
    static const Key CLUMPS[] = { I("cells", 1, clumpCells), F("bare", 1, bare), F("vary", 1, clumpVary), F("edge", 1, clumpEdge) };
    static const Key COLOUR[] = { F("root", 3, root), F("tip", 3, tip), F("dry", 3, dry), F("dry_share", 1, dryShare) };
    static const char *const NESTED[] = { "clumps", "colour" };
    memset(p, 0, sizeof(*p));
    char why[256];
    Json *doc = json_load(path, why, sizeof(why));
    if (!doc) { snprintf(err, errSize, "%s", why); return false; }
    bool ok = doc->type == JSON_OBJECT;
    if (!ok) snprintf(err, errSize, "the file holds %s, not an object", json_type_name(doc->type));
    const int nTop = (int)(sizeof(TOP) / sizeof(TOP[0]));
    for (const Json *m = ok ? doc->child : NULL; m && ok; m = m->next) {
        bool known = false;
        for (int k = 0; k < nTop; ++k) known |= strcmp(m->key, TOP[k].name) == 0;
        for (int k = 0; k < 2; ++k) known |= strcmp(m->key, NESTED[k]) == 0;
        if (!known) { snprintf(err, errSize, "line %d: unknown key \"%s\"", m->line, m->key); ok = false; }
    }
    ok = ok && members(doc, "the file", TOP, nTop, p, err, errSize);
    for (int q = 0; q < 2 && ok; ++q) {
        const Json *o = json_get(doc, NESTED[q]);
        if (!o) { snprintf(err, errSize, "no \"%s\"", NESTED[q]); ok = false; break; }
        const Key *keys = q == 0 ? CLUMPS : COLOUR;
        const int nk = q == 0 ? (int)(sizeof(CLUMPS) / sizeof(CLUMPS[0])) : (int)(sizeof(COLOUR) / sizeof(COLOUR[0]));
        for (const Json *m = o->type == JSON_OBJECT ? o->child : NULL; m && ok; m = m->next) {
            bool known = false;
            for (int k = 0; k < nk; ++k) known |= strcmp(m->key, keys[k].name) == 0;
            if (!known) { snprintf(err, errSize, "line %d: unknown key \"%s\" in \"%s\"", m->line, m->key, NESTED[q]); ok = false; }
        }
        ok = ok && members(o, NESTED[q], keys, nk, p, err, errSize);
    }
    ok = ok && range(p->density > 0.0f, "density", err, errSize);
    ok = ok && range(p->rootGrid >= 1 && p->rootGrid <= 256, "root_grid", err, errSize);
    ok = ok && range(p->tuft[0] >= 1 && p->tuft[0] <= p->tuft[1] && p->tuft[1] <= 32, "tuft", err, errSize);
    ok = ok && range(p->tuftRadius >= 0.0f && p->tuftRadius < 0.5f, "tuft_radius", err, errSize);
    ok = ok && range(p->height[0] > 0.0f && p->height[0] <= p->height[1] && p->height[1] < 4.0f, "height", err, errSize);
    ok = ok && range(p->width > 0.0f && p->width < 0.5f, "width", err, errSize);
    ok = ok && range(p->lean >= 0.0f && p->lean < 1.0f, "lean", err, errSize);
    ok = ok && range(p->bend >= 0.0f && p->bend < 0.8f, "bend", err, errSize);
    ok = ok && range(p->curl >= 0.0f && p->curl <= 1.0f, "curl", err, errSize);
    ok = ok && range(p->taper > 0.0f && p->taper <= 4.0f, "taper", err, errSize);
    for (int b = 0; b < GRASS_BUCKETS && ok; ++b) ok = range(p->segments[b] >= 1 && p->segments[b] <= GRASS_SEG_MAX, "segments", err, errSize);
    ok = ok && range(p->clumpCells >= 1 && p->clumpCells <= p->rootGrid, "clumps.cells", err, errSize);
    ok = ok && range(p->bare >= 0.0f && p->bare <= 1.0f, "clumps.bare", err, errSize);
    ok = ok && range(p->clumpVary >= 0.0f && p->clumpVary < 1.0f, "clumps.vary", err, errSize);
    ok = ok && range(p->clumpEdge >= 0.0f && p->clumpEdge <= 1.0f, "clumps.edge", err, errSize);
    ok = ok && range(p->stiffness > 0.0f && p->interaction >= 0.0f && p->fullness >= 0.0f && p->fullness <= 1.0f, "stiffness, interaction or fullness", err, errSize);
    ok = ok && range(p->dryShare >= 0.0f && p->dryShare <= 1.0f, "colour.dry_share", err, errSize);
    for (int i = 0; i < 3 && ok; ++i) ok = range(p->root[i] >= 0.0f && p->tip[i] >= 0.0f && p->dry[i] >= 0.0f && p->root[i] <= 1.0f && p->tip[i] <= 1.0f && p->dry[i] <= 1.0f, "colour", err, errSize);
    json_free(doc);
    p->present = ok;
    return ok;
}

int grass_profiles_load(const char *dir) {
    int n = 0;
    for (int k = 0; k < GRASS_SPECIES; ++k) {
        char path[1100], err[300];
        snprintf(path, sizeof(path), "%s/%s.json", dir, GRASS_SPECIES_NAMES[k]);
        if (grass_profile_load(path, &g_profiles[k], err, sizeof(err))) n++;
        else printf("Grass: %s (%s) left out: %s\n", GRASS_SPECIES_NAMES[k], path, err);
    }
    return n;
}

int grass_profiles_load_assets(const char *root) {
    char roots[2][1024];
    int nr = 0;
    if (root) snprintf(roots[nr++], sizeof(roots[0]), "%s", root);
    else nr = asset_roots(roots);
    for (int i = 0; i < nr; ++i) {
        char dir[1100], probe[1200];
        snprintf(dir, sizeof(dir), "%s/grass", roots[i]);
        snprintf(probe, sizeof(probe), "%s/%s.json", dir, GRASS_SPECIES_NAMES[0]);
        FILE *f = fopen(probe, "rb");
        if (!f && i + 1 < nr) continue;
        if (f) fclose(f);
        return grass_profiles_load(dir);
    }
    return 0;
}

void grass_profiles_gpu(GrassSpeciesGPU out[GRASS_SPECIES]) {
    memset(out, 0, GRASS_SPECIES * sizeof(GrassSpeciesGPU));
    for (int k = 0; k < GRASS_SPECIES; ++k) {
        const GrassProfile *p = &g_profiles[k];
        GrassSpeciesGPU *o = &out[k];
        if (!p->present) { o->n[0] = o->n[1] = o->n[2] = o->n[3] = 1; o->seg[0] = o->seg[1] = o->seg[2] = 1; continue; }
        o->a[0] = p->density; o->a[1] = p->tuftRadius; o->a[2] = p->height[0]; o->a[3] = p->height[1];
        o->b[0] = p->width; o->b[1] = p->lean; o->b[2] = p->bend; o->b[3] = p->bare;
        o->c[0] = p->clumpVary; o->c[1] = p->clumpEdge; o->c[2] = p->taper; o->c[3] = p->curl;
        o->n[0] = p->rootGrid; o->n[1] = p->tuft[0]; o->n[2] = p->tuft[1]; o->n[3] = p->clumpCells;
        for (int b = 0; b < GRASS_BUCKETS; ++b) o->seg[b] = p->segments[b];
        o->seg[3] = 1;
        for (int i = 0; i < 3; ++i) { o->root[i] = p->root[i]; o->tip[i] = p->tip[i]; o->dry[i] = p->dry[i]; }
        o->root[3] = p->stiffness; o->tip[3] = p->interaction; o->dry[3] = p->dryShare;
        o->misc[0] = p->fullness;
    }
}

int grass_bucket_segments(int b) {
    int s = 1;
    for (int k = 0; k < GRASS_SPECIES; ++k)
        if (g_profiles[k].present && g_profiles[k].segments[b] > s) s = g_profiles[k].segments[b];
    return s;
}
