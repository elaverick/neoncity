#include "bake.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/* The representation spike (IM1): bakes each test module many ways and
 * measures each against the truth. For every camera the truth is the scene
 * traced through the glass (truth.comp); the impostor's answer is the same
 * view reconstructed from the baked views alone (recon.comp, with the march
 * the facade shader will use). Errors are taken on tonemapped radiance:
 * PSNR, and the share of pixels off by more than BAD_ERR (seams and holes,
 * which PSNR averages away). Each impostor's memory is estimated in the
 * final block formats (section 6 of the design), its cost timed on a
 * 1600 x 900 dispatch.
 *
 * Cameras, in the module frame (the window's centre at x = W/2, half way up
 * the opening): from the street, 2 to 60 m below the window and 10 to 80 m
 * out, some well off to the side; and from level with it, and above it (from
 * another tower or in flight). */

#define DEG       (3.14159265f / 180.0f)
#define EVAL_PPM  40.0f    /* evaluation pixels a metre on the glass */
#define EVAL_SS   4        /* rays a pixel across, for the truth and the impostor alike: edges are compared anti-aliased */
#define EXPOSURE  2.0f
#define BAD_ERR   0.1f     /* a pixel this far off (tonemapped, 0..1) is visibly wrong */
#define COARSE    4        /* the coarse PSNR's blocks of pixels: structure (ghosts, holes, misplaced parts), not edge aliasing */
#define TIME_CAM  3        /* the camera the cost is timed from */

typedef struct CamSpec { float below, out, lateral; int street; const char *name; } CamSpec;
static const CamSpec CAMS[] = {
    { 2, 10, 0, 1, "street 2 m below, 10 m out" },      { 2, 12, 18, 1, "street 2 below, 12 out, 18 aside" },
    { 8, 15, 0, 1, "street 8 below, 15 out" },          { 10, 25, 30, 1, "street 10 below, 25 out, 30 aside" },
    { 25, 12, 4, 1, "street 25 below, 12 out" },        { 30, 40, -12, 1, "street 30 below, 40 out" },
    { 45, 30, 0, 1, "street 45 below, 30 out" },        { 60, 20, 8, 1, "street 60 below, 20 out" },
    { 6, 40, 55, 1, "street 6 below, 40 out, 55 aside" }, { 20, 70, -35, 1, "street 20 below, 70 out, 35 aside" },
    { 0, 10, 0, 0, "level, 10 out" },                    { 0, 30, 24, 0, "level, 30 out, 24 aside" },
    { 0, 80, 6, 0, "level, 80 out" },                    { -12, 40, 10, 0, "12 above, 40 out" },
    { -35, 45, -5, 0, "35 above, 45 out" },
};
#define NCAMS ((int)(sizeof(CAMS) / sizeof(CAMS[0])))
static const int SHEET_CAMS[3] = { 0, 3, 10 };

typedef struct CapSpec { int kind, nu, nv; float tpm, margin, azMax, elMin, elMax, s0; } CapSpec;   /* degrees; 0: the default range, s0 */
typedef struct RecSpec { int K, last, depth, backHalf, maxSteps, refine; float step, unknownW, sigma; int ends, nearest; } RecSpec;   /* nearest: unfiltered colour */

typedef struct Row {
    char   scene[32], id[96];
    double psnrSt, psnrLv, psnrAll, psnrCoarse, badPct, mb, ms, steps, guessed, offGrid;
} Row;

typedef struct Lab {
    Gpu       *g;
    Scene      s;
    const char *out;
    Camera     cams[NCAMS];
    EvalGrid   eval, timing;
    float     *truth[NCAMS];
    float     *rec;
    Row        rows[1024];
    int        nrows;
} Lab;

static float tonemap(float x) {
    x = fmaxf(x, 0.0f) * EXPOSURE;
    return powf(x / (1.0f + x), 1.0f / 2.2f);
}

/* The default ranges: azimuth and elevation +-70 and -40..80 degrees (the slope grid's top
 * moves, view_range), hemi-octahedral inset 0.8, s0 0.12 (a 3.4 m room 28 m deep). */
static ViewGrid view_grid(const CapSpec *c) {
    ViewGrid v = { c->nu, c->nv, c->kind, 70.0f * DEG, -40.0f * DEG, 80.0f * DEG, 0.8f, 0.12f };
    if (c->azMax > 0) { v.azMax = c->azMax * DEG; v.elMin = c->elMin * DEG; v.elMax = c->elMax * DEG; }
    if (c->s0 > 0) v.s0 = c->s0;
    return v;
}

