#include "material_def.h"
#include "json.h"
#include <stdio.h>
#include <string.h>

const char *const PBR_MATERIAL_NAMES[PBR_MATERIAL_COUNT] = {
#include "pbr_material_names.inc"
};

const char *const PBR_MAP_KEYS[PBR_MAPS_PER_MATERIAL] = {
    [PBR_ALBEDO] = "albedo", [PBR_NORMAL] = "normal", [PBR_ORM] = "orm", [PBR_EMISSIVE] = "emissive",
};

bool material_map_required(int map) { return map != PBR_EMISSIVE; }

/* dir/rel with "." and ".." segments taken out, so two materials naming one
 * file by different routes name it the same way (the renderer loads it once). */
static void resolve(const char *dir, const char *rel, char *out, size_t size) {
    char joined[2048];
    bool absolute = rel[0] == '/' || rel[0] == '\\' || (rel[0] && rel[1] == ':');
    snprintf(joined, sizeof(joined), absolute ? "%s%s" : "%s/%s", absolute ? "" : dir, rel);
    for (char *c = joined; *c; ++c) if (*c == '\\') *c = '/';
    /* split into segments, dropping "." and folding ".." into the one before */
    char *seg[256];
    int n = 0;
    bool rooted = joined[0] == '/';
    for (char *tok = strtok(joined, "/"); tok && n < 256; tok = strtok(NULL, "/")) {
        if (strcmp(tok, ".") == 0) continue;
        if (strcmp(tok, "..") == 0 && n > 0 && strcmp(seg[n - 1], "..") != 0) { n--; continue; }   /* a leading ".." stays */
        seg[n++] = tok;
    }
    size_t len = 0;
    out[0] = 0;
    if (rooted && len + 1 < size) out[len++] = '/', out[len] = 0;
    for (int i = 0; i < n; ++i) len += (size_t)snprintf(out + len, len < size ? size - len : 0, i ? "/%s" : "%s", seg[i]);
}

/* Reads one or `n` numbers into v (one number fills all n). */
static bool numbers(const Json *j, float *v, int lo, int n, const char *key, char *err, size_t errSize) {
    if (j->type == JSON_NUMBER) {
        for (int i = 0; i < n; ++i) v[i] = (float)j->number;
        return true;
    }
    int count = json_count(j);
    if (j->type != JSON_ARRAY || count < lo || count > n) {
        if (lo == n) snprintf(err, errSize, "line %d: \"%s\" must be a number or %d numbers", j->line, key, n);
        else snprintf(err, errSize, "line %d: \"%s\" must be %d to %d numbers", j->line, key, lo, n);
        return false;
    }
    for (int i = 0; i < count; ++i) {
        const Json *e = json_at(j, i);
        if (e->type != JSON_NUMBER) { snprintf(err, errSize, "line %d: \"%s\" holds %s", e->line, key, json_type_name(e->type)); return false; }
        v[i] = (float)e->number;
    }
    return true;
}

bool material_def_load(const char *path, MaterialDef *def, char *err, size_t errSize) {
    static const MaterialDef DEFAULTS = {
        .tile = { 1.0f, 1.0f }, .albedoScale = 1.0f, .albedoMean = { 0.5f, 0.5f, 0.5f }, .relief = { 0.5f, 1.0f },
    };
    *def = DEFAULTS;
    char why[256];
    Json *doc = json_load(path, why, sizeof(why));
    if (!doc) { snprintf(err, errSize, "%s", why); return false; }

    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/'), *bs = strrchr(dir, '\\');
    if (bs > slash) slash = bs;
    if (slash) *slash = 0; else snprintf(dir, sizeof(dir), ".");

    bool ok = doc->type == JSON_OBJECT;
    if (!ok) snprintf(err, errSize, "the file holds %s, not an object", json_type_name(doc->type));
    for (const Json *m = ok ? doc->child : NULL; m && ok; m = m->next) {
        if (strcmp(m->key, "maps") == 0) {
            if (m->type != JSON_OBJECT) { snprintf(err, errSize, "line %d: \"maps\" must be an object", m->line); ok = false; break; }
            for (const Json *e = m->child; e && ok; e = e->next) {
                int k = 0;
                while (k < PBR_MAPS_PER_MATERIAL && strcmp(e->key, PBR_MAP_KEYS[k]) != 0) k++;
                if (k == PBR_MAPS_PER_MATERIAL) { snprintf(err, errSize, "line %d: unknown map \"%s\"", e->line, e->key); ok = false; }
                else if (e->type != JSON_STRING || !e->string[0]) { snprintf(err, errSize, "line %d: map \"%s\" must be a file name", e->line, e->key); ok = false; }
                else resolve(dir, e->string, def->maps[k], sizeof(def->maps[k]));
            }
        }
        else if (strcmp(m->key, "tile") == 0)         ok = numbers(m, def->tile, 2, 2, m->key, err, errSize);
        else if (strcmp(m->key, "albedo_scale") == 0) ok = numbers(m, &def->albedoScale, 1, 1, m->key, err, errSize);
        else if (strcmp(m->key, "albedo_mean") == 0)  ok = numbers(m, def->albedoMean, 3, 3, m->key, err, errSize);
        else if (strcmp(m->key, "relief") == 0)       ok = numbers(m, def->relief, 2, 2, m->key, err, errSize);
        else if (strcmp(m->key, "params") == 0)       ok = m->type == JSON_ARRAY ? numbers(m, def->params, 1, 4, m->key, err, errSize)
                                                         : (snprintf(err, errSize, "line %d: \"params\" must be an array", m->line), false);
        else { snprintf(err, errSize, "line %d: unknown key \"%s\"", m->line, m->key); ok = false; }
    }
    for (int k = 0; k < PBR_MAPS_PER_MATERIAL && ok; ++k)
        if (material_map_required(k) && !def->maps[k][0]) { snprintf(err, errSize, "no \"%s\" map", PBR_MAP_KEYS[k]); ok = false; }
    if (ok && (def->tile[0] <= 0.0f || def->tile[1] <= 0.0f)) { snprintf(err, errSize, "\"tile\" must be positive"); ok = false; }
    if (ok && def->relief[1] <= 0.0f) { snprintf(err, errSize, "\"relief\" spread must be positive"); ok = false; }
    for (int i = 0; i < 3 && ok; ++i)
        if (def->albedoMean[i] <= 0.0f) { snprintf(err, errSize, "\"albedo_mean\" must be positive"); ok = false; }
    json_free(doc);
    return ok;
}
