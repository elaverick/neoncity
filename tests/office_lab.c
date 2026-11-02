/* interior_lab office [layouts] [out_dir]: the generated office floors behind the interior
 * impostors (tools/interior_bake/floor_gen.c, docs/interior-impostors.md, section 17). Plans every
 * module of the catalogue and a sweep of random inputs: each must pass validation, plan the same
 * twice and be different from the rest of the catalogue. Given out_dir, draws the catalogue's plans,
 * glass at the bottom: zones, solids, what can be walked to, and the lights on in each state. */
#include "floor_gen.h"
#include "plant.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "stb_image_write.h"

#define PPM 20.0f   /* pixels a metre */

typedef struct Img { int w, h; uint8_t *p; } Img;

static void rect(Img *im, float x0, float y0, float x1, float y1, int r, int g, int b) {
    int i0 = (int)floorf(x0), i1 = (int)ceilf(x1), j0 = (int)floorf(y0), j1 = (int)ceilf(y1);
    for (int y = j0 < 0 ? 0 : j0; y < j1 && y < im->h; ++y)
        for (int x = i0 < 0 ? 0 : i0; x < i1 && x < im->w; ++x) {
            uint8_t *q = im->p + 3 * ((size_t)y * im->w + x);
            q[0] = (uint8_t)r; q[1] = (uint8_t)g; q[2] = (uint8_t)b;
        }
}

static void draw_plan(const FloorPlan *P, int state, const char *path) {
    static const uint8_t ZC[FZ_COUNT][3] = { { 66, 72, 88 }, { 70, 92, 104 }, { 96, 72, 60 }, { 72, 78, 64 } };
    int nx = (int)floorf(P->xMax / 0.1f), nz = (int)floorf(P->zMax / 0.1f);
    uint8_t *mask = malloc((size_t)nx * nz);
    if (!mask) return;
    char why[200];
    floor_validate(P, why, sizeof(why), mask);
    Img im;
    im.w = (int)(P->xMax * PPM) + 40; im.h = (int)(P->p.D * PPM) + 40;
    im.p = malloc((size_t)im.w * im.h * 3);
    if (!im.p) { free(mask); return; }
    memset(im.p, 18, (size_t)im.w * im.h * 3);
    float ox = 20.0f, oy = (float)im.h - 20.0f;
#define X(v) (ox + (v) * PPM)
#define Y(v) (oy - (v) * PPM)
    /* the floor: what can be walked to is lighter; the rest of the plan's area dark */
    rect(&im, X(0), Y(P->zMax), X(P->xMax), Y(0), 30, 30, 34);
    for (int j = 0; j < nz; ++j)
        for (int i = 0; i < nx; ++i)
            if (mask[(size_t)j * nx + i] == 2) rect(&im, X(i * 0.1f), Y((j + 1) * 0.1f), X((i + 1) * 0.1f), Y(j * 0.1f), 52, 52, 58);
    for (int z = 0; z < P->nzone; ++z) {
        const FloorZone *Zn = &P->zone[z];
        const uint8_t *c = ZC[Zn->kind];
        float x0 = X(Zn->x0), y0 = Y(Zn->z1), x1 = X(Zn->x1), y1 = Y(Zn->z0);
        rect(&im, x0, y0, x1, y0 + 1, c[0] * 2, c[1] * 2, c[2] * 2);
        rect(&im, x0, y1 - 1, x1, y1, c[0] * 2, c[1] * 2, c[2] * 2);
        rect(&im, x0, y0, x0 + 1, y1, c[0] * 2, c[1] * 2, c[2] * 2);
        rect(&im, x1 - 1, y0, x1, y1, c[0] * 2, c[1] * 2, c[2] * 2);
    }
    for (int i = 0; i < P->nitem; ++i) {
        const FloorItem *it = &P->item[i];
        int r = 120, g = 120, b = 120;
        switch (it->kind) {
        case FI_COLUMN:  r = 200; g = 200; b = 200; break;
        case FI_CORE:    r = 90; g = 80; b = 70; break;
        case FI_DOOR:    r = 250; g = 180; b = 60; break;
        case FI_DESK:    r = 104; g = 84; b = 60; break;
        case FI_PANEL:   r = 180; g = 170; b = 150; break;
        case FI_LOUNGE:  r = 150; g = 90; b = 70; break;
        case FI_WALL:    r = 120; g = 80; b = 50; break;
        case FI_ART:     r = 240; g = 60; b = 90; break;
        case FI_FILING:  r = 130; g = 140; b = 110; break;
        case FI_PRINTER: r = 90; g = 110; b = 90; break;
        case FI_PLANT:   r = 60; g = 170; b = 70; break;
        default: continue;   /* lights and rooms below */
        }
        float gr = it->kind == FI_DESK ? 1.0f : 0.0f;
        rect(&im, X(it->x0) + gr, Y(it->z1) + gr, X(it->x1) - gr, Y(it->z0) - gr, r, g, b);
    }
    for (int i = 0; i < P->nsolid; ++i) {   /* the rooms' walls and tables, the lounges' furniture */
        const FloorRect *s = &P->solid[i];
        int kind = P->item[s->item].kind;
        if (kind == FI_ROOM) rect(&im, X(s->x0), Y(s->z1), X(s->x1), Y(s->z0), 90, 170, 220);
        if (kind == FI_LOUNGE) rect(&im, X(s->x0), Y(s->z1), X(s->x1), Y(s->z0), 190, 130, 95);
    }
    /* the fittings on in this state, sized by their lumens */
    for (int i = 0; i < P->nlight; ++i) {
        const FloorLight *l = &P->light[i];
        if (!(l->bits & (1u << state))) continue;
        float rd = 1.0f + sqrtf(l->lumens) * 0.05f;
        rect(&im, X(l->x) - rd, Y(l->z) - rd, X(l->x) + rd, Y(l->z) + rd, 255, 210, 90);
    }
    for (int i = 0; i < P->nitem; ++i) {
        const FloorItem *it = &P->item[i];
        if (it->kind != FI_DESK || !((it->bit[0] | it->bit[1]) & (1u << state))) continue;
        rect(&im, X(it->ox) - 2, Y(it->oz) - 2, X(it->ox) + 2, Y(it->oz) + 2, 110, 190, 255);
    }
    /* the glass */
    rect(&im, X(0), Y(0), X(P->xMax), Y(0) + 3, 120, 200, 255);
    if (P->p.core != FLOOR_CORE_BACK) rect(&im, X(0), Y(P->p.D) - 3, X(P->xMax), Y(P->p.D), 120, 200, 255);
    if (P->p.corner) rect(&im, X(0) - 3, Y(P->zMax), X(0), Y(0), 120, 200, 255);
    /* a tile's seam */
    if (!P->p.corner) for (int y = 0; y < im.h; y += 8) { rect(&im, X(0), (float)y, X(0) + 1, (float)y + 3, 255, 90, 90); rect(&im, X(P->W) - 1, (float)y, X(P->W), (float)y + 3, 255, 90, 90); }
#undef X
#undef Y
    stbi_write_png(path, im.w, im.h, 3, im.p, im.w * 3);
    free(im.p);
    free(mask);
}

