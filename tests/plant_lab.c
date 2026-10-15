/* Lobby plant lab: builds every archetype over many seeds, quality tiers and
 * gravity settings (section 11 of the specification), and checks that each
 * one validates, is deterministic and keeps to its budget. Reports
 * generation time, triangle and leaf counts. With an output directory it also
 * renders contact sheets: every species over a row of seeds, the three
 * quality tiers, and gravity 0 / 0.85 / 1.5.
 *
 *   plant_lab [seeds] [out_dir] */
#include "plant.h"
#include "hash.h"
#include "mathlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); g_fail++; } } while (0)

/* ---------------------------------------------------------------- render --- */

typedef struct Image { int w, h; unsigned char *p; float *z; } Image;

static Image image_new(int w, int h) {
    Image im = { w, h, (unsigned char *)malloc((size_t)w * h * 3), (float *)malloc(sizeof(float) * (size_t)w * h) };
    for (int i = 0; i < w * h; ++i) {
        float t = (float)(i / w) / (float)h;
        im.p[i * 3 + 0] = (unsigned char)(34 + 20 * t); im.p[i * 3 + 1] = (unsigned char)(30 + 16 * t); im.p[i * 3 + 2] = (unsigned char)(28 + 12 * t);
        im.z[i] = -1e30f;
    }
    return im;
}

typedef struct Cam { vec3 V, X, Y; float scale, ox, oy; } Cam;

static Cam camera(float az, float el, float scale, float ox, float oy) {
    Cam c;
    c.V = v3(cosf(el) * sinf(az), sinf(el), cosf(el) * cosf(az));
    c.X = v3(cosf(az), 0.0f, -sinf(az));
    c.Y = v3_cross(c.V, c.X);
    c.scale = scale; c.ox = ox; c.oy = oy;
    return c;
}

static void leaf_colour(int kind, float out[3]) {
    static const float C[LS_COUNT][3] = {
        { 0.20f, 0.42f, 0.14f }, { 0.12f, 0.30f, 0.12f }, { 0.16f, 0.36f, 0.10f },
        { 0.14f, 0.34f, 0.10f }, { 0.22f, 0.46f, 0.12f }, { 0.08f, 0.26f, 0.10f } };
    memcpy(out, C[kind < LS_COUNT ? kind : 0], sizeof(float) * 3);
}
static void wood_colour(int kind, float out[3]) {
    static const float C[WM_COUNT][3] = {
        { 0.36f, 0.38f, 0.28f }, { 0.34f, 0.28f, 0.22f }, { 0.42f, 0.38f, 0.32f }, { 0.44f, 0.40f, 0.32f },
        { 0.26f, 0.42f, 0.16f }, { 0.20f, 0.36f, 0.14f }, { 0.38f, 0.30f, 0.22f } };
    memcpy(out, C[kind < WM_COUNT ? kind : 0], sizeof(float) * 3);
}

static void raster(Image *im, const Cam *c, const PlantMesh *m) {
    vec3 L = v3_norm(v3(-0.4f, 0.85f, 0.35f));
    for (uint32_t i = 0; i + 2 < m->count; i += 3) {
        const PlantVertex *v = &m->v[i];
        vec3 P[3];
        for (int k = 0; k < 3; ++k) P[k] = v3(v[k].p[0], v[k].p[1], v[k].p[2]);
        vec3 N = v3_cross(v3_sub(P[1], P[0]), v3_sub(P[2], P[0]));
        int under = v3_dot(N, c->V) < 0.0f;          /* not culled: a leaf is seen from both sides */
        float sx[3], sy[3], sz[3];
        for (int k = 0; k < 3; ++k) {
            sx[k] = c->ox + v3_dot(P[k], c->X) * c->scale;
            sy[k] = c->oy - v3_dot(P[k], c->Y) * c->scale;
            sz[k] = v3_dot(P[k], c->V);
        }
        float alb[3];
        if (v->part == PP_LEAF) leaf_colour(v->kind, alb); else wood_colour(v->kind, alb);
        float tint = 0.85f + 0.3f * (float)v->tint / 255.0f;
        int x0 = (int)floorf(fminf(sx[0], fminf(sx[1], sx[2]))), x1 = (int)ceilf(fmaxf(sx[0], fmaxf(sx[1], sx[2])));
        int y0 = (int)floorf(fminf(sy[0], fminf(sy[1], sy[2]))), y1 = (int)ceilf(fmaxf(sy[0], fmaxf(sy[1], sy[2])));
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > im->w - 1) x1 = im->w - 1; if (y1 > im->h - 1) y1 = im->h - 1;
        float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
        if (fabsf(area) < 1e-9f) continue;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                float px = (float)x + 0.5f, py = (float)y + 0.5f;
                float w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area;
                float w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area;
                float w2 = 1.0f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                float z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
                int idx = y * im->w + x;
                if (z <= im->z[idx]) continue;
                im->z[idx] = z;
                vec3 n = v3_norm(v3(w0 * v[0].n[0] + w1 * v[1].n[0] + w2 * v[2].n[0],
                                    w0 * v[0].n[1] + w1 * v[1].n[1] + w2 * v[2].n[1],
                                    w0 * v[0].n[2] + w1 * v[1].n[2] + w2 * v[2].n[2]));
                float ao = (w0 * v[0].ao + w1 * v[1].ao + w2 * v[2].ao) / 255.0f;
                if (under) n = v3_scale(n, -1.0f);
                float d = fmaxf(v3_dot(n, L), 0.0f), sh = (0.22f + 0.85f * d) * (0.35f + 0.65f * ao);
                if (v->part == PP_LEAF && under) sh *= 0.85f;
                for (int k = 0; k < 3; ++k) im->p[idx * 3 + k] = (unsigned char)fminf(255.0f, 255.0f * powf(alb[k] * tint * sh, 1.0f / 2.2f));
            }
    }
}

