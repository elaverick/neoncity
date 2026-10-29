#include "interior_def.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const INTERIOR_CHANNEL_NAMES[INTERIOR_CHANNELS] = { "lint", "albedo", "depth", "flags" };
/* VK_FORMAT_B10G11R11_UFLOAT_PACK32, R8G8B8A8_UNORM, BC5_UNORM_BLOCK, R8_UNORM */
const uint32_t INTERIOR_CHANNEL_FORMATS[INTERIOR_CHANNELS] = { 122u, 37u, 141u, 9u };

bool interior_catalogue_load(const char *path, char names[][64], int max, int *count, char *err, size_t errSize) {
    *count = 0;
    char why[256];
    Json *doc = json_load(path, why, sizeof(why));
    if (!doc) { snprintf(err, errSize, "%s", why); return false; }
    const Json *mods = json_get(doc, "modules");
    bool ok = mods && mods->type == JSON_ARRAY;
    if (!ok) snprintf(err, errSize, "%s: no \"modules\" list", path);
    for (int i = 0; ok && i < json_count(mods); ++i) {
        const Json *e = json_at(mods, i);
        if (e->type != JSON_STRING || !e->string[0] || strlen(e->string) >= 64) { snprintf(err, errSize, "line %d: a module must be a name", e->line); ok = false; }
        else if (*count >= max) { snprintf(err, errSize, "%s: more than %d modules", path, max); ok = false; }
        else snprintf(names[(*count)++], 64, "%s", e->string);
    }
    json_free(doc);
    return ok;
}

/* n numbers from an array (or a member that's a number, for n = 1). */
static bool nums(const Json *obj, const char *key, float *v, int n) {
    const Json *j = json_get(obj, key);
    if (!j) return false;
    if (n == 1 && j->type == JSON_NUMBER) { v[0] = (float)j->number; return true; }
    if (j->type != JSON_ARRAY || json_count(j) != n) return false;
    for (int i = 0; i < n; ++i) {
        const Json *e = json_at(j, i);
        if (e->type != JSON_NUMBER) return false;
        v[i] = (float)e->number;
    }
    return true;
}

static bool flag(const Json *obj, const char *key, bool *v) {
    const Json *j = json_get(obj, key);
    if (!j || j->type != JSON_BOOL) return false;
    *v = j->number != 0.0;
    return true;
}

static const char *str(const Json *obj, const char *key) {
    const Json *j = json_get(obj, key);
    return j && j->type == JSON_STRING ? j->string : NULL;
}

bool interior_module_load(const char *path, InteriorModule *m, char *err, size_t errSize) {
    memset(m, 0, sizeof(*m));
    char why[256];
    Json *doc = json_load(path, why, sizeof(why));
    if (!doc) { snprintf(err, errSize, "%s", why); return false; }
    const char *bad = NULL;
    float f[4];
    const Json *views = json_get(doc, "views"), *march = json_get(doc, "march"), *grid = json_get(doc, "grid");
    const Json *means = json_get(json_get(doc, "bay_means"), "lint"), *files = json_get(doc, "files");
    const char *type = str(doc, "type"), *kind = views ? str(views, "kind") : NULL, *name = str(doc, "module");
    if (!nums(doc, "version", f, 1) || (int)f[0] != INTERIOR_FORMAT_VERSION) bad = "is not format version 2 (re-bake it)";
    else if (!name || strlen(name) >= sizeof(m->name)) bad = "has no \"module\" name";
    else if (!type || (strcmp(type, "through") && strcmp(type, "core") && strcmp(type, "corner"))) bad = "has no \"type\" (through, core or corner)";
    else if (!nums(doc, "size", m->size, 3) || !nums(doc, "ceiling", &m->ceiling, 1) || !nums(doc, "opening", m->opening, 2)) bad = "lacks its size, ceiling or opening";
    else if (!nums(doc, "bays", f, 1) || !nums(doc, "bay_width", &m->bayWidth, 1) || !nums(doc, "bake_scale", &m->bakeScale, 1)) bad = "lacks its bays or bake scale";
    else if (!views || !kind || !nums(views, "n", f + 2, 2) || !nums(views, "range", m->viewRange, 4)) bad = "lacks its view grid";
    else if (!nums(doc, "layers", &f[1], 1) || !flag(doc, "last_slot", &m->lastSlot)) bad = "lacks its layers";
    else if (!march || !grid || !files || !means) bad = "lacks its march, grid, files or bay means";
    if (!bad) {
        snprintf(m->name, sizeof(m->name), "%s", name);
        m->type = !strcmp(type, "through") ? INTERIOR_THROUGH : !strcmp(type, "core") ? INTERIOR_CORE : INTERIOR_CORNER;
        m->bays = (int)f[0];
        m->layers = (int)f[1];
        m->viewN[0] = (int)f[2]; m->viewN[1] = (int)f[3];
        m->viewKind = !strcmp(kind, "azel") ? 0 : !strcmp(kind, "hemioct") ? 1 : !strcmp(kind, "slope") ? 2 : -1;
        float ms[5], dim[2];
        if (!nums(march, "max_steps", &ms[0], 1) || !nums(march, "texels_a_step", &ms[1], 1) || !nums(march, "bisections", &ms[2], 1) ||
            !nums(march, "guess_weight", &ms[3], 1) || !nums(march, "off_grid_weight", &ms[4], 1)) bad = "lacks a march parameter";
        else if (!nums(grid, "origin", m->origin, 2) || !nums(grid, "texels_per_metre", &m->tpm, 1) || !nums(grid, "size", dim, 2) ||
                 !flag(grid, "wrap_x", &m->wrapX)) bad = "lacks a grid parameter";
        else {
            m->maxSteps = (int)ms[0]; m->texStep = ms[1]; m->bisections = (int)ms[2];
            m->guessWeight = ms[3]; m->offGridWeight = ms[4];
            m->dim[0] = (int)dim[0]; m->dim[1] = (int)dim[1];
        }
    }
    if (!bad) {
        if (m->viewKind < 0) bad = "has an unknown view grid";
        else if (m->viewN[0] < 2 || m->viewN[1] < 2 || m->viewN[0] * m->viewN[1] * m->layers > 2048) bad = "has a view grid out of range";
        else if (m->layers < 1 || m->layers > 4) bad = "has layers out of range (1 to 4)";
        else if (m->dim[0] < 4 || m->dim[1] < 4 || m->dim[0] > 4096 || m->dim[1] > 4096 || !(m->tpm > 0.0f)) bad = "has a texel grid out of range";
        else if (!(m->size[0] > 0.0f && m->size[1] > 0.0f && m->size[2] > 0.0f) || !(m->ceiling > 0.0f && m->ceiling <= m->size[1])) bad = "has a size out of range";
        else if (m->bays < 1 || m->bays > INTERIOR_MAX_BAYS || json_count(means) != m->bays || !(m->bayWidth > 0.0f)) bad = "has bays out of range, or not a mean for each";
        else if (!(m->bakeScale > 0.0f) || m->maxSteps < 1 || m->maxSteps > 256 || !(m->texStep > 0.0f)) bad = "has a scale or march out of range";
    }
    for (int b = 0; !bad && b < m->bays; ++b) {
        const Json *e = json_at(means, b);
        bool ok = e->type == JSON_ARRAY && json_count(e) == 3;
        for (int k = 0; ok && k < 3; ++k) {
            ok = json_at(e, k)->type == JSON_NUMBER && json_at(e, k)->number >= 0.0;
            if (ok) m->bayMean[b][k] = (float)json_at(e, k)->number;
        }
        if (!ok) bad = "has a bay mean that isn't three non-negative numbers";
    }
    /* the files beside the sidecar */
    const char *slash = strrchr(path, '/'), *bs = strrchr(path, '\\');
    if (bs > slash) slash = bs;
    int dir = slash ? (int)(slash - path) + 1 : 0;
    for (int c = 0; !bad && c < INTERIOR_CHANNELS; ++c) {
        const char *file = str(files, INTERIOR_CHANNEL_NAMES[c]);
        if (!file || !file[0]) bad = "doesn't name each channel's file";
        else snprintf(m->files[c], sizeof(m->files[c]), "%.*s%s", dir, path, file);
    }
    if (bad) snprintf(err, errSize, "%s %s", path, bad);
    json_free(doc);
    return !bad;
}

