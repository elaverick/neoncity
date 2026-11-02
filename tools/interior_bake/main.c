#include "bake.h"

/* interior_bake: bakes office floor modules into interior impostors
 * (docs/interior-impostors.md), and measures the representation.
 *
 *   interior_bake --scene <name> --out <dir> [--views NxM] [--grid slope|azel|hemioct]
 *                 [--res texels/m] [--margin m] [--layers K] [--last]
 *                 [--state after_hours|late_shift|cleaners|screens]
 *                 [--spp n] [--ext-spp n] [--bounces n] [--clamp c] [--seed n] [--direct] [--preview png]
 *       bakes one test module (scene.c) to <dir>/<name>.*.ktx2 and .json, path traced
 *       (pathtrace.glsl: 16384 samples for its own lights, 8192 for each other light basis,
 *       8 bounces, a clamp of 16), or with IM1's direct light (--direct); --preview writes the
 *       nearest layer of the straight-on view, and of the view 25 degrees up as <png>_up;
 *       --near bakes its near tier (32 texels a metre) to <dir>/<name>.near.*
 *   interior_bake --scene <name> --beauty x,y,z,yaw,pitch[,fov] --image out.png [--size WxH] [--state s] [--spp n]
 *       a perspective still from inside the module (beauty.c); a generated floor also takes --beauty auto
 *   interior_bake --lab <dir> [--quick]
 *       the representation sweep (lab.c): error and cost of each way of baking, sheets and tables in <dir>
 *   interior_bake --test <dir> [--write-ref]
 *       the path tracer's tests (test.c) against the reference in <dir> (ctest bake_test),
 *       or render that reference
 *   interior_bake --catalogue [--dry-run] [--only name] [--force] [--adopt] [--out dir]
 *       bakes both tiers of every module in assets/interiors/catalogue.json whose inputs changed, into
 *       assets/interiors (catalogue.c); --dry-run says which, --force bakes them anyway, --only takes one
 *       module, --adopt records today's hash in sidecars that have none instead of baking.
 *       --spp and the other settings above apply, and count as inputs
 *   interior_bake --check [--out dir]
 *       validates the bakes against the catalogue, without the GPU (ctest interior_catalogue)
 *   interior_bake --list
 *   interior_bake --hash <name> [--state s]
 *       a hash of the scene's geometry and materials, without the GPU
 *
 * --assets <dir> reads the models from there (default: the source tree's
 * assets); --validation turns on the Vulkan validation layer. With
 * IMP_SELFTEST set, --lab checks the pipeline instead (lab.c: selftest). */

#ifndef NEONCITY_ASSETS
#define NEONCITY_ASSETS "assets"
#endif

static void usage(void) {
    fprintf(stderr, "usage: interior_bake --scene <name> --out <dir> [--views NxM] [--grid slope|azel|hemioct] [--res t] [--margin m]\n"
                    "                     [--layers K] [--last] [--state s] [--spp n] [--ext-spp n] [--bounces n] [--clamp c] [--seed n] [--direct]\n"
                    "                     [--near] [--preview png] [--assets dir] [--validation]\n"
                    "       interior_bake --scene <name> --beauty x,y,z,yaw,pitch[,fov] --image png [--size WxH] [--state s] [--spp n]\n"
                    "       interior_bake --lab <dir> [--quick] [--assets dir] [--validation]\n"
                    "       interior_bake --test <dir> [--write-ref] [--validation]\n"
                    "       interior_bake --catalogue [--dry-run] [--only name] [--force] [--adopt] [--out dir] [--assets dir]\n"
                    "       interior_bake --check [--out dir] [--assets dir]\n"
                    "       interior_bake --list\n"
                    "       interior_bake --hash <name> [--state s]\n");
}