/* A dark pot under the soil line, for scale. */
static void pot(Image *im, const Cam *c, float half, float depth) {
    int x0 = (int)(c->ox - half * c->scale), x1 = (int)(c->ox + half * c->scale);
    int y0 = (int)(c->oy), y1 = (int)(c->oy + depth * c->scale);
    for (int y = y0; y <= y1 && y < im->h; ++y)
        for (int x = x0; x <= x1; ++x) {
            if (x < 0 || x >= im->w || y < 0) continue;
            int idx = y * im->w + x;
            if (im->z[idx] > -1e29f) continue;
            im->p[idx * 3 + 0] = 18; im->p[idx * 3 + 1] = 18; im->p[idx * 3 + 2] = 20;
        }
}

/* ------------------------------------------------------------------ lab --- */

static double now(void) { return (double)clock() / CLOCKS_PER_SEC; }

static uint32_t tri_count(const PlantMesh *m) { return m->count / 3; }

int main(int argc, char **argv) {
    int seeds = argc > 1 ? atoi(argv[1]) : 12;
    const char *out = argc > 2 ? argv[2] : NULL;
    static const float GRAV[3] = { 0.0f, 0.85f, 1.5f };
    static const char *QN[PQ_COUNT] = { "low", "medium", "high" };
    static const int TRI_LIMIT[PQ_COUNT] = { 5000, 14000, 28000 };

    printf("Lobby plants, %s: %d seeds x %d tiers x %d gravities per species\n", PLANT_SPEC_ID, seeds, PQ_COUNT, 3);
    printf("%-22s %-6s %8s %8s %8s %8s %7s %7s %6s %6s\n", "species", "tier", "tris", "maxTris", "leaves", "leaflets", "colony", "passes", "ms", "maxMs");
    for (int sp = 0; sp < PS_COUNT; ++sp) {
        for (int q = 0; q < PQ_COUNT; ++q) {
            double tris = 0, leaves = 0, leaflets = 0, colony = 0, passes = 0, ms = 0, maxMs = 0;
            uint32_t maxTris = 0;
            int n = 0;
            for (int s = 0; s < seeds; ++s)
                for (int gi = 0; gi < 3; ++gi) {
                    PlantParams p;
                    plant_defaults(&p, sp, hash_u32((uint32_t)s * 7919u + 13u));
                    p.quality = q;
                    p.gravity = GRAV[gi];
                    double t0 = now();
                    PlantMesh a, b;
                    int ok = plant_build(&p, &a);
                    double dt = (now() - t0) * 1000.0;
                    CHECK(ok, "%s %s seed %d gravity %.2f failed validation", plant_info(sp)->id, QN[q], s, GRAV[gi]);
                    if (gi == 1) {
                        int ok2 = plant_build(&p, &b);
                        CHECK(ok2 && a.count == b.count && !memcmp(a.v, b.v, sizeof(PlantVertex) * a.count),
                              "%s %s seed %d is not deterministic", plant_info(sp)->id, QN[q], s);
                        plant_mesh_free(&b);
                    }
                    CHECK(tri_count(&a) <= (uint32_t)TRI_LIMIT[q], "%s %s seed %d: %u triangles over the %d budget",
                          plant_info(sp)->id, QN[q], s, tri_count(&a), TRI_LIMIT[q]);
                    CHECK(!ok || a.mx[1] <= 1.45f * p.height, "%s %s seed %d: %.2f m tall for %.2f m", plant_info(sp)->id, QN[q], s, a.mx[1], p.height);
                    tris += tri_count(&a); leaves += a.st.leaves; leaflets += a.st.leaflets; colony += a.st.colonized; passes += a.st.passes;
                    ms += dt; if (dt > maxMs) maxMs = dt;
                    if (tri_count(&a) > maxTris) maxTris = tri_count(&a);
                    n++;
                    plant_mesh_free(&a);
                }
            printf("%-22s %-6s %8.0f %8u %8.0f %8.0f %7.1f %7.1f %6.2f %6.2f\n", plant_info(sp)->id, QN[q], tris / n, maxTris,
                   leaves / n, leaflets / n, colony / n, passes / n, ms / n, maxMs);
        }
    }

    if (out) {
        char path[512];
        int cw = 300, ch = 330, cols = seeds < 6 ? seeds : 6;
        /* species x seeds, medium, default gravity */
        Image im = image_new(cw * cols, ch * PS_COUNT);
        for (int sp = 0; sp < PS_COUNT; ++sp)
            for (int s = 0; s < cols; ++s) {
                PlantParams p;
                plant_defaults(&p, sp, hash_u32((uint32_t)s * 7919u + 13u));
                PlantMesh m;
                plant_build(&p, &m);
                float scale = (float)ch * 0.78f / 2.3f;
                Cam c = camera(0.5f + (float)s * 0.9f, 0.18f, scale, (float)(s * cw + cw / 2), (float)(sp * ch) + ch * 0.82f);
                raster(&im, &c, &m);
                pot(&im, &c, 0.4f, 0.12f);
                plant_mesh_free(&m);
            }
        snprintf(path, sizeof(path), "%s/plants_species.png", out);
        stbi_write_png(path, im.w, im.h, 3, im.p, im.w * 3);
        free(im.p); free(im.z);
        /* tiers and gravity, one seed */
        Image t = image_new(cw * 6, ch * PS_COUNT);
        for (int sp = 0; sp < PS_COUNT; ++sp)
            for (int k = 0; k < 6; ++k) {
                PlantParams p;
                plant_defaults(&p, sp, hash_u32(7919u * 3u + 13u));
                if (k < 3) p.quality = k; else p.gravity = GRAV[k - 3];
                PlantMesh m;
                plant_build(&p, &m);
                Cam c = camera(0.9f, 0.18f, (float)ch * 0.78f / 2.3f, (float)(k * cw + cw / 2), (float)(sp * ch) + ch * 0.82f);
                raster(&t, &c, &m);
                pot(&t, &c, 0.4f, 0.12f);
                plant_mesh_free(&m);
            }
        snprintf(path, sizeof(path), "%s/plants_tiers_gravity.png", out);
        stbi_write_png(path, t.w, t.h, 3, t.p, t.w * 3);
        free(t.p); free(t.z);
        /* close-ups: one plant per species, larger */
        Image cl = image_new(600 * 3, 640 * 2);
        for (int sp = 0; sp < PS_COUNT; ++sp) {
            PlantParams p;
            plant_defaults(&p, sp, hash_u32(7919u * 5u + 13u));
            p.quality = PQ_HIGH;
            PlantMesh m;
            plant_build(&p, &m);
            float sc = 640.0f * 0.8f / (p.height * 1.15f);
            Cam c = camera(0.6f, 0.3f, sc, (float)((sp % 3) * 600 + 300), (float)((sp / 3) * 640) + 640 * 0.86f);
            raster(&cl, &c, &m);
            plant_mesh_free(&m);
        }
        snprintf(path, sizeof(path), "%s/plants_closeup.png", out);
        stbi_write_png(path, cl.w, cl.h, 3, cl.p, cl.w * 3);
        free(cl.p); free(cl.z);
        printf("wrote contact sheets to %s\n", out);
    }
    if (g_fail) { printf("%d failure(s)\n", g_fail); return 1; }
    printf("all plant tests passed\n");
    return 0;
}
