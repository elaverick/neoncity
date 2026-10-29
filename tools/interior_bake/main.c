#include "bake.h"

/* interior_bake: bakes office floor modules into interior impostors
 * (docs/interior-impostors.md), and measures the representation.
 *
 *   interior_bake --scene <name> --out <dir> [--views NxM] [--grid slope|azel|hemioct]
 *                 [--res texels/m] [--margin m] [--layers K] [--last]
 *       bakes one test module (scene.c) to <dir>/<name>.*.ktx2 and .json
 *   interior_bake --lab <dir> [--quick]
 *       the representation sweep (lab.c): error and cost of each way of baking, sheets and tables in <dir>
 *   interior_bake --list
 *
 * --assets <dir> reads the models from there (default: the source tree's
 * assets); --validation turns on the Vulkan validation layer. With
 * IMP_SELFTEST set, --lab checks the pipeline instead (lab.c: selftest). */

#ifndef NEONCITY_ASSETS
#define NEONCITY_ASSETS "assets"
#endif

static void usage(void) {
    fprintf(stderr, "usage: interior_bake --scene <name> --out <dir> [--views NxM] [--grid slope|azel|hemioct] [--res t] [--margin m]\n"
                    "                     [--layers K] [--last] [--assets dir] [--validation]\n"
                    "       interior_bake --lab <dir> [--quick] [--assets dir] [--validation]\n"
                    "       interior_bake --list\n");
}

int main(int argc, char **argv) {
    const char *scene = NULL, *out = NULL, *lab = NULL, *assets = NEONCITY_ASSETS;
    bool validation = false, quick = false;
    /* the defaults IM1 chose (docs/interior-impostors.md, section 13) */
    ViewGrid v = { 5, 10, GRID_SLOPE, 60.0f, -30.0f, 70.0f, 0.8f, 0.12f };
    float tpm = 16.0f, margin = 1.0f;
    ImpConfig ic = { 0 };
    ic.K = 3; ic.lastSlot = 0;
    ic.maxSteps = 64; ic.maxTexStep = 1.0f; ic.refine = 6; ic.unknownW = 0.15f; ic.offGridW = 0.1f;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i], *next = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--scene") && next) scene = argv[++i];
        else if (!strcmp(a, "--out") && next) out = argv[++i];
        else if (!strcmp(a, "--lab") && next) lab = argv[++i];
        else if (!strcmp(a, "--assets") && next) assets = argv[++i];
        else if (!strcmp(a, "--views") && next) {
            if (sscanf(argv[++i], "%dx%d", &v.nu, &v.nv) != 2 || v.nu < 2 || v.nv < 2) { usage(); return 2; }
        } else if (!strcmp(a, "--grid") && next) {
            const char *k = argv[++i];
            v.kind = !strcmp(k, "hemioct") ? GRID_HEMIOCT : !strcmp(k, "azel") ? GRID_AZEL : GRID_SLOPE;
        } else if (!strcmp(a, "--res") && next) tpm = (float)atof(argv[++i]);
        else if (!strcmp(a, "--margin") && next) margin = (float)atof(argv[++i]);
        else if (!strcmp(a, "--layers") && next) ic.K = atoi(argv[++i]);
        else if (!strcmp(a, "--last")) ic.lastSlot = 1;
        else if (!strcmp(a, "--quick")) quick = true;
        else if (!strcmp(a, "--validation")) validation = true;
        else if (!strcmp(a, "--list")) {
            for (int s = 0; s < SCENE_COUNT; ++s) printf("%s\n", SCENE_NAMES[s]);
            return 0;
        } else { usage(); return 2; }
    }
    if (ic.K < 1 || ic.K > (ic.lastSlot ? CAP_SLOTS : CAP_SLOTS - 1) || tpm <= 0.0f || margin < 0.0f) { usage(); return 2; }
    v.azMax *= 3.14159265f / 180.0f;
    v.elMin *= 3.14159265f / 180.0f;
    v.elMax *= 3.14159265f / 180.0f;

    if (lab) {
        Gpu *g = gpu_create(validation);
        int r = lab_run(g, lab, assets, quick);
        gpu_destroy(g);
        return r;
    }
    if (!scene || !out) { usage(); return 2; }
    Scene s;
    if (!scene_build(&s, scene, assets)) return 1;
    Gpu *g = gpu_create(validation);
    gpu_load_scene(g, &s);
    TexGrid t = tex_grid(&s, tpm, margin);
    Capture c;
    gpu_capture(g, &s, &v, &t, &c);
    bool ok = bake_write(out, &s, &c, &ic);
    free(c.layers);
    free(c.counts);
    gpu_destroy(g);
    scene_free(&s);
    return ok ? 0 : 1;
}