static void cap_name(char *buf, size_t n, const CapSpec *c) {
    static const char *kinds[] = { "azel", "hoct", "slope" };
    snprintf(buf, n, "%s%dx%d r%.0f m%.1f", kinds[c->kind], c->nu, c->nv, c->tpm, c->margin);
    if (c->azMax > 0) snprintf(buf + strlen(buf), n - strlen(buf), " az%.0f el%.0f..%.0f", c->azMax, c->elMin, c->elMax);
    if (c->s0 > 0) snprintf(buf + strlen(buf), n - strlen(buf), " s0 %.2f", c->s0);
}

/* Bytes of the impostor in the final formats: BC6H lint, BC7 albedo and ext,
 * BC5 or RG16 depth; back layers at a quarter of the texels if halved; mips. */
static double impostor_mb(const TexGrid *t, int views, const ImpConfig *ic) {
    double texels = (double)t->nx * t->ny * views;
    if (ic->mpiSlices) return texels * ic->mpiSlices * 3.0 * (4.0 / 3.0) / (1024.0 * 1024.0);
    double layers = 1.0 + (ic->K - 1) * (ic->backHalf ? 0.25 : 1.0);
    double perTexel = 3.0 + (ic->depth == DEPTH_BC5 ? 1.0 : 4.0);
    return texels * layers * perTexel * (4.0 / 3.0) / (1024.0 * 1024.0);
}

static void heat(float e, uint8_t *p) {   /* black, red, yellow, white */
    float t = fminf(e * 4.0f, 1.0f) * 3.0f;
    p[0] = (uint8_t)(255.0f * fminf(t, 1.0f));
    p[1] = (uint8_t)(255.0f * fminf(fmaxf(t - 1.0f, 0.0f), 1.0f));
    p[2] = (uint8_t)(255.0f * fminf(fmaxf(t - 2.0f, 0.0f), 1.0f));
}

/* Truth | impostor | error, for each of the sheet's cameras, top down. */
static void write_sheet(Lab *L, const char *id, float *const recs[3]) {
    int w = L->eval.w, h = L->eval.h, gap = 4, W = 3 * w + 2 * gap, H = 3 * h + 2 * gap;
    uint8_t *img = calloc((size_t)W * H * 3, 1);
    if (!img) return;
    for (int r = 0; r < 3; ++r) {
        const float *tr = L->truth[SHEET_CAMS[r]], *rc = recs[r];
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                size_t i = ((size_t)(h - 1 - y) * w + x) * 4;   /* the eval grid runs bottom up */
                uint8_t *row = img + ((size_t)(r * (h + gap) + y) * W) * 3;
                float err = 0.0f;
                for (int k = 0; k < 3; ++k) {
                    float a = tonemap(tr[i + k]), b = tonemap(rc[i + k]);
                    row[(x) * 3 + k] = (uint8_t)(a * 255.0f + 0.5f);
                    row[(w + gap + x) * 3 + k] = (uint8_t)(b * 255.0f + 0.5f);
                    err = fmaxf(err, fabsf(a - b));
                }
                heat(err, row + (2 * (w + gap) + x) * 3);
            }
    }
    char name[160], path[1400];
    snprintf(name, sizeof(name), "%s_%s", L->s.name, id);
    for (char *c = name; *c; ++c) if (*c == ' ' || *c == '/' || *c == '.') *c = '_';
    snprintf(path, sizeof(path), "%s/%s.png", L->out, name);
    stbi_write_png(path, W, H, 3, img, W * 3);
    free(img);
}

