/* Street vendor lab (docs/street-vendor-generation.md, SV0 of the vendor roadmap).
 *
 * Generates every archetype over many seeds, at every tier and affluence
 * class, and checks that each one validates (section 4), is deterministic
 * (section 12: twice, byte for byte, description and mesh), keeps to its
 * tier's budget (section 9) and, in rows of five sharing one archetype and
 * one trade, differs from its neighbours (section 8). Then the automatic mix
 * over random sites, by affluence: which archetypes and trades it picks.
 * With an output directory it renders contact sheets: each archetype at
 * poor, middling and wealthy, and a row of five neighbours.
 *
 *   vendor_lab [seeds] [out_dir] [verbose_seed]
 */
#include "vendor.h"
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

/* ----------------------------------------------------------------- sites --- */

static VendorSite site_for(int arch, uint32_t seed, float affluence) {
    VendorSite s;
    memset(&s, 0, sizeof(s));
    Rng r = rng_make(seed ^ 0x51735u);
    float w, d;
    vendor_min_site(arch, &w, &d);
    s.seed = seed;
    s.w = w + rng_range(&r, 0.2f, arch == VA_CONTAINER || arch == VA_PAIR ? 7.5f : 3.5f);
    s.d = d + rng_range(&r, 0.3f, 2.5f);
    s.affluence = affluence;
    s.archetype = (int8_t)arch;
    s.trade = -1;
    s.hostWall = arch == VA_LEANTO || rng_float(&r) < 0.4f;
    static const uint8_t KIND[VA_COUNT] = { VSITE_APRON, VSITE_KERB, VSITE_LANE, VSITE_COURT, VSITE_COURT, VSITE_POCKET, VSITE_KERB };
    s.kind = KIND[arch];
    if (rng_float(&r) < 0.15f) { s.kind = VSITE_ARCOLOGY; s.ceiling = rng_range(&r, 4.0f, 6.0f); }
    if (arch == VA_PAIR && s.kind == VSITE_ARCOLOGY) s.kind = VSITE_COURT, s.ceiling = 0.0f;
    return s;
}

static VendorSite site_random(uint32_t seed) {
    VendorSite s;
    memset(&s, 0, sizeof(s));
    Rng r = rng_make(seed ^ 0x5A7Du);
    s.seed = seed;
    s.kind = (uint8_t)(rng_u32(&r) % VSITE_COUNT);
    s.affluence = rng_float(&r);
    s.archetype = -1; s.trade = -1;
    switch (s.kind) {
    case VSITE_KERB:   s.w = rng_range(&r, 2.0f, 4.5f); s.d = rng_range(&r, 1.6f, 3.2f); break;
    case VSITE_APRON:  s.w = rng_range(&r, 3.0f, 8.0f); s.d = rng_range(&r, 3.0f, 6.0f); s.hostWall = 1; break;
    case VSITE_LANE:   s.w = rng_range(&r, 2.5f, 6.0f); s.d = rng_range(&r, 2.4f, 3.5f); s.hostWall = rng_float(&r) < 0.8f; break;
    case VSITE_COURT:  s.w = rng_range(&r, 4.0f, 14.0f); s.d = rng_range(&r, 4.0f, 9.0f); s.hostWall = rng_float(&r) < 0.5f; break;
    case VSITE_POCKET: s.w = rng_range(&r, 3.0f, 13.0f); s.d = rng_range(&r, 3.0f, 8.0f); break;
    case VSITE_PLAZA:  s.w = rng_range(&r, 3.0f, 13.0f); s.d = rng_range(&r, 3.0f, 7.0f); s.lunch = 1; s.affluence = 0.6f + 0.4f * s.affluence; break;
    default:           s.w = rng_range(&r, 3.0f, 7.0f); s.d = rng_range(&r, 2.6f, 5.0f); s.ceiling = rng_range(&r, 3.6f, 6.0f); break;
    }
    return s;
}

/* ------------------------------------------------------------- reporting --- */

typedef struct Reason { char text[64]; int n; } Reason;
static Reason g_reason[128];
static int g_nreason = 0;