int main(int argc, char **argv) {
    const char *scene = NULL, *out = NULL, *lab = NULL, *test = NULL, *preview = NULL, *assets = NEONCITY_ASSETS, *beauty = NULL, *image = NULL;
    const char *only = NULL;
    bool validation = false, quick = false, writeRef = false, hashOnly = false, near = false;
    bool catalogue = false, check = false, dry = false, force = false, adopt = false, oneScene = false;
    BakeSettings b = bake_settings_default();
    int state = -1, imgW = 1280, imgH = 534;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i], *next = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--scene") && next) { scene = argv[++i]; oneScene = true; }
        else if (!strcmp(a, "--catalogue")) catalogue = true;
        else if (!strcmp(a, "--check")) check = true;
        else if (!strcmp(a, "--dry-run")) dry = true;
        else if (!strcmp(a, "--force")) force = true;
        else if (!strcmp(a, "--adopt")) adopt = true;
        else if (!strcmp(a, "--only") && next) only = argv[++i];
        else if (!strcmp(a, "--hash") && next) { scene = argv[++i]; hashOnly = true; }
        else if (!strcmp(a, "--out") && next) out = argv[++i];
        else if (!strcmp(a, "--lab") && next) lab = argv[++i];
        else if (!strcmp(a, "--test") && next) test = argv[++i];
        else if (!strcmp(a, "--write-ref")) writeRef = true;
        else if (!strcmp(a, "--preview") && next) preview = argv[++i];
        else if (!strcmp(a, "--direct")) b.direct = true;
        else if (!strcmp(a, "--spp") && next) b.tc.spp = atoi(argv[++i]);
        else if (!strcmp(a, "--ext-spp") && next) b.extSpp = atoi(argv[++i]);
        else if (!strcmp(a, "--beauty") && next) beauty = argv[++i];
        else if (!strcmp(a, "--image") && next) image = argv[++i];
        else if (!strcmp(a, "--size") && next) {
            if (sscanf(argv[++i], "%dx%d", &imgW, &imgH) != 2 || imgW < 1 || imgH < 1) { usage(); return 2; }
        } else if (!strcmp(a, "--state") && next) {
            const char *n = argv[++i];
            for (state = 0; state < LIGHTS_COUNT && strcmp(n, LIGHT_STATE_NAMES[state]); ++state) {}
            if (state == LIGHTS_COUNT) { usage(); return 2; }
        }
        else if (!strcmp(a, "--bounces") && next) b.tc.bounces = atoi(argv[++i]);
        else if (!strcmp(a, "--clamp") && next) b.tc.clamp = (float)atof(argv[++i]);
        else if (!strcmp(a, "--seed") && next) b.tc.seed = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--assets") && next) assets = argv[++i];
        else if (!strcmp(a, "--views") && next) {
            if (sscanf(argv[++i], "%dx%d", &b.v.nu, &b.v.nv) != 2 || b.v.nu < 2 || b.v.nv < 2) { usage(); return 2; }
        } else if (!strcmp(a, "--grid") && next) {
            const char *k = argv[++i];
            b.v.kind = !strcmp(k, "hemioct") ? GRID_HEMIOCT : !strcmp(k, "azel") ? GRID_AZEL : GRID_SLOPE;
        } else if (!strcmp(a, "--res") && next) b.tpm = (float)atof(argv[++i]);
        else if (!strcmp(a, "--near")) { near = true; b.tpm = 32.0f; }
        else if (!strcmp(a, "--margin") && next) b.margin = (float)atof(argv[++i]);
        else if (!strcmp(a, "--layers") && next) b.ic.K = atoi(argv[++i]);
        else if (!strcmp(a, "--last")) b.ic.lastSlot = 1;
        else if (!strcmp(a, "--quick")) quick = true;
        else if (!strcmp(a, "--validation")) validation = true;
        else if (!strcmp(a, "--list")) {
            if (!floor_catalogue_load(assets)) return 1;
            for (int s = 0; s < SCENE_COUNT; ++s) printf("%s\n", SCENE_NAMES[s]);
            for (int s = 0; s < FLOOR_CATALOGUE_COUNT; ++s) printf("%s\n", FLOOR_CATALOGUE[s].name);
            return 0;
        } else { usage(); return 2; }
    }
    if (b.ic.K < 1 || b.ic.K > (b.ic.lastSlot ? CAP_SLOTS : CAP_SLOTS - 1) || b.tpm <= 0.0f || b.margin < 0.0f) { usage(); return 2; }
    if (b.tc.spp < 1 || b.tc.bounces < 1 || b.tc.bounces > 64 || b.tc.clamp < 0.0f || b.extSpp < 1) { usage(); return 2; }
    if (catalogue || check) {
        if ((catalogue && check) || oneScene || hashOnly || near || b.direct || state >= 0 || lab || test || beauty) { usage(); return 2; }
        char dir[1100];
        snprintf(dir, sizeof(dir), "%s/interiors", assets);
        const char *where = out ? out : dir;
        if (check) return catalogue_check(&b, assets, where);
        CatalogueRun run = { assets, where, only, dry, force, adopt, validation };
        return catalogue_bake(&b, &run);
    }
    bake_settings_finish(&b);

    if (lab || test) {
        Gpu *g = gpu_create(validation);
        int r = lab ? lab_run(g, lab, assets, quick) : test_run(g, test, writeRef);
        gpu_destroy(g);
        return r;
    }
    if (hashOnly) {
        Scene s;
        if (!scene_build(&s, scene, state, assets)) return 1;
        printf("%s %016llx\n", s.name, (unsigned long long)scene_hash(&s));
        scene_free(&s);
        return 0;
    }
    if (!scene || (!out && !beauty) || (beauty && !image)) { usage(); return 2; }
    Scene s;
    if (!scene_build(&s, scene, state, assets)) return 1;
    /* the input hash leaves the tier's name out, as the catalogue's does */
    if (near) { size_t len = strlen(s.name); snprintf(s.name + len, sizeof(s.name) - len, ".near"); }
    bool ok;
    if (beauty) {
        Gpu *g = gpu_create(validation);
        gpu_load_scene(g, &s);
        ok = beauty_render(g, &s, &b.tc, beauty, imgW, imgH, image);
        gpu_destroy(g);
    } else {
        ok = bake_module(out, &s, &b, validation, preview);
    }
    scene_free(&s);
    return ok ? 0 : 1;
}