/* Reconstructs every camera from the uploaded impostor, scores it and times it. */
static void score(Lab *L, const ViewGrid *v, const TexGrid *t, const ImpConfig *ic, const char *id, bool sheet) {
    double se[2] = { 0, 0 }, bad[2] = { 0, 0 }, n[2] = { 0, 0 }, seC = 0, nC = 0;
    ReconStats st = { 0 };
    float *sheetRec[3] = { NULL, NULL, NULL };
    size_t px = (size_t)L->eval.w * L->eval.h;
    for (int c = 0; c < NCAMS; ++c) {
        gpu_recon(L->g, &L->s, v, t, ic, &L->cams[c], &L->eval, L->rec, &st, NULL);
        int k = CAMS[c].street ? 0 : 1;
        for (size_t i = 0; i < px; ++i) {
            float worst = 0.0f;
            for (int ch = 0; ch < 3; ++ch) {
                float d = tonemap(L->truth[c][i * 4 + ch]) - tonemap(L->rec[i * 4 + ch]);
                se[k] += (double)d * d;
                worst = fmaxf(worst, fabsf(d));
            }
            bad[k] += worst > BAD_ERR;
            n[k] += 1.0;
        }
        for (int by = 0; by + COARSE <= L->eval.h; by += COARSE)
            for (int bx = 0; bx + COARSE <= L->eval.w; bx += COARSE)
                for (int ch = 0; ch < 3; ++ch) {
                    double a = 0, b = 0;
                    for (int y = by; y < by + COARSE; ++y)
                        for (int x = bx; x < bx + COARSE; ++x) {
                            size_t i = ((size_t)y * L->eval.w + x) * 4 + ch;
                            a += tonemap(L->truth[c][i]);
                            b += tonemap(L->rec[i]);
                        }
                    double d = (a - b) / (COARSE * COARSE);
                    seC += d * d;
                    nC += 1.0;
                }
        for (int r = 0; r < 3; ++r)
            if (sheet && SHEET_CAMS[r] == c) {
                sheetRec[r] = malloc(px * 16);
                if (sheetRec[r]) memcpy(sheetRec[r], L->rec, px * 16);
            }
    }
    double ms;
    gpu_recon(L->g, &L->s, v, t, ic, &L->cams[TIME_CAM], &L->timing, NULL, NULL, &ms);
    Row *r = &L->rows[L->nrows++];
    snprintf(r->scene, sizeof(r->scene), "%s", L->s.name);
    snprintf(r->id, sizeof(r->id), "%s", id);
    r->psnrSt = 10.0 * log10(3.0 * n[0] / fmax(se[0], 1e-12));
    r->psnrLv = 10.0 * log10(3.0 * n[1] / fmax(se[1], 1e-12));
    r->psnrAll = 10.0 * log10(3.0 * (n[0] + n[1]) / fmax(se[0] + se[1], 1e-12));
    r->psnrCoarse = 10.0 * log10(nC / fmax(seC, 1e-12));
    r->badPct = 100.0 * (bad[0] + bad[1]) / (n[0] + n[1]);
    r->mb = impostor_mb(t, view_count(v), ic);
    r->ms = ms;
    r->steps = st.marched > 0 ? st.steps / st.marched : 0;
    r->guessed = st.marched > 0 ? 100.0 * st.guessed / st.marched : 0;
    r->offGrid = st.marched > 0 ? 100.0 * st.offGrid / st.marched : 0;
    printf("  %-58s PSNR %5.2f (street %5.2f, level %5.2f, coarse %5.2f)  bad %5.2f%%  %6.1f MB  %.3f ms  %4.1f steps  guessed %4.1f%%  off grid %4.1f%%\n",
           id, r->psnrAll, r->psnrSt, r->psnrLv, r->psnrCoarse, r->badPct, r->mb, r->ms, r->steps, r->guessed, r->offGrid);
    fflush(stdout);
    if (sheet && sheetRec[0] && sheetRec[1] && sheetRec[2]) write_sheet(L, id, sheetRec);
    for (int i = 0; i < 3; ++i) free(sheetRec[i]);
}

static ImpConfig imp_config(const RecSpec *r) {
    ImpConfig ic = { 0 };
    ic.K = r->K; ic.lastSlot = r->last; ic.depth = r->depth; ic.backHalf = r->backHalf;
    ic.maxSteps = r->maxSteps; ic.refine = r->refine; ic.maxTexStep = r->step; ic.unknownW = r->unknownW; ic.nearSigma = r->sigma; ic.bothEnds = r->ends; ic.filtered = !r->nearest;
    ic.offGridW = 0.1f;
    return ic;
}

static void rec_name(char *buf, size_t n, const char *cap, const RecSpec *r) {
    snprintf(buf, n, "%s K%d%s %s%s s%.0f/%d b%d u%.2f", cap, r->K, r->last ? "l" : "n", r->depth == DEPTH_BC5 ? "bc5" : "rg16",
             r->backHalf ? " half" : "", r->step, r->maxSteps, r->refine, r->unknownW);
    if (r->sigma > 0) snprintf(buf + strlen(buf), n - strlen(buf), " near%.1f", r->sigma);
    if (r->ends) snprintf(buf + strlen(buf), n - strlen(buf), " ends");
    if (r->nearest) snprintf(buf + strlen(buf), n - strlen(buf), " nearest");
}