/* Tallies each "reject:" line of a log by its words before the first number. */
static void tally(const char *log) {
    for (const char *p = strstr(log, "reject: "); p; p = strstr(p + 1, "reject: ")) {
        char key[64];
        int k = 0;
        const char *q = p + 8;
        while (*q && *q != '\n' && k < 60 && !(*q >= '0' && *q <= '9')) key[k++] = *q++;
        key[k] = 0;
        if (k < 3) continue;   /* a summary line ("reject: 900 triangles ...") */
        int i;
        for (i = 0; i < g_nreason; ++i) if (!strcmp(g_reason[i].text, key)) break;
        if (i == g_nreason && g_nreason < 128) { strcpy(g_reason[g_nreason].text, key); g_reason[g_nreason].n = 0; g_nreason++; }
        if (i < g_nreason) g_reason[i].n++;
    }
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

/* ----------------------------------------------------------------- render --- */

typedef struct Image { int w, h; unsigned char *p; float *z; } Image;

static float srgb_to_lin(float s) { return s <= 0.04045f ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f); }
static float lin_to_srgb(float l) { l = clampf(l, 0.0f, 1.0f); return l <= 0.0031308f ? l * 12.92f : 1.055f * powf(l, 1.0f / 2.4f) - 0.055f; }
static void hex_lin(uint32_t c, float o[3]) { for (int k = 0; k < 3; ++k) o[k] = srgb_to_lin((float)((c >> (16 - 8 * k)) & 255u) / 255.0f); }

static const uint32_t NEON[8] = { 0xFF1480u, 0x0DD9FFu, 0xFF1A0Au, 0xFF8010u, 0x8C1FFFu, 0x26FF66u, 0xA6D9FFu, 0xFFD926u };

/* A vertex's colour as the lab sees it: the surface's pattern over its palette slot or colour, and how much it glows. */
static void shade_vertex(const Vendor *v, const VendorVertex *x, float u, float vv, float col[3], float *glow) {
    float base[3];
    *glow = 0.0f;
    if (x->col == VCOL_RGB) hex_lin(x->rgb, base);
    else if (x->col <= VCOL_ACC2) vendor_palette_rgb(v->palette, x->col, base);
    else { float g[3] = { 0.5f, 0.5f, 0.5f }; memcpy(base, g, sizeof(g)); }
    float k = 1.0f;
    switch (x->surf) {
    case VS_CORRUGATED: k = 0.75f + 0.25f * sinf(u * 2.0f * PI_F / 0.08f); break;
    case VS_RUST: { float rust[3]; hex_lin(0x7A3A1Au, rust); for (int i = 0; i < 3; ++i) base[i] = lerpf(base[i], rust[i], 0.4f + 0.4f * v->wear); k = 0.8f + 0.2f * sinf(u * 2.0f * PI_F / 0.08f); break; }
    case VS_TIMBER: case VS_DECK: { float wood[3]; hex_lin(0x8A6038u, wood); for (int i = 0; i < 3; ++i) base[i] = lerpf(base[i], wood[i], 0.65f); k = fmodf(fabsf(vv), 0.15f) < 0.01f ? 0.5f : 1.0f; break; }
    case VS_TILE: k = fmodf(fabsf(u), 0.15f) < 0.01f || fmodf(fabsf(vv), 0.15f) < 0.01f ? 0.55f : 1.0f; break;
    case VS_BRICK: { float br[3]; hex_lin(0x8A3A2Au, br); for (int i = 0; i < 3; ++i) base[i] = lerpf(base[i], br[i], 0.6f);
                     float row = floorf(vv / 0.075f); k = fmodf(fabsf(vv), 0.075f) < 0.01f || fmodf(fabsf(u + 0.11f * fmodf(row, 2.0f)), 0.22f) < 0.01f ? 0.6f : 1.0f; break; }
    case VS_STRIPE: if (((int)floorf(u / 0.28f)) & 1) { base[0] = base[1] = base[2] = 0.8f; } break;
    case VS_STAINLESS: base[0] = base[1] = base[2] = 0.45f; break;
    case VS_CHROME: base[0] = base[1] = base[2] = 0.6f; break;
    case VS_RUBBER: base[0] = base[1] = base[2] = 0.02f; break;
    case VS_GLASS: for (int i = 0; i < 3; ++i) base[i] *= 0.6f; break;
    case VS_SIGN: base[0] = base[1] = base[2] = 0.025f; break;
    case VS_ROOM: {
        /* the painted room: shelves of stock under the lamp */
        float lamp[3];
        hex_lin(x->rgb, lamp);
        float shelf = fmodf(vv, 0.4f) < 0.04f ? 0.2f : 0.55f + 0.35f * hash_float(hash2i(v->site.seed, (int)floorf(u / 0.12f), (int)floorf(vv / 0.4f)));
        for (int i = 0; i < 3; ++i) base[i] = lamp[i] * shelf * 0.5f;
        *glow = 1.0f;
        break;
    }
    case VS_NEON:
        if (x->rgb & (1u << 30)) { base[0] = 0.6f; base[1] = 0.55f; base[2] = 0.45f; k = ((int)floorf(u / 0.12f) & 1) && fmodf(vv, 0.3f) > 0.08f ? 1.0f : 0.05f; }
        else { hex_lin(NEON[x->rgb & 7u], base); *glow = (int)(fmodf(fabsf(u) * 7.0f + fabsf(vv) * 5.0f, 1.0f) * 4.0f) & 1 ? 1.0f : 0.3f; }
        break;
    case VS_TUBE: case VS_BULB:
        *glow = 1.0f;
        break;
    default:
        break;
    }
    if (x->flags & VPF_EMISSIVE) *glow = maxf(*glow, 1.0f);
    for (int i = 0; i < 3; ++i) col[i] = base[i] * k;
}