int interior_channel_layers(const InteriorModule *m, int c) {
    int views = m->viewN[0] * m->viewN[1];
    return c == INTERIOR_FLAGS ? views : views * m->layers;
}

static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t u64(const uint8_t *p) { return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32; }

/* The KTX2 layout as advert_frames_load reads it (src/render/advert_def.c). */
bool interior_image_load(const InteriorModule *m, int c, InteriorImage *img, char *err, size_t errSize) {
    static const uint8_t ID[12] = { 0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n' };
    memset(img, 0, sizeof(*img));
    const char *path = m->files[c];
    FILE *fp = fopen(path, "rb");
    if (!fp) { snprintf(err, errSize, "can't open %s", path); return false; }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *d = size > 0 ? (uint8_t *)malloc((size_t)size) : NULL;
    bool ok = d && fread(d, 1, (size_t)size, fp) == (size_t)size;
    fclose(fp);
    if (!ok) { free(d); snprintf(err, errSize, "can't read %s", path); return false; }
    img->data = d; img->size = (size_t)size;

    const char *why = NULL;
    if (size < 104 || memcmp(d, ID, 12) != 0) why = "isn't a KTX2 file";
    else {
        img->format = u32(d + 12);
        img->width = u32(d + 20); img->height = u32(d + 24);
        img->layers = u32(d + 32);
        uint32_t depth = u32(d + 28), faces = u32(d + 36), levels = u32(d + 40), super = u32(d + 44);
        img->offset = u64(d + 80);
        img->length = u64(d + 88);
        uint32_t block = c == INTERIOR_DEPTH ? 4u : 1u, bytes = c == INTERIOR_DEPTH ? 16u : c == INTERIOR_FLAGS ? 1u : 4u;
        uint64_t want = (uint64_t)((img->width + block - 1) / block) * ((img->height + block - 1) / block) * bytes * img->layers;
        if (img->format != INTERIOR_CHANNEL_FORMATS[c]) why = "isn't in its channel's format";
        else if (depth != 0 || faces != 1 || levels != 1 || super != 0) why = "isn't a 2D array of one level, uncompressed";
        else if (img->width != (uint32_t)m->dim[0] || img->height != (uint32_t)m->dim[1] || img->layers != (uint32_t)interior_channel_layers(m, c))
            why = "doesn't match the sidecar's grid, views and layers";
        else if (img->length != want) why = "has a level of the wrong size";
        else if (img->offset % 4 || img->offset + img->length > (uint64_t)size) why = "has its level outside the file";
    }
    if (why) {
        snprintf(err, errSize, "%s %s", path, why);
        interior_image_free(img);
        return false;
    }
    return true;
}

void interior_image_free(InteriorImage *img) {
    free(img->data);
    memset(img, 0, sizeof(*img));
}