static void run_capture(Lab *L, const CapSpec *cs, const RecSpec *recs, int nrecs, const int *sheets) {
    ViewGrid v = view_grid(cs);
    TexGrid t = tex_grid(&L->s, cs->tpm, cs->margin);
    Capture cap;
    gpu_capture(L->g, &L->s, &v, &t, &cap);
    char cn[64];
    cap_name(cn, sizeof(cn), cs);
    for (int i = 0; i < nrecs; ++i) {
        ImpConfig ic = imp_config(&recs[i]);
        size_t bytes;
        uint32_t *imp = imp_pack(&cap, &ic, &bytes);
        gpu_set_impostor(L->g, imp, bytes);
        char id[96];
        rec_name(id, sizeof(id), cn, &recs[i]);
        score(L, &v, &t, &ic, id, sheets && sheets[i]);
        free(imp);
    }
    free(cap.layers);
    free(cap.counts);
}

static void run_mpi(Lab *L, const CapSpec *cs, int slices, bool sheet) {
    ViewGrid v = view_grid(cs);
    TexGrid t = tex_grid(&L->s, cs->tpm, cs->margin);
    MpiCapture m;
    gpu_capture_mpi(L->g, &L->s, &v, &t, slices, &m);
    gpu_set_impostor(L->g, m.rgba, (size_t)view_count(&v) * slices * t.nx * t.ny * 8u);
    ImpConfig ic = { 0 };
    ic.K = 1; ic.mpiSlices = slices; ic.offGridW = 0.1f; ic.unknownW = 1.0f;
    char cn[64], id[96];
    cap_name(cn, sizeof(cn), cs);
    snprintf(id, sizeof(id), "%s MPI %d", cn, slices);
    score(L, &v, &t, &ic, id, sheet);
    free(m.rgba);
}

/* --------------------------------------------------------------- the sweep --- */

#define BASE_MARCH 48, 6, 2.0f, 0.15f, 0.0f, 0   /* maxSteps, refine, maxTexStep, unknownW, nearest-hit sigma, both ends */