typedef struct View { float ex[3], ey[3], ez[3]; float scale, ox, oy; int cx0, cy0, cw, ch; } View;

static void project(const View *V, const float p[3], float *sx, float *sy, float *sz) {
    float x = p[0] * V->ex[0] + p[1] * V->ex[1] + p[2] * V->ex[2];
    float y = p[0] * V->ey[0] + p[1] * V->ey[1] + p[2] * V->ey[2];
    float z = p[0] * V->ez[0] + p[1] * V->ez[1] + p[2] * V->ez[2];
    *sx = (float)V->cx0 + V->ox + x * V->scale;
    *sy = (float)V->cy0 + V->oy - y * V->scale;
    *sz = z;
}

static void raster(Image *im, const View *V, const float P[3][3], const float C[3][3]) {
    float sx[3], sy[3], sz[3];
    for (int k = 0; k < 3; ++k) project(V, P[k], &sx[k], &sy[k], &sz[k]);
    int x0 = (int)floorf(minf(sx[0], minf(sx[1], sx[2]))), x1 = (int)ceilf(maxf(sx[0], maxf(sx[1], sx[2])));
    int y0 = (int)floorf(minf(sy[0], minf(sy[1], sy[2]))), y1 = (int)ceilf(maxf(sy[0], maxf(sy[1], sy[2])));
    x0 = x0 < V->cx0 ? V->cx0 : x0; y0 = y0 < V->cy0 ? V->cy0 : y0;
    x1 = x1 > V->cx0 + V->cw - 1 ? V->cx0 + V->cw - 1 : x1; y1 = y1 > V->cy0 + V->ch - 1 ? V->cy0 + V->ch - 1 : y1;
    float d = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
    if (fabsf(d) < 1e-6f) return;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w1 = ((px - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (py - sy[0])) / d;
            float w2 = ((sx[1] - sx[0]) * (py - sy[0]) - (px - sx[0]) * (sy[1] - sy[0])) / d;
            float w0 = 1.0f - w1 - w2;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
            int i = y * im->w + x;
            if (z <= im->z[i]) continue;
            im->z[i] = z;
            for (int k = 0; k < 3; ++k) im->p[i * 3 + k] = (unsigned char)(lin_to_srgb(w0 * C[0][k] + w1 * C[1][k] + w2 * C[2][k]) * 255.0f + 0.5f);
        }
}

