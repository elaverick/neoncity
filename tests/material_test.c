/* The authored PBR materials (assets/materials/*.json, src/render/material_def.c).
 *
 * The reader: JSON with comments parses, and malformed text fails with the
 * line it fails on; a material file's errors (an unknown key, a missing
 * required map, a value of the wrong kind) are caught.
 *
 * The files: every material CMake found (PBR_MATERIAL_NAMES) reads; each map
 * it names is a PNG that decodes; its properties are sound; the tiles of
 * what is mapped in world x and z (the road, the lawns, the land's cover)
 * divide the 64 chunks over which the shaders' world coordinates wrap.
 *
 * The models (test_models) and the adverts (assets/adverts/*.json,
 * src/render/advert_def.c: test_adverts) and the interior impostor modules
 * (assets/interiors/, src/render/interior_def.c: test_interiors) the same way.
 *
 * material_test [assets dir] (default: assets). */
#include "material_def.h"
#include "advert_def.h"
#include "interior_def.h"
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
    /* The shaders' world coordinates wrap every 64 chunks (ground.glsl): what is mapped in world x and z
     * (the roads, and the lots', lawns' and flat roofs' grain: planarFrame; the land's ground cover: terrain/ground.glsl;
     * the land roads' barriers: terrain/roads.glsl) must repeat a whole number of times in them. */
    static const char *const WORLD_MAPPED[] = { "road", "lot_asphalt", "grass", "roof_gravel", "concrete", "land_rock", "land_scree",
                                                "land_soil", "land_grass", "land_sand", "land_mud", "land_snow" };
    for (size_t k = 0; k < sizeof(WORLD_MAPPED) / sizeof(WORLD_MAPPED[0]); ++k) {
        char path[1100], err[512];
        snprintf(path, sizeof(path), "%s/materials/%s.json", assets, WORLD_MAPPED[k]);
        MaterialDef def;
        if (!material_def_load(path, &def, err, sizeof(err))) continue;   /* reported above */
        for (int i = 0; i < 2; ++i) {
            double n = 64.0 * CHUNK_SIZE / def.tile[i];
            /* to within the float tile's own rounding (1.1 is 1.1000000238: 6399.99986 repeats) */
            CHECK(fabs(n - floor(n + 0.5)) < 1e-4 + 1e-7 * n, "%s: the tile %g doesn't divide 64 chunks", WORLD_MAPPED[k], def.tile[i]);
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

/* The advert reader: defaults, and a file's errors caught. */
static void test_advert_def(void) {
    const char *path = "material_test_advert.json";
    static const struct { const char *text; bool ok; } CASES[] = {
        { "{ \"frames\": \"a.ktx2\", \"fps\": 12.5 }", true },
        { "{ \"fps\": 12 }", false },                                          /* no frames */
        { "{ \"frames\": 3, \"fps\": 12 }", false },
        { "{ \"frames\": \"a.ktx2\" }", false },                               /* no fps */
        { "{ \"frames\": \"a.ktx2\", \"fps\": 0 }", false },
        { "{ \"frames\": \"a.ktx2\", \"fps\": 12, \"glow\": 2 }", false },
        { "{ \"frames\": \"a.ktx2\", \"fps\": 12, \"accent\": [1, 1, 1] }", false },   /* the brand panel's, gone */
        { "{ \"frames\": \"a.ktx2\", \"fps\": 12, \"sheet\": \"a.png\" }", false },
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); ++i) {
        FILE *f = fopen(path, "wb");
        if (!f) { CHECK(0, "can't write %s", path); return; }
        fputs(CASES[i].text, f);
        fclose(f);
        AdvertDef d;
        char err[600];
        bool ok = advert_def_load(path, &d, err, sizeof(err));
        CHECK(ok == CASES[i].ok, "advert %zu: %s", i, ok ? "accepted" : err);
        if (i == 0 && ok)
            CHECK(d.fps == 12.5f && d.weight == 1.0f && d.glow == 0.3f && strcmp(d.frames, "a.ktx2") == 0,
                  "advert values and defaults");
    }
    remove(path);
}

/* Every advert CMake found (ADVERT_NAMES) reads; its frames file is a KTX2
 * array of BC7 or BC1 frames with its whole mip chain and mean colour
 * (advert_frames_load checks); it plays whole within a screen's slot. */
static void test_adverts(const char *assets) {
    int frames = 0;
    double mb = 0.0;
    for (int i = 0; i < ADVERT_COUNT; ++i) {
        const char *name = ADVERT_NAMES[i];
        char path[1100], err[1300];
        snprintf(path, sizeof(path), "%s/adverts/%s.json", assets, name);
        AdvertDef d;
        if (!advert_def_load(path, &d, err, sizeof(err))) { CHECK(0, "advert %s: %s", name, err); continue; }
        AdvertFrames f;
        if (!advert_frames_load(d.frames, &f, err, sizeof(err))) { CHECK(0, "advert %s: %s", name, err); continue; }
        double seconds = f.frames / (double)d.fps, slot = ADVERT_SLOT_SECONDS * (1.0 - ADVERT_STATIC);
        CHECK(seconds <= slot + 1e-6, "advert %s: %.2f s won't fit a screen's %.3f s slot", name, seconds, slot);
        CHECK(d.brightness < 50.0f, "advert %s: brightness %g", name, d.brightness);
        for (int k = 0; k < 3; ++k) CHECK(f.mean[k] >= 0.0f && f.mean[k] <= 1.0f, "advert %s: mean colour %g", name, f.mean[k]);
        frames += (int)f.frames;
        mb += f.size / 1e6;
        advert_frames_free(&f);
    }
    printf("%d adverts, %d frames, %.1f MB\n", ADVERT_COUNT, frames, mb);
}

/* Every interior impostor module the catalogue names (assets/interiors/,
 * src/render/interior_def.c) reads: its sidecar is sound and its four files
 * hold what it says (interior_image_load checks the format, grid, views and
 * layers); its depth blocks are the 8-level kind the baker writes, and its
 * radiance is finite. */
static void test_interiors(const char *assets) {
    char path[1100], err[1400], names[INTERIOR_MAX_MODULES][64];
    int n = 0;
    snprintf(path, sizeof(path), "%s/interiors/catalogue.json", assets);
    if (!interior_catalogue_load(path, names, INTERIOR_MAX_MODULES, &n, err, sizeof(err))) { CHECK(0, "interiors: %s", err); return; }
    double mb = 0.0, nearMb = 0.0;
    int nears = 0;
    for (int t = 0; t < 2 * n; ++t) {
        /* each module, then each one's near tier, if it has one: the same floor at twice the texels */
        int i = t % n;
        bool isNear = t >= n;
        char mpath[1200], want[80];
        snprintf(want, sizeof(want), "%s%s", names[i], isNear ? ".near" : "");
        snprintf(mpath, sizeof(mpath), "%s/interiors/%s.json", assets, want);
        if (isNear) { FILE *fp = fopen(mpath, "rb"); if (!fp) continue; fclose(fp); nears++; }
        InteriorModule m;
        if (!interior_module_load(mpath, &m, err, sizeof(err))) { CHECK(0, "interior %s: %s", want, err); continue; }
        CHECK(strcmp(m.name, want) == 0, "interior %s: its sidecar names it %s", want, m.name);
        if (isNear) {
            InteriorModule b;
            char bpath[1200];
            snprintf(bpath, sizeof(bpath), "%s/interiors/%s.json", assets, names[i]);
            if (interior_module_load(bpath, &b, err, sizeof(err))) {
                CHECK(m.tpm == 2.0f * b.tpm, "interior %s: %g texels a metre, its base has %g", want, m.tpm, b.tpm);
                CHECK(m.size[0] == b.size[0] && m.size[1] == b.size[1] && m.size[2] == b.size[2] && m.ceiling == b.ceiling && m.type == b.type && m.state == b.state,
                      "interior %s: isn't the same floor as its base", want);
                CHECK(m.viewN[0] == b.viewN[0] && m.viewN[1] == b.viewN[1] && m.layers == b.layers, "interior %s: its views or layers differ from its base's", want);
            }
        }
        CHECK(m.opening[0] >= 0.0f && m.opening[0] < m.opening[1] && m.opening[1] <= m.size[1], "interior %s: opening %g..%g", names[i], m.opening[0], m.opening[1]);
        for (int c = 0; c < INTERIOR_CHANNELS; ++c) {
            InteriorImage img;
            if (!interior_image_load(&m, c, &img, err, sizeof(err))) { CHECK(0, "interior %s: %s", names[i], err); continue; }
            if (isNear) nearMb += img.size / 1e6; else mb += img.size / 1e6;
            const uint8_t *d = img.data + img.offset;
            if (c == INTERIOR_DEPTH) {
                /* red0 >= red1 in each channel's half (red0 < red1 is BC4's 6-level mode, whose
                 * indices 6 and 7 mean 0 and 1: never written) */
                uint64_t sixLevel = 0;
                for (uint64_t h = 0; h < img.length / 8; ++h) sixLevel += d[h * 8] < d[h * 8 + 1];
                CHECK(sixLevel == 0, "interior %s: %llu depth blocks in BC4's 6-level mode", names[i], (unsigned long long)sixLevel);
            } else if (c == INTERIOR_LINT) {
                /* every block in BC6H's mode 11 (00011), the one the baker writes */
                uint64_t bad = 0;
                for (uint64_t b = 0; b < img.length / 16; ++b) bad += (d[b * 16] & 31u) != 3u;
                CHECK(bad == 0, "interior %s: %llu radiance blocks not in BC6H's mode 11", names[i], (unsigned long long)bad);
            } else if (c == INTERIOR_ALBEDO || c == INTERIOR_EXT) {
                /* every block in BC7's mode 6 (seven bits, 0000001 from the low end) */
                uint64_t bad = 0;
                for (uint64_t b = 0; b < img.length / 16; ++b) bad += (d[b * 16] & 127u) != 64u;
                CHECK(bad == 0, "interior %s: %llu %s blocks not in BC7's mode 6", names[i], (unsigned long long)bad, INTERIOR_CHANNEL_NAMES[c]);
            }
            interior_image_free(&img);
        }
    }
    printf("%d interior modules, %.1f MB; %d near tiers, %.1f MB\n", n, mb, nears, nearMb);
}

int main(int argc, char **argv) {
    const char *assets = argc > 1 ? argv[1] : "assets";
    test_json();
    test_def();
    test_advert_def();
    test_files(assets);
    test_models(assets);
    test_adverts(assets);
    test_interiors(assets);
    if (g_failures) { printf("%d failures\n", g_failures); return 1; }
    printf("material_test: ok\n");
    return 0;
}
