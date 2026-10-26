#include "advert_def.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const ADVERT_NAMES[ADVERT_TABLE_SIZE] = {
#include "advert_names.inc"
};

/* One number, or n numbers (one number fills all n). */
static bool numbers(const Json *j, float *v, int n, char *err, size_t errSize) {
    if (j->type == JSON_NUMBER) {
        for (int i = 0; i < n; ++i) v[i] = (float)j->number;
        return true;
    }
    bool ok = j->type == JSON_ARRAY && json_count(j) == n;
    for (int i = 0; i < n && ok; ++i) {
        const Json *e = json_at(j, i);
        ok = e->type == JSON_NUMBER;
        if (ok) v[i] = (float)e->number;
    }
    if (!ok) snprintf(err, errSize, "line %d: \"%s\" must be a number%s", j->line, j->key, n > 1 ? " or that many numbers" : "");
    return ok;
}

bool advert_def_load(const char *path, AdvertDef *def, char *err, size_t errSize) {
    static const AdvertDef DEFAULTS = { .weight = 1.0f, .brightness = 1.0f, .glow = 0.3f };
    *def = DEFAULTS;
    char why[256];
    Json *doc = json_load(path, why, sizeof(why));
    if (!doc) { snprintf(err, errSize, "%s", why); return false; }

    bool ok = doc->type == JSON_OBJECT;
    if (!ok) snprintf(err, errSize, "the file holds %s, not an object", json_type_name(doc->type));
    for (const Json *m = ok ? doc->child : NULL; m && ok; m = m->next) {
        if (strcmp(m->key, "frames") == 0) {
            if (m->type != JSON_STRING || !m->string[0]) { snprintf(err, errSize, "line %d: \"frames\" must be a file name", m->line); ok = false; break; }
            /* beside the manifest, unless absolute */
            const char *slash = strrchr(path, '/'), *bs = strrchr(path, '\\');
            if (bs > slash) slash = bs;
            bool absolute = m->string[0] == '/' || m->string[0] == '\\' || m->string[1] == ':';
            int dir = slash && !absolute ? (int)(slash - path) + 1 : 0;
            snprintf(def->frames, sizeof(def->frames), "%.*s%s", dir, path, m->string);
        }
        else if (strcmp(m->key, "fps") == 0)        ok = numbers(m, &def->fps, 1, err, errSize);
        else if (strcmp(m->key, "weight") == 0)     ok = numbers(m, &def->weight, 1, err, errSize);
        else if (strcmp(m->key, "brightness") == 0) ok = numbers(m, &def->brightness, 1, err, errSize);
        else if (strcmp(m->key, "glow") == 0)       ok = numbers(m, &def->glow, 1, err, errSize);
        else { snprintf(err, errSize, "line %d: unknown key \"%s\"", m->line, m->key); ok = false; }
    }
    if (ok && !def->frames[0])                     { snprintf(err, errSize, "no \"frames\""); ok = false; }
    if (ok && !(def->fps > 0.0f))                  { snprintf(err, errSize, "\"fps\" must be given, above 0"); ok = false; }
    if (ok && def->weight < 0.0f)                  { snprintf(err, errSize, "\"weight\" must not be negative"); ok = false; }
    if (ok && def->brightness <= 0.0f)             { snprintf(err, errSize, "\"brightness\" must be positive"); ok = false; }
    if (ok && (def->glow < 0.0f || def->glow > 1.0f)) { snprintf(err, errSize, "\"glow\" must be from 0 to 1"); ok = false; }
    json_free(doc);
    return ok;
}

static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t u64(const uint8_t *p) { return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32; }

/* The KTX2 layout (khronos.org/ktx): a 12-byte identifier, nine u32s (format,
 * type size, width, height, depth, layers, faces, levels, supercompression), the
 * offsets and lengths of the format descriptor, key/value data and global data,
 * then each level's offset and lengths, level 0 first. */
bool advert_frames_load(const char *path, AdvertFrames *f, char *err, size_t errSize) {
    static const uint8_t ID[12] = { 0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n' };
    memset(f, 0, sizeof(*f));
    FILE *fp = fopen(path, "rb");
    if (!fp) { snprintf(err, errSize, "can't open %s", path); return false; }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *d = size > 0 ? (uint8_t *)malloc((size_t)size) : NULL;
    bool ok = d && fread(d, 1, (size_t)size, fp) == (size_t)size;
    fclose(fp);
    if (!ok) { free(d); snprintf(err, errSize, "can't read %s", path); return false; }
    f->data = d; f->size = (size_t)size;

    const char *why = NULL;
    if (size < 80 || memcmp(d, ID, 12) != 0) why = "isn't a KTX2 file";
    else {
        f->format = u32(d + 12);
        f->width = u32(d + 20); f->height = u32(d + 24);
        f->frames = u32(d + 32);
        f->levels = u32(d + 40);
        uint32_t depth = u32(d + 28), faces = u32(d + 36), super = u32(d + 44);
        uint32_t maxDim = f->width > f->height ? f->width : f->height, chain = 1;
        while (maxDim >> chain) chain++;
        if (f->format != ADVERT_FORMAT_BC7 && f->format != ADVERT_FORMAT_BC1) why = "isn't BC7 or BC1 (sRGB)";
        else if (depth != 0 || faces != 1 || f->frames < 1) why = "isn't a 2D array texture";
        else if (super != 0) why = "is supercompressed";
        else if (f->width < 1 || f->height < 1 || f->width > 4096 || f->height > 4096) why = "has a frame size out of range";
        else if (f->levels != chain || (size_t)size < 80 + 24 * (size_t)f->levels) why = "doesn't have its whole mip chain";
        uint32_t block = f->format == ADVERT_FORMAT_BC7 ? 16u : 8u;
        for (uint32_t l = 0; l < f->levels && !why; ++l) {
            uint32_t w = f->width >> l ? f->width >> l : 1, h = f->height >> l ? f->height >> l : 1;
            f->offset[l] = u64(d + 80 + 24 * l);
            f->length[l] = u64(d + 88 + 24 * l);
            if (f->length[l] != (uint64_t)((w + 3) / 4) * ((h + 3) / 4) * block * f->frames) why = "has a level of the wrong size";
            else if (f->offset[l] % block || f->offset[l] + f->length[l] > (uint64_t)size) why = "has a level outside the file";
        }
        /* the mean colour among the key/value pairs, each: u32 length, key\0value */
        uint32_t kvOff = u32(d + 56), kvLen = u32(d + 60);
        bool mean = false;
        for (uint32_t p = kvOff; !why && p + 4 <= kvOff + kvLen && p + 4 <= (uint32_t)size; ) {
            uint32_t n = u32(d + p);
            if (p + 4 + n > (uint32_t)size || n > kvLen) break;
            const char *key = (const char *)d + p + 4;
            size_t keyLen = strnlen(key, n);
            if (keyLen < n && strcmp(key, "NeonCity.mean") == 0) {
                char v[64] = "";
                snprintf(v, sizeof(v), "%.*s", (int)(n - keyLen - 1), key + keyLen + 1);
                mean = sscanf(v, "%f %f %f", &f->mean[0], &f->mean[1], &f->mean[2]) == 3;
            }
            p += 4 + ((n + 3) & ~3u);
        }
        if (!why && !mean) why = "has no \"NeonCity.mean\"";
    }
    if (why) {
        snprintf(err, errSize, "%s %s", path, why);
        advert_frames_free(f);
        return false;
    }
    return true;
}

void advert_frames_free(AdvertFrames *f) {
    free(f->data);
    memset(f, 0, sizeof(*f));
}