/* Renders a vendor into cell (cx0, cy0, cw, ch) of the image, seen from front right and above. */
static void render(Image *im, const Vendor *v, int cx0, int cy0, int cw, int ch, float az) {
    View V;
    float el = 0.42f;
    float fwd[3] = { -sinf(az) * cosf(el), -sinf(el), -cosf(az) * cosf(el) };   /* towards the vendor */
    V.ez[0] = -fwd[0]; V.ez[1] = -fwd[1]; V.ez[2] = -fwd[2];                    /* towards the viewer: nearer is larger */
    V.ex[0] = cosf(az); V.ex[1] = 0.0f; V.ex[2] = -sinf(az);
    V.ey[0] = V.ez[1] * V.ex[2] - V.ez[2] * V.ex[1]; V.ey[1] = V.ez[2] * V.ex[0] - V.ez[0] * V.ex[2]; V.ey[2] = V.ez[0] * V.ex[1] - V.ez[1] * V.ex[0];
    V.cx0 = cx0; V.cy0 = cy0; V.cw = cw; V.ch = ch;
    /* fit the site and the vendor's height */
    float hw = 0.5f * v->site.w, d = v->site.d, top = maxf(v->top, 2.5f);
    float mnx = 1e9f, mxx = -1e9f, mny = 1e9f, mxy = -1e9f;
    for (int c = 0; c < 8; ++c) {
        float p[3] = { (c & 1) ? hw : -hw, (c & 2) ? top : 0.0f, (c & 4) ? d : 0.0f };
        float x = p[0] * V.ex[0] + p[1] * V.ex[1] + p[2] * V.ex[2], y = p[0] * V.ey[0] + p[1] * V.ey[1] + p[2] * V.ey[2];
        mnx = minf(mnx, x); mxx = maxf(mxx, x); mny = minf(mny, y); mxy = maxf(mxy, y);
    }
    V.scale = 0.9f * minf((float)cw / (mxx - mnx), (float)ch / (mxy - mny));
    V.ox = 0.5f * (float)cw - 0.5f * (mnx + mxx) * V.scale;
    V.oy = 0.5f * (float)ch + 0.5f * (mny + mxy) * V.scale;
    /* the site's ground, and the walk beyond its front */
    {
        float G[2][4][3] = { { { -hw, 0, 0 }, { hw, 0, 0 }, { hw, 0, d }, { -hw, 0, d } }, { { -hw, 0, d }, { hw, 0, d }, { hw, 0, d + 1.0f }, { -hw, 0, d + 1.0f } } };
        for (int g = 0; g < 2; ++g) {
            float c = g ? 0.06f : 0.12f, C[3][3] = { { c, c, c * 1.1f }, { c, c, c * 1.1f }, { c, c, c * 1.1f } };
            float T0[3][3], T1[3][3];
            memcpy(T0[0], G[g][0], 12); memcpy(T0[1], G[g][1], 12); memcpy(T0[2], G[g][2], 12);
            memcpy(T1[0], G[g][0], 12); memcpy(T1[1], G[g][2], 12); memcpy(T1[2], G[g][3], 12);
            for (int k = 0; k < 3; ++k) { T0[k][1] -= 0.01f; T1[k][1] -= 0.01f; }
            raster(im, &V, T0, C); raster(im, &V, T1, C);
        }
    }
    VendorMesh m = { 0 };
    vendor_mesh(v, -1, &m);
    float L[3] = { 0.45f, 0.8f, 0.4f };
    float ll = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    for (int k = 0; k < 3; ++k) L[k] /= ll;
    for (uint32_t t = 0; t + 2 < m.count; t += 3) {
        float P[3][3], C[3][3];
        for (int k = 0; k < 3; ++k) {
            const VendorVertex *x = &m.v[t + k];
            memcpy(P[k], x->p, 12);
            float col[3], glow;
            shade_vertex(v, x, x->uv[0], x->uv[1], col, &glow);
            float ndl = fabsf(x->n[0] * L[0] + x->n[1] * L[1] + x->n[2] * L[2]);
            float lit = 0.3f + 0.7f * ndl;
            for (int i = 0; i < 3; ++i) C[k][i] = glow > 0.0f ? col[i] * (0.6f + 1.6f * glow) + 0.05f * glow : col[i] * lit;
        }
        raster(im, &V, P, C);
    }
    vendor_mesh_free(&m);
}

static Image image_new(int w, int h) {
    Image im = { w, h, (unsigned char *)malloc((size_t)w * h * 3), (float *)malloc(sizeof(float) * (size_t)w * h) };
    for (int i = 0; i < w * h; ++i) { im.p[i * 3] = 18; im.p[i * 3 + 1] = 18; im.p[i * 3 + 2] = 24; im.z[i] = -1e30f; }
    return im;
}

