/* The authored PBR materials (assets/materials/*.json, src/render/material_def.c).
 *
 * The reader: JSON with comments parses, and malformed text fails with the
 * line it fails on; a material file's errors (an unknown key, a missing
 * required map, a value of the wrong kind) are caught.
 *
 * The files: every material CMake found (PBR_MATERIAL_NAMES) reads; each map
 * it names is a PNG that decodes; its properties are sound; the road's tile
 * divides the 64 chunks over which the shaders' world coordinates wrap.
 *
 * material_test [assets dir] (default: assets). */
#include "material_def.h"
#include "json.h"
#include "common.h"
#include "models.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); g_failures++; } } while (0)

static void test_json(void) {
    char err[256];
    Json *j = json_parse("// a comment\n{ \"a\": [1, -2.5e1, true, null], /* block\n comment */ \"b\": \"x\\u00e9\\n\" }", err, sizeof(err));
    CHECK(j, "valid JSON with comments: %s", err);
    if (j) {
        const Json *a = json_get(j, "a");
        CHECK(a && json_count(a) == 4 && json_at(a, 1)->number == -25.0 && json_at(a, 2)->type == JSON_BOOL && json_at(a, 3)->type == JSON_NULL,
              "array members");
        const Json *b = json_get(j, "b");
        CHECK(b && b->type == JSON_STRING && strcmp(b->string, "x\xc3\xa9\n") == 0, "string escapes");
        CHECK(!json_get(j, "c"), "absent member");
        CHECK(a && a->line == 2, "a member's line (%d)", a ? a->line : -1);
        json_free(j);
    }
    static const struct { const char *text; int line; } BAD[] = {
        { "{ \"a\": 1,\n  \"b\" 2 }", 2 },        /* no colon */
        { "[1, 2,\n\n]", 3 },                     /* trailing comma */
        { "{ \"a\": \"open\n\" }", 1 },           /* string across a line */
        { "[1] 2", 1 },                           /* text after */
        { "/* never closed", 1 },
        { "{ \"a\": tru }", 1 },
    };
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); ++i) {
        Json *bad = json_parse(BAD[i].text, err, sizeof(err));
        int line = 0;
        sscanf(err, "line %d", &line);
        CHECK(!bad && line == BAD[i].line, "malformed JSON %zu: %s (expected line %d)", i, bad ? "parsed" : err, BAD[i].line);
        json_free(bad);
    }
}

/* Writes text to a scratch material file and reads it. */
static bool def_from(const char *text, MaterialDef *def, char *err, size_t errSize) {
    const char *path = "material_test_scratch.json";
    FILE *f = fopen(path, "wb");
    if (!f) { snprintf(err, errSize, "can't write %s", path); return false; }
    fputs(text, f);
    fclose(f);
    bool ok = material_def_load(path, def, err, errSize);
    remove(path);
    return ok;
}

static void test_def(void) {
    char err[512];
    MaterialDef d;
    const char *MAPS = "\"maps\": { \"albedo\": \"t/a.png\", \"normal\": \"./t/../t/n.png\", \"orm\": \"../x/t/o.png\" }";
    char text[1024];
    snprintf(text, sizeof(text), "{ %s, \"tile\": [2, 1], \"albedo_mean\": 0.3, \"params\": [1, 2] }", MAPS);
    CHECK(def_from(text, &d, err, sizeof(err)), "a good material: %s", err);
    CHECK(d.tile[0] == 2.0f && d.tile[1] == 1.0f && d.albedoScale == 1.0f && d.albedoMean[2] == 0.3f
          && d.relief[1] == 1.0f && d.params[1] == 2.0f && d.params[2] == 0.0f, "values and defaults");
    CHECK(strcmp(d.maps[PBR_NORMAL], "t/n.png") == 0 && strcmp(d.maps[PBR_ORM], "../x/t/o.png") == 0 && !d.maps[PBR_EMISSIVE][0],
          "map paths resolved (%s, %s)", d.maps[PBR_NORMAL], d.maps[PBR_ORM]);

    static const char *const BAD[] = {
        "{ \"maps\": { \"albedo\": \"a.png\", \"normal\": \"n.png\" } }",   /* no orm */
        "{ %s, \"tiles\": 2 }",                                            /* unknown key */
        "{ %s, \"tile\": [1, 2, 3] }",
        "{ %s, \"tile\": -1 }",
        "{ %s, \"albedo_scale\": \"bright\" }",
        "{ %s, \"params\": [1, 2, 3, 4, 5] }",
        "{ \"maps\": { \"albedo\": \"a.png\", \"normal\": \"n.png\", \"orm\": \"o.png\", \"height\": \"h.png\" } }",
        "[ %s ]",
    };
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); ++i) {
        snprintf(text, sizeof(text), BAD[i], MAPS);
        CHECK(!def_from(text, &d, err, sizeof(err)), "bad material %zu was accepted", i);
    }
}