static const RecSpec K_SET[] = {
    { 1, 0, DEPTH_RG16, 0, BASE_MARCH }, { 2, 0, DEPTH_RG16, 0, BASE_MARCH }, { 3, 0, DEPTH_RG16, 0, BASE_MARCH },
    { 4, 0, DEPTH_RG16, 0, BASE_MARCH }, { 2, 1, DEPTH_RG16, 0, BASE_MARCH }, { 3, 1, DEPTH_RG16, 0, BASE_MARCH },
    { 4, 1, DEPTH_RG16, 0, BASE_MARCH },
};
static const int K_SHEETS[] = { 1, 0, 1, 0, 0, 0, 0 };
static const RecSpec BASE_SET[] = { { 3, 0, DEPTH_RG16, 0, BASE_MARCH } };
static const RecSpec EXTRA_SET[] = {
    { 3, 0, DEPTH_BC5, 0, BASE_MARCH },        { 3, 0, DEPTH_RG16, 1, BASE_MARCH },        { 3, 0, DEPTH_BC5, 1, BASE_MARCH },
    { 3, 0, DEPTH_RG16, 0, 96, 6, 1.0f, 0.15f, 0.0f, 0 }, { 3, 0, DEPTH_RG16, 0, 48, 6, 4.0f, 0.15f, 0.0f, 0 },
    { 3, 0, DEPTH_RG16, 0, 16, 6, 2.0f, 0.15f, 0.0f, 0 }, { 3, 0, DEPTH_RG16, 0, 24, 6, 2.0f, 0.15f, 0.0f, 0 },
    { 3, 0, DEPTH_RG16, 0, 32, 6, 2.0f, 0.15f, 0.0f, 0 }, { 3, 0, DEPTH_RG16, 0, 48, 3, 2.0f, 0.15f, 0.0f, 0 },
    { 3, 0, DEPTH_RG16, 0, 48, 6, 2.0f, 1.0f, 0.0f, 0 },  { 3, 0, DEPTH_RG16, 0, 48, 6, 2.0f, 0.02f, 0.0f, 0 },
    { 3, 0, DEPTH_RG16, 0, 48, 6, 2.0f, 0.15f, 0.0f, 1 }, { 3, 0, DEPTH_RG16, 0, 24, 6, 2.0f, 0.15f, 0.0f, 1 },
    { 3, 0, DEPTH_RG16, 0, 48, 6, 2.0f, 0.15f, 1.0f, 0 }, { 3, 0, DEPTH_RG16, 0, 48, 6, 2.0f, 0.15f, 0.0f, 0, 1 },
};
static const int EXTRA_SHEETS[] = { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
static const int ONE_SHEET[] = { 1 };

/* IMP_SELFTEST=1 (a 4 x 4 grid), 5 (5 x 5) or 2 (the slope grid): cameras far along baked views'
 * own directions, and half way between two. Along a view the reconstruction is exact but for the
 * texels' size: a check of the capture, the view pick and the march, and the floor the
 * resolution sets on the scores. */
static void selftest(Lab *L) {
    CapSpec cs = { GRID_AZEL, 4, 4, 32.0f, 1.0f, 0, 0, 0, 0 };
    float pairs[6][2] = { { 5, 5 }, { 6, 6 }, { 9, 9 }, { 10, 10 }, { 5, 6 }, { 5, 9 } };
    if (atoi(getenv("IMP_SELFTEST")) == 5) {   /* a 5 x 5 grid: the straight-on column of views */
        cs.nu = cs.nv = 5;
        const float p5[6][2] = { { 7, 7 }, { 12, 12 }, { 6, 6 }, { 11, 11 }, { 7, 12 }, { 6, 7 } };
        memcpy(pairs, p5, sizeof(pairs));
    } else if (atoi(getenv("IMP_SELFTEST")) == 2) {   /* the slope baseline, 4 x 10 at 16 texels a metre: the horizontal row is 3 */
        const CapSpec sl = { GRID_SLOPE, 4, 10, 16.0f, 2.0f, 0, 0, 0, 0 };
        cs = sl;
        const float p2[6][2] = { { 13, 13 }, { 17, 17 }, { 21, 21 }, { 14, 14 }, { 13, 17 }, { 13, 14 } };
        memcpy(pairs, p2, sizeof(pairs));
    }
    ViewGrid v = view_grid(&cs);
    TexGrid t = tex_grid(&L->s, cs.tpm, cs.margin);
    Capture cap;
    gpu_capture(L->g, &L->s, &v, &t, &cap);
    ImpConfig ic = imp_config(&BASE_SET[0]);
    size_t bytes;
    uint32_t *imp = imp_pack(&cap, &ic, &bytes);
    gpu_set_impostor(L->g, imp, bytes);
    size_t px = (size_t)L->eval.w * L->eval.h;
    float *tr = malloc(px * 16);
    for (int p = 0; p < 6; ++p) {
        float a[3], b[3], d[3];
        view_dir(&v, (int)pairs[p][0], a);
        view_dir(&v, (int)pairs[p][1], b);
        float l = 0;
        for (int k = 0; k < 3; ++k) { d[k] = a[k] + b[k]; l += d[k] * d[k]; }
        l = sqrtf(l);
        Camera c = { { 0 } };
        float cx = L->s.W * 0.5f, cy = (L->s.sill + L->s.head) * 0.5f;
        c.pos[0] = cx - d[0] / l * 3000.0f; c.pos[1] = cy - d[1] / l * 3000.0f; c.pos[2] = -d[2] / l * 3000.0f;
        gpu_truth(L->g, &L->s, &c, &L->eval, tr);
        ReconStats st = { 0 };
        gpu_recon(L->g, &L->s, &v, &t, &ic, &c, &L->eval, L->rec, &st, NULL);
        double se = 0;
        int bad = 0;
        for (size_t i = 0; i < px; ++i) {
            float worst = 0;
            for (int ch = 0; ch < 3; ++ch) {
                float e = tonemap(tr[i * 4 + ch]) - tonemap(L->rec[i * 4 + ch]);
                se += (double)e * e;
                worst = fmaxf(worst, fabsf(e));
            }
            bad += worst > BAD_ERR;
        }
        printf("  selftest views %d+%d: PSNR %.2f, bad %.2f%%, steps %.1f, guessed %.1f%%, off grid %.1f%%\n", (int)pairs[p][0], (int)pairs[p][1],
               10.0 * log10(3.0 * px / fmax(se, 1e-12)), 100.0 * bad / px, st.steps / fmax(st.marched, 1), 100.0 * st.guessed / fmax(st.marched, 1),
               100.0 * st.offGrid / fmax(st.marched, 1));
        if (p == 0 || p == 4) {
            float *recs[3] = { L->rec, L->rec, L->rec };
            float *keep[3] = { L->truth[SHEET_CAMS[0]], L->truth[SHEET_CAMS[1]], L->truth[SHEET_CAMS[2]] };
            L->truth[SHEET_CAMS[0]] = L->truth[SHEET_CAMS[1]] = L->truth[SHEET_CAMS[2]] = tr;
            write_sheet(L, p == 0 ? "selftest_view5" : "selftest_view5_6", recs);
            for (int r = 0; r < 3; ++r) L->truth[SHEET_CAMS[r]] = keep[r];
        }
    }
    free(tr);
    free(imp);
    free(cap.layers);
    free(cap.counts);
}

/* The baseline is the slope grid's 4 x 10 at 16 texels a metre with 2 m margins; each block varies
 * one thing from it. The azimuth-elevation and hemi-octahedral grids stay for the record, and the
 * candidates last (run_final). */
/* The candidates the sweep chose between (docs/interior-impostors.md, section 13): the second
 * capture's is the choice, the baker's default (main.c). */
#define CHOSEN_CAP_INIT { GRID_SLOPE, 5, 10, 16.0f, 1.0f, 60, -30, 70, 0 }
static const CapSpec CHOSEN_CAP = CHOSEN_CAP_INIT;
#define CHOSEN_REC_INIT { 3, 0, DEPTH_BC5, 0, 64, 6, 1.0f, 0.15f, 0.0f, 0, 1 }
static const RecSpec CHOSEN_REC = CHOSEN_REC_INIT;

static void run_final(Lab *L) {
    static const CapSpec caps[] = {
        { GRID_SLOPE, 4, 10, 16.0f, 1.0f, 60, -30, 70, 0 }, CHOSEN_CAP_INIT,
        { GRID_SLOPE, 6, 10, 16.0f, 1.0f, 60, -30, 70, 0 }, { GRID_SLOPE, 4, 12, 16.0f, 1.0f, 60, -30, 70, 0 },
        { GRID_SLOPE, 4, 10, 24.0f, 1.0f, 60, -30, 70, 0 }, { GRID_SLOPE, 4, 10, 12.0f, 1.0f, 60, -30, 70, 0 },
    };
    static const RecSpec recs[] = {
        { 3, 0, DEPTH_RG16, 0, 64, 6, 1.0f, 0.15f, 0.0f, 0, 1 }, CHOSEN_REC_INIT,
        { 2, 0, DEPTH_BC5, 0, 64, 6, 1.0f, 0.15f, 0.0f, 0, 1 }, { 3, 0, DEPTH_BC5, 0, 96, 6, 1.0f, 0.15f, 0.0f, 0, 1 },
        { 3, 0, DEPTH_BC5, 0, 48, 6, 1.0f, 0.15f, 0.0f, 0, 1 },
    };
    run_capture(L, &caps[0], recs, 5, NULL);
    for (int i = 1; i < (int)(sizeof(caps) / sizeof(caps[0])); ++i) run_capture(L, &caps[i], recs + 1, 1, i == 1 ? ONE_SHEET : NULL);
}

static void run_scene(Lab *L, bool quick) {
    if (getenv("IMP_SELFTEST")) { selftest(L); return; }
    const CapSpec base = { GRID_SLOPE, 4, 10, 16.0f, 2.0f, 0, 0, 0, 0 };
    if (quick) {   /* the choice alone */
        run_capture(L, &CHOSEN_CAP, &CHOSEN_REC, 1, ONE_SHEET);
        return;
    }
    run_capture(L, &base, K_SET, 7, K_SHEETS);
    run_capture(L, &base, EXTRA_SET, (int)(sizeof(EXTRA_SET) / sizeof(EXTRA_SET[0])), EXTRA_SHEETS);
    static const CapSpec grids[] = {
        { GRID_SLOPE, 4, 6, 16.0f, 2.0f, 0, 0, 0, 0 },  { GRID_SLOPE, 4, 8, 16.0f, 2.0f, 0, 0, 0, 0 },
        { GRID_SLOPE, 4, 12, 16.0f, 2.0f, 0, 0, 0, 0 }, { GRID_SLOPE, 4, 14, 16.0f, 2.0f, 0, 0, 0, 0 },
        { GRID_SLOPE, 3, 10, 16.0f, 2.0f, 0, 0, 0, 0 }, { GRID_SLOPE, 5, 10, 16.0f, 2.0f, 0, 0, 0, 0 },
        { GRID_SLOPE, 6, 10, 16.0f, 2.0f, 0, 0, 0, 0 }, { GRID_SLOPE, 6, 12, 12.0f, 2.0f, 0, 0, 0, 0 },
        { GRID_SLOPE, 4, 10, 12.0f, 2.0f, 0, 0, 0, 0 }, { GRID_SLOPE, 4, 10, 24.0f, 2.0f, 0, 0, 0, 0 },
        { GRID_SLOPE, 4, 10, 32.0f, 2.0f, 0, 0, 0, 0 }, { GRID_SLOPE, 4, 10, 16.0f, 1.0f, 0, 0, 0, 0 },
        { GRID_SLOPE, 4, 10, 16.0f, 3.0f, 0, 0, 0, 0 }, { GRID_SLOPE, 4, 10, 16.0f, 2.0f, 0, 0, 0, 0.06f },
        { GRID_SLOPE, 4, 10, 16.0f, 2.0f, 0, 0, 0, 0.25f }, { GRID_SLOPE, 4, 10, 16.0f, 2.0f, 60, -30, 70, 0 },
        { GRID_AZEL, 4, 4, 32.0f, 1.0f, 0, 0, 0, 0 },   { GRID_AZEL, 6, 6, 16.0f, 2.0f, 0, 0, 0, 0 },
        { GRID_AZEL, 4, 10, 16.0f, 2.0f, 0, 0, 0, 0 },  { GRID_HEMIOCT, 5, 5, 16.0f, 2.0f, 0, 0, 0, 0 },
        { GRID_HEMIOCT, 7, 7, 16.0f, 2.0f, 0, 0, 0, 0 },
    };
    static const int gridSheets[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0 };
    for (int i = 0; i < (int)(sizeof(grids) / sizeof(grids[0])); ++i) run_capture(L, &grids[i], BASE_SET, 1, gridSheets[i] ? ONE_SHEET : NULL);
    static const int slices[] = { 16, 32, 64 };
    for (int i = 0; i < 3; ++i) run_mpi(L, &base, slices[i], slices[i] == 32);
    run_final(L);
}

static void write_results(Lab *L, const char *gpuName) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/results.csv", L->out);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "scene,config,psnr_all,psnr_street,psnr_level,psnr_coarse,bad_pct,mb,ms_1600x900,steps_per_view,guessed_pct,offgrid_pct\n");
        for (int i = 0; i < L->nrows; ++i) {
            Row *r = &L->rows[i];
            fprintf(f, "%s,%s,%.3f,%.3f,%.3f,%.3f,%.3f,%.2f,%.4f,%.2f,%.2f,%.2f\n", r->scene, r->id, r->psnrAll, r->psnrSt, r->psnrLv,
                    r->psnrCoarse, r->badPct, r->mb, r->ms, r->steps, r->guessed, r->offGrid);
        }
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s/results.md", L->out);
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# Interior impostor spike (IM1): results\n\nWritten by `interior_bake --lab` on %s. ", gpuName);
    fprintf(f, "%d cameras a scene (%d from the street), evaluated at %.0f px/m over the opening. ", NCAMS, 10, EVAL_PPM);
    fprintf(f, "PSNR on tonemapped radiance, each pixel the mean of %d x %d rays; *coarse*: PSNR over blocks of %d x %d pixels (structure, not edge aliasing); ",
            EVAL_SS, EVAL_SS, COARSE, COARSE);
    fprintf(f, "*bad*: pixels off by more than %.2f; MB in the final block formats; ", BAD_ERR);
    fprintf(f, "ms: the march for a 1600 x 900 view (uncompressed buffers); steps: a view's march; guessed / off grid: views whose march crossed undescribed depths, or left the texel grid.\n\n");
    fprintf(f, "Config: view grid NuxNv, texels a metre, margin above and below the floor (m); K layers, n nearest or l with the last surface kept; depth format; march: texels a step / most steps, bisections, weight of a guessing view.\n\n");
    /* the mean over the scenes, per config */
    fprintf(f, "## Mean over the scenes\n\n| config | PSNR | street | level | coarse | bad %% | MB (through30) | ms (worst) |\n|---|---|---|---|---|---|---|---|\n");
    for (int i = 0; i < L->nrows; ++i) {
        bool firstOfId = true;
        for (int j = 0; j < i; ++j) if (!strcmp(L->rows[j].id, L->rows[i].id)) firstOfId = false;
        if (!firstOfId) continue;
        double p = 0, st = 0, lv = 0, co = 0, bad = 0, mb = 0, ms = 0;
        int n = 0;
        for (int j = i; j < L->nrows; ++j) {
            Row *r = &L->rows[j];
            if (strcmp(r->id, L->rows[i].id)) continue;
            p += r->psnrAll; st += r->psnrSt; lv += r->psnrLv; co += r->psnrCoarse; bad += r->badPct; ms = fmax(ms, r->ms);
            if (!strcmp(r->scene, "through30")) mb = r->mb;
            n++;
        }
        fprintf(f, "| %s | %.2f | %.2f | %.2f | %.2f | %.2f | %.1f | %.3f |\n", L->rows[i].id, p / n, st / n, lv / n, co / n, bad / n, mb, ms);
    }
    for (int sc = 0; sc < SCENE_COUNT; ++sc) {
        fprintf(f, "\n## %s\n\n| config | PSNR | street | level | coarse | bad %% | MB | ms | steps | guessed %% | off grid %% |\n|---|---|---|---|---|---|---|---|---|---|---|\n", SCENE_NAMES[sc]);
        for (int i = 0; i < L->nrows; ++i) {
            Row *r = &L->rows[i];
            if (strcmp(r->scene, SCENE_NAMES[sc])) continue;
            fprintf(f, "| %s | %.2f | %.2f | %.2f | %.2f | %.2f | %.1f | %.3f | %.1f | %.1f | %.1f |\n", r->id, r->psnrAll, r->psnrSt, r->psnrLv,
                    r->psnrCoarse, r->badPct, r->mb, r->ms, r->steps, r->guessed, r->offGrid);
        }
    }
    fclose(f);
    printf("wrote %s/results.md and results.csv\n", L->out);
}