static uint32_t next_u(uint32_t *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }

int office_mode(int argc, char **argv) {
    int layouts = argc > 2 ? atoi(argv[2]) : 400;
    const char *outDir = argc > 3 ? argv[3] : NULL;
    FloorPlan *P = malloc(sizeof(FloorPlan)), *Q = malloc(sizeof(FloorPlan));
    if (!P || !Q) return 1;
    int failures = 0, firsts = 0, passed = 0, attempts[9] = { 0 }, kinds[FI_COUNT] = { 0 }, zones[FZ_COUNT] = { 0 };
    if (!floor_catalogue_load(NEONCITY_ASSETS)) return 1;
    uint64_t *hashes = malloc(sizeof(uint64_t) * (size_t)FLOOR_CATALOGUE_COUNT);
    if (!hashes) return 1;

    for (int i = 0; i < FLOOR_CATALOGUE_COUNT; ++i) {
        const FloorEntry *e = &FLOOR_CATALOGUE[i];
        bool ok = floor_plan(&e->p, P);
        if (!ok) { printf("FAIL: catalogue module %s: %s\n", e->name, P->why); failures++; continue; }
        floor_plan(&e->p, Q);
        uint64_t h = floor_hash(P);
        if (h != floor_hash(Q) || P->nsolid != Q->nsolid) { printf("FAIL: %s differs when replanned\n", e->name); failures++; }
        hashes[i] = h;
        for (int k = 0; k < i; ++k) if (hashes[k] == h) { printf("FAIL: %s has the same layout as %s\n", e->name, FLOOR_CATALOGUE[k].name); failures++; }
        int nz[FZ_COUNT] = { 0 }, nk[FI_COUNT] = { 0 };
        for (int z = 0; z < P->nzone; ++z) nz[P->zone[z].kind]++;
        for (int k = 0; k < P->nitem; ++k) nk[P->item[k].kind]++;
        float lit[LIGHTS_COUNT] = { 0 };
        for (int s = 0; s < LIGHTS_COUNT; ++s) {
            for (int z = 0; z < P->nzone; ++z) lit[s] += floor_lux(P, s, (P->zone[z].x0 + P->zone[z].x1) * 0.5f, (P->zone[z].z0 + P->zone[z].z1) * 0.5f);
            lit[s] /= (float)(P->nzone ? P->nzone : 1);
        }
        printf("%-18s %4.1f x %4.1f m, %d attempt(s): %2d desks, %d rooms, %d lounges, %2d zones (work %d, store %d), %d plants, %d columns; zones' mean lux %3.0f/%3.0f/%3.0f/%3.0f\n",
               e->name, P->W, e->p.D, P->attempts, nk[FI_DESK], nk[FI_ROOM], nk[FI_LOUNGE], P->nzone, nz[FZ_WORK], nz[FZ_STORE], nk[FI_PLANT], nk[FI_COLUMN], lit[0], lit[1], lit[2], lit[3]);
        if (outDir) {
            char path[600];
            static const char *const ST[LIGHTS_COUNT] = { "after_hours", "late_shift", "cleaners", "screens" };
            snprintf(path, sizeof(path), "%s/plan_%s.png", outDir, e->name);
            draw_plan(P, e->state, path);
            for (int s = 0; s < LIGHTS_COUNT; ++s) {
                snprintf(path, sizeof(path), "%s/state_%s_%s.png", outDir, e->name, ST[s]);
                if (i == 6) draw_plan(P, s, path);   /* one module's four states */
            }
        }
    }

    /* the sweep: the inputs a catalogue could ask for */
    uint32_t r = 0xC0FFEE11u;
    double worst = 0.0, total = 0.0;
    int bad = 0, shown = 0;
    for (int n = 0; n < layouts; ++n) {
        FloorParams p;
        memset(&p, 0, sizeof(p));
        static const float BW[4] = { 2.7f, 3.0f, 3.3f, 3.6f }, HS[4] = { 3.6f, 3.8f, 4.0f, 4.2f };
        p.bays = 3 + (int)(next_u(&r) % 3u);
        p.bayW = BW[next_u(&r) % 4u];
        p.H = HS[next_u(&r) % 4u];
        p.style = (int)(next_u(&r) % FS_COUNT);
        p.corner = next_u(&r) % 8u == 0u;
        p.core = p.corner ? FLOOR_CORE_BACK : (int)(next_u(&r) % 3u);
        p.D = 8.0f + (float)(next_u(&r) % 25u);
        if (p.core == FLOOR_CORE_CENTRE && p.D < 20.0f) p.D = 20.0f + p.D * 0.5f;
        p.seed = next_u(&r);
        clock_t t0 = clock();
        bool ok = floor_plan(&p, P);
        double dt = (double)(clock() - t0) / CLOCKS_PER_SEC;
        total += dt;
        if (dt > worst) worst = dt;
        if (!ok) {
            bad++;
            if (shown++ < 12) printf("  rejected: %d x %.1f m, %.0f deep, H %.1f, core %s, corner %d, %s, seed %u: %s\n", p.bays, p.bayW, p.D, p.H, FLOOR_CORE_NAMES[p.core], p.corner, FLOOR_STYLE_NAMES[p.style], p.seed, P->why);
            continue;
        }
        passed++;
        firsts += P->attempts == 1;
        attempts[P->attempts]++;
        for (int k = 0; k < P->nitem; ++k) kinds[P->item[k].kind]++;
        for (int z = 0; z < P->nzone; ++z) zones[P->zone[z].kind]++;
        uint64_t h = floor_hash(P);
        floor_plan(&p, Q);
        if (h != floor_hash(Q)) { printf("FAIL: seed %u differs when replanned\n", p.seed); failures++; }
    }
    if (layouts) {
        printf("sweep: %d of %d passed (%.1f%%), %.1f%% first attempt; attempts:", passed, layouts, 100.0 * passed / layouts, passed ? 100.0 * firsts / passed : 0.0);
        for (int a = 1; a <= 8; ++a) printf(" %d:%d", a, attempts[a]);
        printf("\n  per layout: %.1f desks, %.1f rooms, %.1f lounges, %.1f plants, %.1f filing runs; zones %.1f work, %.1f room, %.1f lounge, %.1f store\n",
               (double)kinds[FI_DESK] / (passed ? passed : 1), (double)kinds[FI_ROOM] / (passed ? passed : 1), (double)kinds[FI_LOUNGE] / (passed ? passed : 1),
               (double)kinds[FI_PLANT] / (passed ? passed : 1), (double)kinds[FI_FILING] / (passed ? passed : 1),
               (double)zones[FZ_WORK] / (passed ? passed : 1), (double)zones[FZ_ROOM] / (passed ? passed : 1), (double)zones[FZ_LOUNGE] / (passed ? passed : 1), (double)zones[FZ_STORE] / (passed ? passed : 1));
        printf("  time: average %.1f ms, worst %.1f ms\n", 1000.0 * total / layouts, 1000.0 * worst);
        if (bad * 100 > layouts) { printf("FAIL: more than 1%% of the inputs found no valid layout\n"); failures++; }
    }
    free(P); free(Q); free(hashes);
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("every office floor valid and repeatable\n");
    return 0;
}