static void test_files(const char *assets) {
    int images = 0;
    char seen[PBR_TEXTURE_COUNT][1024];
    int nSeen = 0;
    for (int m = 0; m < PBR_MATERIAL_COUNT; ++m) {
        const char *name = PBR_MATERIAL_NAMES[m];
        char path[1100], err[512];
        snprintf(path, sizeof(path), "%s/materials/%s.json", assets, name);
        MaterialDef d;
        if (!material_def_load(path, &d, err, sizeof(err))) { CHECK(0, "%s: %s", name, err); continue; }
        for (int k = 0; k < PBR_MAPS_PER_MATERIAL; ++k) {
            if (!d.maps[k][0]) continue;
            int w, h, c;
            CHECK(stbi_info(d.maps[k], &w, &h, &c), "%s: its %s map %s isn't a readable PNG", name, PBR_MAP_KEYS[k], d.maps[k]);
            bool dup = false;
            for (int i = 0; i < nSeen && !dup; ++i) dup = strcmp(seen[i], d.maps[k]) == 0;
            if (!dup) snprintf(seen[nSeen++], sizeof(seen[0]), "%s", d.maps[k]);
            images++;
        }
        CHECK(d.albedoScale > 0.0f && d.albedoScale < 4.0f, "%s: albedo_scale %g", name, d.albedoScale);
        CHECK(d.relief[0] >= 0.0f && d.relief[0] <= 1.0f, "%s: the relief's median %g lies outside the height map's range", name, d.relief[0]);
        for (int i = 0; i < 3; ++i) CHECK(d.albedoMean[i] <= 1.0f, "%s: albedo_mean %g", name, d.albedoMean[i]);
    }
    /* The shaders' world coordinates wrap every 64 chunks (ground.glsl). */
    char path[1100], err[512];
    snprintf(path, sizeof(path), "%s/materials/road.json", assets);
    MaterialDef road;
    if (material_def_load(path, &road, err, sizeof(err))) {
        for (int i = 0; i < 2; ++i) {
            double n = 64.0 * CHUNK_SIZE / road.tile[i];
            CHECK(fabs(n - floor(n + 0.5)) < 1e-4, "road: the tile %g doesn't divide 64 chunks", road.tile[i]);
        }
    }
    printf("%d materials, %d maps from %d image files\n", PBR_MATERIAL_COUNT, images, nSeen);
}

/* Every model (src/city/models.h) reads, is shaded by authored materials (its
 * own and its liveries', each once), stands on y = 0 centred, and is of a size
 * street furniture might be. */
static void test_models(const char *assets) {
    int tris = 0, liveries = 0;
    for (int i = 0; i < PM_COUNT; ++i) {
        const char *name = MODEL_NAMES[i];
        char path[1100], err[256];
        snprintf(path, sizeof(path), "%s/models/%s.glb", assets, name);
        Model m;
        if (!model_read(path, &m, err, sizeof(err))) { CHECK(0, "model %s: %s", name, err); continue; }
        CHECK(m.liveries >= 1 && m.liveries <= MODEL_LIVERIES, "model %s: %d liveries", name, m.liveries);
        for (int k = 0; k < m.liveries; ++k) {
            CHECK(m.pbr[k] < PBR_MATERIAL_COUNT, "model %s: livery %d's material is %d", name, k, m.pbr[k]);
            for (int j = 0; j < k; ++j) CHECK(m.pbr[j] != m.pbr[k], "model %s: liveries %d and %d are both %s", name, j, k, PBR_MATERIAL_NAMES[m.pbr[k]]);
        }
        liveries += m.liveries;
        CHECK(fabsf(m.mn[1]) < 1e-3f, "model %s: its foot is at %g, not 0", name, m.mn[1]);
        CHECK(fabsf(m.mn[0] + m.mx[0]) < 1e-2f && fabsf(m.mn[2] + m.mx[2]) < 1e-2f, "model %s: not centred", name);
        for (int k = 0; k < 3; ++k) CHECK(m.mx[k] - m.mn[k] > 0.05f && m.mx[k] - m.mn[k] < 4.0f, "model %s: %g m along axis %d", name, m.mx[k] - m.mn[k], k);
        for (uint32_t v = 0; v < m.nv; ++v) {
            float l = sqrtf(m.v[v].n[0] * m.v[v].n[0] + m.v[v].n[1] * m.v[v].n[1] + m.v[v].n[2] * m.v[v].n[2]);
            if (fabsf(l - 1.0f) > 0.01f) { CHECK(0, "model %s: vertex %u's normal isn't unit", name, v); break; }
        }
        tris += (int)(m.ni / 3);
        model_free(&m);
    }
    printf("%d models, %d triangles\n", PM_COUNT, tris);
}

int main(int argc, char **argv) {
    const char *assets = argc > 1 ? argv[1] : "assets";
    test_json();
    test_def();
    test_files(assets);
    test_models(assets);
    if (g_failures) { printf("%d failures\n", g_failures); return 1; }
    printf("material_test: ok\n");
    return 0;
}