/* ------------------------------------------------------------------- main --- */

static const char *ROLE[VR_COUNT] = { "shell", "support", "roof", "counter", "flap", "strut", "awning", "cloth", "deck", "step",
    "flue", "hood", "sign", "neon", "rail", "wheel", "handle", "bed", "cabinet", "table", "tier", "lamp", "room", "stool", "prop",
    "heap", "hang", "decor", "signature", "gas" };
static const char *SHAPE[7] = { "box", "beam", "cyl", "quad", "vault", "cone", "blob" };
static const char *EDGE[5] = { "supports", "mounted_on", "attached_to", "open_into", "seats_at" };

/* vendor_lab dump <archetype> <seed> [affluence] [tier]: one vendor's log, parts, graph and items. */
static int dump(int argc, char **argv) {
    int arch = argc > 2 ? atoi(argv[2]) : 0;
    uint32_t seed = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 0) : 1u;
    float aff = argc > 4 ? (float)atof(argv[4]) : 0.15f;
    int tier = argc > 5 ? atoi(argv[5]) : VTIER_STANDARD;
    Vendor *v = (Vendor *)malloc(sizeof(Vendor));
    char *buf = (char *)malloc(1 << 16);
    BLog log = { buf, 1 << 16, 0 };
    buf[0] = 0;
    VendorSite site = site_for(arch, seed, aff);
    vendor_generate(&site, tier, NULL, 0, v, &log);
    printf("%s\nsite %.2f x %.2f, ceiling %.1f, host wall %d; box %.2f %.2f %.2f %.2f, top %.2f, front %.2f\n", buf, site.w, site.d, site.ceiling,
           site.hostWall, v->box[0], v->box[1], v->box[2], v->box[3], v->top, v->front);
    for (int i = 0; i < v->npart; ++i) {
        const VPart *p = &v->part[i];
        printf("  %3d %-9s %-5s a(%.2f %.2f %.2f) b(%.2f %.2f %.2f) r %.2f\n", i, ROLE[p->role], SHAPE[p->shape], p->a[0], p->a[1], p->a[2], p->b[0], p->b[1], p->b[2], p->r);
    }
    for (int e = 0; e < v->nedge; ++e) printf("  edge %d %s %d\n", v->edge[e].from, EDGE[v->edge[e].kind], v->edge[e].to);
    for (int i = 0; i < v->nitem; ++i) {
        const VItem *it = &v->item[i];
        printf("  item %-16s %-9s at (%.2f %.2f %.2f) size (%.2f %.2f %.2f) facing (%.2f %.2f)\n", it->sig ? vendor_signature_name(it->kind) : vendor_item_name(it->kind),
               ROLE[it->role], it->p[0], it->p[1], it->p[2], it->s[0], it->s[1], it->s[2], it->fx, it->fz);
    }
    for (int i = 0; i < v->nsign; ++i) printf("  sign kind %d at (%.2f %.2f %.2f) %.2f x %.2f\n", v->sign[i].kind, v->sign[i].c[0], v->sign[i].c[1], v->sign[i].c[2], v->sign[i].w, v->sign[i].h);
    for (int i = 0; i < v->nneon; ++i) printf("  neon %s at (%.2f %.2f %.2f) %.2f\n", vendor_motif_name(v->neon[i].motif), v->neon[i].c[0], v->neon[i].c[1], v->neon[i].c[2], v->neon[i].size);
    free(v); free(buf);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "dump")) return dump(argc, argv);
    int seeds = argc > 1 ? atoi(argv[1]) : 60;
    const char *out = argc > 2 ? argv[2] : NULL;
    uint32_t verbose = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 0) : 0;
    if (seeds < 1) seeds = 1;
    Vendor *a = (Vendor *)malloc(sizeof(Vendor)), *b = (Vendor *)malloc(sizeof(Vendor));
    char *logbuf = (char *)malloc(1 << 16);
    if (!a || !b || !logbuf) return 1;
    printf("vendor lab: %d seeds per archetype, tier and class (Vendor is %u bytes)\n", seeds, (unsigned)sizeof(Vendor));

    /* 1. every archetype, tier and class: valid, deterministic, within budget */
    static const float AFF[VC_COUNT] = { 0.15f, 0.5f, 0.85f };
    int total = 0, okCount = 0, levels[3] = { 0 }, rerolls = 0, forced = 0, reductions = 0;
    int *tris = (int *)malloc(sizeof(int) * (size_t)seeds * VC_COUNT);
    clock_t t0 = clock();
    printf("\n%-18s %-8s %5s %5s  %6s %6s %6s  %5s %5s %5s %5s %5s\n", "archetype", "tier", "pass", "level", "median", "max", "shell", "props", "heaps", "hang", "decor", "neon");
    for (int arch = 0; arch < VA_COUNT; ++arch)
        for (int tier = 0; tier < VTIER_COUNT; ++tier) {
            int pass = 0, n = 0, lv = 0, shell = 0, props = 0, heaps = 0, hang = 0, decor = 0, neon = 0;
            for (int c = 0; c < VC_COUNT; ++c)
                for (int s = 0; s < seeds; ++s) {
                    uint32_t seed = hash3i(0xBEEFu, arch, c, s);
                    VendorSite site = site_for(arch, seed, AFF[c]);
                    BLog log = { logbuf, 1 << 16, 0 };
                    logbuf[0] = 0;
                    int ok = vendor_generate(&site, tier, NULL, 0, a, &log);
                    vendor_generate(&site, tier, NULL, 0, b, NULL);
                    total++;
                    CHECK(!memcmp(a, b, sizeof(Vendor)), "%s seed %08x: not deterministic", vendor_archetype_name(arch), seed);
                    if (s < 3 && ok) {
                        VendorMesh ma = { 0 }, mb = { 0 };
                        vendor_mesh(a, -1, &ma); vendor_mesh(b, -1, &mb);
                        CHECK(ma.count == mb.count && !memcmp(ma.v, mb.v, sizeof(VendorVertex) * ma.count), "%s seed %08x: mesh not deterministic", vendor_archetype_name(arch), seed);
                        CHECK((int)ma.count == 3 * a->st.tris, "%s seed %08x: counted %d triangles, meshed %u", vendor_archetype_name(arch), seed, a->st.tris, ma.count / 3);
                        vendor_mesh_free(&ma); vendor_mesh_free(&mb);
                    }
                    if (!ok) {
                        tally(logbuf);
                        if (a->npart == 0) { CHECK(0, "%s seed %08x: nothing built on a %.1f x %.1f site", vendor_archetype_name(arch), seed, site.w, site.d); }
                        else CHECK(0, "%s %s seed %08x (%s): invalid at every level", vendor_archetype_name(arch), vendor_tier_name(tier), seed, vendor_class_name(c));
                        if (g_fail <= 3) printf("%s\n", logbuf);
                        continue;
                    }
                    CHECK(a->archetype == arch, "%s seed %08x: built a %s", vendor_archetype_name(arch), seed, vendor_archetype_name(a->archetype));
                    pass++; okCount++;
                    levels[a->st.level]++; lv += a->st.level;
                    rerolls += a->st.rerolls; forced += a->st.forced; reductions += a->st.reductions;
                    if (tier == VTIER_STANDARD) tris[n] = a->st.tris;
                    n += tier == VTIER_STANDARD;
                    shell += a->st.shellTris; props += a->st.props; heaps += a->st.heaps; hang += a->st.hangings; decor += a->st.decor; neon += a->st.neon;
                    if (a->st.level) tally(logbuf);
                }
            int cnt = pass ? pass : 1;
            if (tier == VTIER_STANDARD && n) {
                qsort(tris, (size_t)n, sizeof(int), cmp_int);
                printf("%-18s %-8s %4d%% %5.2f  %6d %6d %6d  %5.1f %5.1f %5.1f %5.1f %5.1f\n", vendor_archetype_name(arch), vendor_tier_name(tier), 100 * pass / (VC_COUNT * seeds),
                       (float)lv / (float)cnt, tris[n / 2], tris[n - 1], shell / cnt, (float)props / cnt, (float)heaps / cnt, (float)hang / cnt, (float)decor / cnt, (float)neon / cnt);
            } else {
                printf("%-18s %-8s %4d%% %5.2f  %6s %6s %6d  %5.1f %5.1f %5.1f %5.1f %5.1f\n", vendor_archetype_name(arch), vendor_tier_name(tier), 100 * pass / (VC_COUNT * seeds),
                       (float)lv / (float)cnt, "", "", shell / cnt, (float)props / cnt, (float)heaps / cnt, (float)hang / cnt, (float)decor / cnt, (float)neon / cnt);
            }
        }
    double ms = 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC / (double)(2 * total);
    printf("\n%d of %d valid; levels %d / %d / %d; %d dressing rerolls, %d forced, %d reductions; %.3f ms a vendor\n",
           okCount, total, levels[0], levels[1], levels[2], rerolls, forced, reductions, ms);
    if (g_nreason) {
        printf("rejections and retries by rule:\n");
        for (int i = 0; i < g_nreason; ++i) printf("  %5d  %s\n", g_reason[i].n, g_reason[i].text);
    }

    /* 2. rows of five, one archetype and one trade, sharing their context (section 8) */
    int rows = 0, rowForced = 0;
    for (int arch = 0; arch < VA_COUNT; ++arch)
        for (int s = 0; s < (seeds + 5) / 6; ++s) {
            VendorFinger ctx[5];
            int trade = (int)(hash3i(0x7E57u, arch, s, 0) % VT_COUNT);
            for (int k = 0; k < 5; ++k) {
                VendorSite site = site_for(arch, hash3i(0x7E58u, arch, s, k), 0.2f + 0.6f * hash_float(hash3i(0x7E59u, arch, s, 0)));
                site.trade = (int8_t)trade;
                BLog log = { logbuf, 1 << 16, 0 };
                logbuf[0] = 0;
                if (!vendor_generate(&site, VTIER_STANDARD, ctx, k, a, &log)) {
                    CHECK(0, "row %s/%s %d: vendor %d invalid", vendor_archetype_name(arch), vendor_trade_name(trade), s, k);
                    if (g_fail <= 2) printf("%s\n", logbuf);
                    break;
                }
                for (int j = 0; j < k; ++j) {
                    int sh = vendor_finger_shared(&a->finger, &ctx[j]);
                    CHECK(sh <= V_FINGER_SHARE, "row %s %d: vendor %d shares %d axes with %d", vendor_archetype_name(arch), s, k, sh, j);
                    CHECK(a->finger.signature == VSIG_COUNT || a->finger.signature != ctx[j].signature, "row %s %d: vendor %d repeats %d's signature", vendor_archetype_name(arch), s, k, j);
                }
                rowForced += a->st.forced;
                ctx[k] = a->finger;
            }
            rows++;
        }
    printf("\n%d rows of five with one archetype and one trade: none too alike (%d forced changes)\n", rows, rowForced);

    /* 3. the automatic mix over random sites, by affluence class */
    int mixA[VC_COUNT][VA_COUNT] = { { 0 } }, mixT[VC_COUNT][VT_COUNT] = { { 0 } }, mixN[VC_COUNT] = { 0 }, siteOk[VSITE_COUNT] = { 0 }, siteN[VSITE_COUNT] = { 0 };
    int lunchT[VT_COUNT] = { 0 }, lunchN = 0;
    for (int s = 0; s < seeds * 40; ++s) {
        VendorSite site = site_random(hash2i(0xA770u, s, 0));
        int ok = vendor_generate(&site, VTIER_STANDARD, NULL, 0, a, NULL);
        siteN[site.kind]++;
        if (!ok) continue;
        siteOk[site.kind]++;
        int c = vendor_class(site.affluence);
        mixA[c][a->archetype]++; mixT[c][a->trade]++; mixN[c]++;
        if (site.lunch) { lunchT[a->trade]++; lunchN++; }
    }
    printf("\nautomatic mix over %d random sites (archetype share by class):\n%-10s", seeds * 40, "");
    for (int ar = 0; ar < VA_COUNT; ++ar) printf(" %8.8s", vendor_archetype_name(ar));
    printf("\n");
    for (int c = 0; c < VC_COUNT; ++c) {
        printf("%-10s", vendor_class_name(c));
        for (int ar = 0; ar < VA_COUNT; ++ar) printf(" %7.0f%%", mixN[c] ? 100.0f * (float)mixA[c][ar] / (float)mixN[c] : 0.0f);
        printf("\n");
    }
    for (int c = 0; c < VC_COUNT; ++c) {
        printf("top trades, %s:", vendor_class_name(c));
        for (int k = 0; k < 5; ++k) {
            int best = 0;
            for (int t = 1; t < VT_COUNT; ++t) if (mixT[c][t] > mixT[c][best]) best = t;
            if (!mixT[c][best]) break;
            printf(" %s %.0f%%%s", vendor_trade_name(best), 100.0f * (float)mixT[c][best] / (float)mixN[c], k < 4 ? "," : "");
            mixT[c][best] = -mixT[c][best];
        }
        printf("\n");
    }
    printf("lunch plazas:");
    for (int k = 0; k < 4; ++k) {
        int best = 0;
        for (int t = 1; t < VT_COUNT; ++t) if (lunchT[t] > lunchT[best]) best = t;
        if (!lunchT[best]) break;
        printf(" %s %.0f%%%s", vendor_trade_name(best), 100.0f * (float)lunchT[best] / (float)lunchN, k < 3 ? "," : "");
        lunchT[best] = 0;
    }
    printf("\nsites that took a vendor:");
    for (int k = 0; k < VSITE_COUNT; ++k) printf(" %s %d/%d", vendor_site_name(k), siteOk[k], siteN[k]);
    printf("\n");

    if (verbose) {
        VendorSite site = site_random(verbose);
        BLog log = { logbuf, 1 << 16, 0 };
        logbuf[0] = 0;
        vendor_generate(&site, VTIER_STANDARD, NULL, 0, a, &log);
        printf("\n%s", logbuf);
    }

    /* 4. contact sheets */
    if (out) {
        const int CW = 300, CH = 250;
        Image im = image_new(CW * 6, CH * VA_COUNT);
        printf("\ncontact sheet: rows are archetypes, columns poor, poor, middling, middling, wealthy, wealthy\n");
        for (int arch = 0; arch < VA_COUNT; ++arch)
            for (int col = 0; col < 6; ++col) {
                int c = col / 2;
                uint32_t seed = hash3i(0x5EE7u, arch, col, 0);
                VendorSite site = site_for(arch, seed, AFF[c]);
                site.kind = arch == VA_LEANTO ? VSITE_LANE : site.kind;
                site.ceiling = 0.0f;
                if (!vendor_generate(&site, VTIER_STANDARD, NULL, 0, a, NULL)) continue;
                render(&im, a, col * CW, arch * CH, CW, CH, 0.6f);
                printf("  %-18s %-9s %-20s %4d tris\n", vendor_archetype_name(arch), vendor_class_name(c), vendor_trade_name(a->trade), a->st.tris);
            }
        char path[512];
        snprintf(path, sizeof(path), "%s/vendors.png", out);
        stbi_write_png(path, im.w, im.h, 3, im.p, im.w * 3);
        printf("wrote %s\n", path);
        free(im.p); free(im.z);
        /* a row of five noodle bars sharing their context, and a mixed street */
        Image row = image_new(CW * 5, CH * 2);
        VendorFinger ctx[5];
        for (int line = 0; line < 2; ++line)
            for (int k = 0; k < 5; ++k) {
                VendorSite site = line == 0 ? site_for(VA_CANOPY, hash2i(0xF0A1u, k, 0), 0.3f) : site_random(hash2i(0xF0A2u, k, 7));
                site.ceiling = 0.0f;
                if (line == 0) site.trade = VT_NOODLE;
                if (!vendor_generate(&site, VTIER_STANDARD, ctx, line == 0 ? k : 0, a, NULL)) continue;
                if (line == 0) ctx[k] = a->finger;
                render(&row, a, k * CW, line * CH, CW, CH, 0.6f);
            }
        snprintf(path, sizeof(path), "%s/vendor_rows.png", out);
        stbi_write_png(path, row.w, row.h, 3, row.p, row.w * 3);
        printf("wrote %s\n", path);
        free(row.p); free(row.z);
    }

    free(tris); free(a); free(b); free(logbuf);
    printf("\n%s\n", g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