int lab_run(Gpu *g, const char *outDir, const char *assets, bool quick) {
    Lab *L = calloc(1, sizeof(Lab));
    if (!L) fatal("out of memory");
    L->g = g;
    L->out = outDir;
    for (int sc = 0; sc < SCENE_COUNT; ++sc) {
        if (!scene_build(&L->s, SCENE_NAMES[sc], assets)) return 1;
        gpu_load_scene(g, &L->s);
        Scene *s = &L->s;
        float cx = s->W * 0.5f, cy = (s->sill + s->head) * 0.5f;
        for (int c = 0; c < NCAMS; ++c) {
            Camera *cam = &L->cams[c];
            cam->pos[0] = cx + CAMS[c].lateral;
            cam->pos[1] = cy - CAMS[c].below;
            cam->pos[2] = -CAMS[c].out;
            cam->street = CAMS[c].street;
            snprintf(cam->name, sizeof(cam->name), "cam%d", c);
        }
        EvalGrid e = { 0.0f, s->sill, EVAL_PPM, EVAL_PPM, (int)lroundf(s->W * EVAL_PPM), (int)lroundf((s->head - s->sill) * EVAL_PPM), EVAL_SS };
        EvalGrid tg = { 0.0f, s->sill, 1600.0f / s->W, 900.0f / (s->head - s->sill), 1600, 900, 1 };
        L->eval = e;
        L->timing = tg;
        size_t px = (size_t)e.w * e.h;
        for (int c = 0; c < NCAMS; ++c) {
            L->truth[c] = malloc(px * 16);
            if (!L->truth[c]) fatal("out of memory");
            gpu_truth(g, s, &L->cams[c], &e, L->truth[c]);
        }
        L->rec = malloc(px * 16);
        if (!L->rec) fatal("out of memory");
        printf("%s:\n", s->name);
        run_scene(L, quick);
        for (int c = 0; c < NCAMS; ++c) free(L->truth[c]);
        free(L->rec);
        scene_free(&L->s);
    }
    write_results(L, gpu_name(g));
    free(L);
    return 0;
}
