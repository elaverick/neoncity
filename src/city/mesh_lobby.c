#include "city_internal.h"

/* ===========================================================================
 * Interiors: a planned office lobby (src/gen/interior.c) realised in the
 * building's frame. The planning frame is the building frame turned in
 * quarter turns, so its rectangles are boxes here too. Floors and ceilings
 * follow the true footprint; partitions come from the boundary table;
 * furniture is rough boxes for now.
 * ========================================================================= */

void interiors_plan(const Building *b, const BGraph *g, Interiors *in) {
    memset(in, 0, sizeof(*in));
    if (g->archetype == ARCH_ARCOLOGY) {
        ALayout *A = (ALayout *)malloc(sizeof(ALayout));
        if (!A) fatal("out of memory planning an arcology");
        if (arcology_plan(b, g, 0, A, NULL) && A->ok) in->arc = A;
        else free(A);
        return;
    }
    int secs[2], ns = interior_sections(g, secs, 2);
    for (int k = 0; k < ns; ++k) {
        ILayout *L = (ILayout *)malloc(sizeof(ILayout));
        if (!L) fatal("out of memory planning an interior");
        if (interior_plan(b, g, secs[k], 0, L, NULL)) in->lay[in->n++] = L;
        else free(L);
    }
}

void interiors_free(Interiors *in) {
    for (int k = 0; k < in->n; ++k) free(in->lay[k]);
    free(in->arc);
    in->n = 0;
    in->arc = NULL;
}

const ILayout *interior_of(const Interiors *in, int si) {
    for (int k = 0; in && k < in->n; ++k) if (in->lay[k]->section == si) return in->lay[k];
    return NULL;
}

/* What stands behind section si's envelope, if anything was planned there. */
int behind_of(const Interiors *in, int si, Behind *bh) {
    bh->L = interior_of(in, si); bh->A = NULL; bh->si = si;
    if (!bh->L && in && in->arc && in->arc->tier[0].section == si) bh->A = in->arc;
    return bh->L || bh->A;
}

int city_casts_shadow(const Vertex *v) {
    const uint8_t *p = v->params;
    switch (p[0]) {
    case MAT_INTERIOR:
        return p[1] != IS_GLASS;
    case MAT_ARCOLOGY:
        if (p[1] == ARS_RAIL) return p[2] != 0 && p[2] != 3;   /* glass balustrades */
        return p[1] != ARS_SHAFT && p[1] != ARS_SEARCH && p[1] != ARS_STEAM && p[1] != ARS_PUDDLE;
    case MAT_LITTER:   /* the rigid things: soaked paper slumps, dry paper and bags stir (city.vert: litter) */
        return p[1] == LK_CAN || p[1] == LK_BOTTLE || p[1] == LK_CUP || p[1] == LK_BOX || p[1] == LK_BINBAG;
    default:
        return 1;
    }
}

/* params: y surface, z variety (a region or fixture type), w ceiling height above the floor (half metres). */
static Mat imat(const BGraph *g, const ILayout *L, int surface, int sub) {
    return mat(MAT_INTERIOR, g->seed, (uint8_t)surface, (uint8_t)sub, q_half(L->ceilY - L->floorY));
}

/* Floors and ceilings carry the building frame's angle (aux), so the shader
 * can follow their uv (building metres) in world space. */
Mat iframe(const Mesh *m, Mat mt) {
    float a = m->xf ? atan2f(m->xs, m->xc) : 0.0f;
    mt.aux = (uint32_t)((a + PI_F) / (2.0f * PI_F) * 65535.0f + 0.5f) & 0xFFFFu;
    return mt;
}

/* Frame rectangle -> building-frame box. */
static void ibox(Mesh *m, const ILayout *L, float x0, float z0, float x1, float z1, float y0, float y1, int faces, Mat mt) {
    float ax, az, bx, bz;
    interior_to_local(L, x0, z0, &ax, &az);
    interior_to_local(L, x1, z1, &bx, &bz);
    if (y1 - y0 < 1e-3f || fabsf(ax - bx) < 1e-3f || fabsf(az - bz) < 1e-3f) return;
    emit_box(m, v3(minf(ax, bx), y0, minf(az, bz)), v3(maxf(ax, bx), y1, maxf(az, bz)), faces, mt, UV_FACADE);
}

static void iflat(Mesh *m, const ILayout *L, float x0, float z0, float x1, float z1, float y, int down, Mat mt) {
    float ax, az, bx, bz;
    interior_to_local(L, x0, z0, &ax, &az);
    interior_to_local(L, x1, z1, &bx, &bz);
    float lx0 = minf(ax, bx), lx1 = maxf(ax, bx), lz0 = minf(az, bz), lz1 = maxf(az, bz);
    float uv[4][2] = { { lx0, lz0 }, { lx1, lz0 }, { lx1, lz1 }, { lx0, lz1 } };
    emit_quad(m, v3(lx0, y, lz0), v3(lx1, y, lz0), v3(lx1, y, lz1), v3(lx0, y, lz1), v3(0, down ? -1.0f : 1.0f, 0), uv, mt);
}

/* Vertical quad on the frame line (x0,z0)-(x1,z1), facing frame direction (nx, nz). */
static void iwall(Mesh *m, const ILayout *L, float x0, float z0, float x1, float z1, float y0, float y1, float nx, float nz, Mat mt) {
    float ax, az, bx, bz, lnx, lnz;
    interior_to_local(L, x0, z0, &ax, &az);
    interior_to_local(L, x1, z1, &bx, &bz);
    interior_to_local(L, nx, nz, &lnx, &lnz);
    float len = sqrtf((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
    float uv[4][2] = { { 0, y0 }, { len, y0 }, { len, y1 }, { 0, y1 } };
    emit_quad(m, v3(ax, y0, az), v3(bx, y0, bz), v3(bx, y1, bz), v3(ax, y1, az), v3(lnx, 0, lnz), uv, mt);
}

/* A sloped panel on the frame line (x0,z0)-(x1,z1): its foot rises from ya to yb, h tall. */
static void iwall_slope(Mesh *m, const ILayout *L, float x0, float z0, float x1, float z1, float ya, float yb, float h, float nx, float nz, Mat mt) {
    float ax, az, bx, bz, lnx, lnz;
    interior_to_local(L, x0, z0, &ax, &az);
    interior_to_local(L, x1, z1, &bx, &bz);
    interior_to_local(L, nx, nz, &lnx, &lnz);
    float len = sqrtf((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
    float uv[4][2] = { { 0, ya }, { len, yb }, { len, yb + h }, { 0, ya + h } };
    emit_quad(m, v3(ax, ya, az), v3(bx, yb, bz), v3(bx, yb + h, bz), v3(ax, ya + h, az), v3(lnx, 0, lnz), uv, mt);
}

/* Greedy rectangles covering the cells of region r (whole cells only, or edge cells too). */
int region_rects(const ILayout *L, int r, int wholeOnly, int16_t (*out)[4], int max) {
    int N = L->nx * L->nz, n = 0;
    uint8_t *used = (uint8_t *)calloc((size_t)N, 1);
    if (!used) fatal("out of memory realising an interior");
#define WANT(i, j) (L->occ[(j) * L->nx + (i)] == IO_USABLE && L->own[(j) * L->nx + (i)] == r && \
                    !(wholeOnly && (L->flag[(j) * L->nx + (i)] & IF_EDGE)) && !used[(j) * L->nx + (i)])
    for (int j = 0; j < L->nz && n < max; ++j)
        for (int i = 0; i < L->nx && n < max; ++i) {
            if (!WANT(i, j)) continue;
            int i1 = i + 1, j1 = j + 1;
            while (i1 < L->nx && WANT(i1, j)) i1++;
            for (;;) {
                if (j1 >= L->nz) break;
                int ok = 1;
                for (int a = i; a < i1 && ok; ++a) ok = WANT(a, j1);
                if (!ok) break;
                j1++;
            }
            for (int b = j; b < j1; ++b) for (int a = i; a < i1; ++a) used[b * L->nx + a] = 1;
            out[n][0] = (int16_t)i; out[n][1] = (int16_t)j; out[n][2] = (int16_t)i1; out[n][3] = (int16_t)j1;
            n++;
        }
#undef WANT
    free(used);
    return n;
}

/* Sutherland-Hodgman: keep the part of a polygon where a*x + b*z + c >= 0. */
int clip_half(const float (*in)[2], int n, float a, float b, float c, float (*out)[2]) {
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const float *p = in[i], *q = in[(i + 1) % n];
        float dp = a * p[0] + b * p[1] + c, dq = a * q[0] + b * q[1] + c;
        if (dp >= 0.0f) { out[m][0] = p[0]; out[m][1] = p[1]; m++; }
        if ((dp >= 0.0f) != (dq >= 0.0f)) {
            float t = dp / (dp - dq);
            out[m][0] = p[0] + (q[0] - p[0]) * t; out[m][1] = p[1] + (q[1] - p[1]) * t; m++;
        }
        if (m >= 2 * BG_MAX_PV - 2) break;
    }
    return m;
}

/* A footprint outline minus a frame rectangle (the atrium void), as up to
 * four caps: the void may touch or cut the outline, so it is never a hole. */
void emit_cap_minus(Mesh *m, const ILayout *L, const BLoop *outer, float vx0, float vz0, float vx1, float vz1, float y, int down, Mat mt) {
    float poly[2 * BG_MAX_PV][2], a[2 * BG_MAX_PV][2], b[2 * BG_MAX_PV][2];
    int n = outer->n;
    for (int i = 0; i < n; ++i) interior_to_frame(L, outer->x[i], outer->z[i], &poly[i][0], &poly[i][1]);
    for (int piece = 0; piece < 4; ++piece) {
        int k;
        if (piece == 0)      k = clip_half((const float (*)[2])poly, n, -1, 0, vx0, a);   /* x <= vx0 */
        else if (piece == 1) k = clip_half((const float (*)[2])poly, n, 1, 0, -vx1, a);   /* x >= vx1 */
        else {
            k = clip_half((const float (*)[2])poly, n, 1, 0, -vx0, b);
            k = clip_half((const float (*)[2])b, k, -1, 0, vx1, a);
            memcpy(b, a, sizeof(float) * 2 * (size_t)k);
            k = piece == 2 ? clip_half((const float (*)[2])b, k, 0, -1, vz0, a) : clip_half((const float (*)[2])b, k, 0, 1, -vz1, a);
        }
        if (k < 3 || k > BG_MAX_PV) continue;
        BFoot f;
        memset(&f, 0, sizeof(f));
        f.outer.n = k;
        for (int i = 0; i < k; ++i) interior_to_local(L, a[i][0], a[i][1], &f.outer.x[i], &f.outer.z[i]);
        if (fabsf(bloop_area(&f.outer)) < 0.01f) continue;
        emit_cap(m, &f, y, down, mt);
    }
}

/* Runs along one side of the void where the floor beyond it is inside (so a
 * slab edge and a balustrade belong there). side: 0 front, 1 right, 2 rear, 3 left. */
static int void_side_runs(const ILayout *L, int side, float (*run)[2], int max) {
    const IAtrium *A = &L->atrium;
    int alongX = side == 0 || side == 2, a0 = alongX ? A->x0 : A->z0, a1 = alongX ? A->x1 : A->z1, n = 0, start = -1;
    for (int k = a0; k <= a1; ++k) {
        int ok = 0;
        if (k < a1) {
            int i = alongX ? k : (side == 1 ? A->x1 : A->x0 - 1), j = alongX ? (side == 2 ? A->z1 : A->z0 - 1) : k;
            ok = i >= 0 && j >= 0 && i < L->nx && j < L->nz && L->occ[j * L->nx + i] == IO_USABLE && !(L->flag[j * L->nx + i] & IF_EDGE);
        }
        if (ok && start < 0) start = k;
        if (!ok && start >= 0) {
            if (n < max) { run[n][0] = (float)start * IN_CELL; run[n][1] = (float)k * IN_CELL; n++; }
            start = -1;
        }
    }
    return n;
}

/* Slab edges round the void (y0..y1) and, if rail > 0, a glass balustrade on
 * them, with a gap where the feature stair lands. */
static void emit_void_edges(Mesh *m, const BGraph *g, const ILayout *L, float y0, float y1, float rail, const IFixture *stair) {
    const IAtrium *A = &L->atrium;
    float vx0 = L->gx0 + A->x0 * IN_CELL, vx1 = L->gx0 + A->x1 * IN_CELL, vz0 = L->gz0 + A->z0 * IN_CELL, vz1 = L->gz0 + A->z1 * IN_CELL;
    Mat edge = imat(g, L, IS_WALL, 1), glass = imat(g, L, IS_GLASS, 0), metal = imat(g, L, IS_METAL, 0);
    for (int side = 0; side < 4; ++side) {
        float run[32][2];
        int nr = void_side_runs(L, side, run, 32);
        int alongX = side == 0 || side == 2;
        float line = side == 0 ? vz0 : side == 1 ? vx1 : side == 2 ? vz1 : vx0;
        float nx = side == 1 ? -1.0f : side == 3 ? 1.0f : 0.0f, nz = side == 0 ? 1.0f : side == 2 ? -1.0f : 0.0f;
        for (int r = 0; r < nr; ++r) {
            float a0 = (alongX ? L->gx0 : L->gz0) + run[r][0], a1 = (alongX ? L->gx0 : L->gz0) + run[r][1];
            if (alongX) iwall(m, L, a0, line + nz * 0.01f, a1, line + nz * 0.01f, y0, y1, nx, nz, edge);
            else        iwall(m, L, line + nx * 0.01f, a0, line + nx * 0.01f, a1, y0, y1, nx, nz, edge);
            if (rail <= 0.0f) continue;
            float gap0 = a1, gap1 = a1;
            if (stair && side == 2) { gap0 = clampf(stair->x0 - 0.05f, a0, a1); gap1 = clampf(stair->x1 + 0.05f, a0, a1); }
            float seg[2][2] = { { a0, gap0 }, { gap1, a1 } };
            for (int sgi = 0; sgi < 2; ++sgi) {
                float s0 = seg[sgi][0], s1 = seg[sgi][1];
                if (s1 - s0 < 0.1f) continue;
                if (alongX) {
                    float zc = line - nz * 0.06f;
                    ibox(m, L, s0, zc - 0.015f, s1, zc + 0.015f, y1, y1 + rail, F_NOBOT, glass);
                    ibox(m, L, s0, zc - 0.03f, s1, zc + 0.03f, y1 + rail, y1 + rail + 0.05f, F_NOBOT | F_BOT, metal);
                } else {
                    float xc = line - nx * 0.06f;
                    ibox(m, L, xc - 0.015f, s0, xc + 0.015f, s1, y1, y1 + rail, F_NOBOT, glass);
                    ibox(m, L, xc - 0.03f, s0, xc + 0.03f, s1, y1 + rail, y1 + rail + 0.05f, F_NOBOT | F_BOT, metal);
                }
            }
        }
    }
}

/* The inside of the envelope: glazing with mullions on the lobby's 1.5 m
 * module (the facade shader's), a head panel above it, and a lining where a
 * room or core stands against it (the facade outside is opaque there). */
static void emit_envelope_inside(Mesh *m, const BGraph *g, const ILayout *L, const BSection *s, int si) {
    const BLoop *loops[2] = { &s->foot.outer, &s->foot.hole };
    Mat lining = imat(g, L, IS_LINING, 0), head = imat(g, L, IS_WALL, 0), metal = imat(g, L, IS_METAL, 0), glass = imat(g, L, IS_GLASS, 1);
    float y0 = L->floorY, gt = L->glassTop, yc = L->ceilY;
    float uvq[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    for (int li = 0; li < 2; ++li) {
        const BLoop *l = loops[li];
        for (int k = 0; k < l->n; ++k) {
            EdgeInfo e = edge_of(l, k);
            if (e.len < 0.3f) continue;
            float du0 = 1e9f, du1 = -1e9f, dTop = 0.0f;
            for (int d = 0; d < g->ndoor && li == 0; ++d)
                if (g->door[d].section == si && g->door[d].face == k && g->door[d].type == DOOR_PRIMARY) {
                    du0 = g->door[d].u - g->door[d].width * 0.5f - 0.15f; du1 = g->door[d].u + g->door[d].width * 0.5f + 0.15f;
                    dTop = y0 + g->door[d].height + 0.15f;
                }
            Xf sv = xf_save(m);
            enter_face(m, l, k);
            float span[64][2];
            uint8_t lined[64];
            Behind bh = { L, NULL, si };
            int ns = envelope_spans(g, &bh, l, li, k, span, lined, 64);
            for (int q = 0; q < ns; ++q) {
                float u0 = span[q][0], u1 = q == ns - 1 ? e.len : span[q][1];
                if (u1 - u0 > 0.01f) {
                    if (lined[q]) {
                        /* faced both ways: through the lobby glass beside it, a glance
                           into the cavity behind the facade meets the lining, not the room */
                        emit_quad(m, v3(u0, y0, -0.05f), v3(u1, y0, -0.05f), v3(u1, yc, -0.05f), v3(u0, yc, -0.05f), v3(0, 0, -1), uvq, lining);
                        emit_quad(m, v3(u0, y0, -0.05f), v3(u1, y0, -0.05f), v3(u1, yc, -0.05f), v3(u0, yc, -0.05f), v3(0, 0, 1), uvq, lining);
                    } else {
                        emit_quad(m, v3(u0, gt, -0.03f), v3(u1, gt, -0.03f), v3(u1, yc, -0.03f), v3(u0, yc, -0.03f), v3(0, 0, -1), uvq, head);
                        /* glass, split round the entrance */
                        float cut0 = clampf(du0, u0, u1), cut1 = clampf(du1, u0, u1);
                        if (cut0 - u0 > 0.01f) emit_quad(m, v3(u0, y0 + 0.08f, -0.015f), v3(cut0, y0 + 0.08f, -0.015f), v3(cut0, gt, -0.015f), v3(u0, gt, -0.015f), v3(0, 0, -1), uvq, glass);
                        if (u1 - cut1 > 0.01f) emit_quad(m, v3(cut1, y0 + 0.08f, -0.015f), v3(u1, y0 + 0.08f, -0.015f), v3(u1, gt, -0.015f), v3(cut1, gt, -0.015f), v3(0, 0, -1), uvq, glass);
                        if (cut1 - cut0 > 0.01f && gt > dTop) emit_quad(m, v3(cut0, dTop, -0.015f), v3(cut1, dTop, -0.015f), v3(cut1, gt, -0.015f), v3(cut0, gt, -0.015f), v3(0, 0, -1), uvq, glass);
                        /* mullions, and one on each edge it shares with a lined stretch */
                        float m0 = q > 0 ? u0 - 0.05f : u0 + 0.05f, m1 = q < ns - 1 ? u1 + 0.05f : u1 - 0.05f;
                        for (float um = ceilf(m0 / 1.5f) * 1.5f; um < m1; um += 1.5f) {
                            if (um > du0 - 0.05f && um < du1 + 0.05f) continue;
                            emit_box(m, v3(um - 0.04f, y0, -0.15f), v3(um + 0.04f, gt, -0.01f), F_SIDES, metal, UV_FACADE);
                        }
                        emit_box(m, v3(u0, gt - 0.06f, -0.12f), v3(u1, gt, -0.01f), F_SIDES | F_BOT, metal, UV_FACADE);
                    }
                }
            }
            xf_restore(m, sv);
        }
    }
}

/* Height of a partition: up to the ceiling, or to the gallery slab's underside. */
static float wall_top(const ILayout *L) {
    return L->atrium.mode == IA_GALLERY && L->atrium.x1 > L->atrium.x0 ? L->atrium.galleryY - 0.35f : L->ceilY;
}

/* A strip of half-thickness t centred on a partition run. */
void run_strip(const IWall *r, float t, float *x0, float *z0, float *x1, float *z1) {
    if (fabsf(r->z1 - r->z0) < 1e-4f) { *x0 = r->x0; *x1 = r->x1; *z0 = r->z0 - t; *z1 = r->z0 + t; }
    else                              { *x0 = r->x0 - t; *x1 = r->x0 + t; *z0 = r->z0; *z1 = r->z1; }
}

/* Is the frame point (x, z) at least `inset` inside the envelope? */
static int inside_by(const ILayout *L, float x, float z, float inset) {
    return bfoot_contains(&L->foot, x, z) && bfoot_boundary_dist(&L->foot, x, z, NULL, NULL) >= inset;
}

/* Stop a partition that runs into the envelope just behind the facade's
 * lining (0.05 m in), so its end is not drawn on the facade's plane. */
static void trim_run(const ILayout *L, IWall *r) {
    float len = sqrtf((r->x1 - r->x0) * (r->x1 - r->x0) + (r->z1 - r->z0) * (r->z1 - r->z0));
    if (len < 0.3f) return;
    float ux = (r->x1 - r->x0) / len, uz = (r->z1 - r->z0) / len;
    for (int s = 0; s < 40 && !inside_by(L, r->x0, r->z0, 0.055f); ++s) { r->x0 += ux * 0.005f; r->z0 += uz * 0.005f; }
    for (int s = 0; s < 40 && !inside_by(L, r->x1, r->z1, 0.055f); ++s) { r->x1 -= ux * 0.005f; r->z1 -= uz * 0.005f; }
}

static void emit_partitions(Mesh *m, const BGraph *g, const ILayout *L) {
    int max = 4096;
    IWall *w = (IWall *)malloc(sizeof(IWall) * (size_t)max);
    if (!w) fatal("out of memory realising an interior");
    int n = interior_walls(L, w, max);
    float y0 = L->floorY, top = wall_top(L);
    Mat wall = imat(g, L, IS_WALL, 0), glass = imat(g, L, IS_GLASS, 0), metal = imat(g, L, IS_METAL, 0), door = imat(g, L, IS_DOOR, 0);
    for (int k = 0; k < n; ++k) {
        IWall rw = w[k];
        trim_run(L, &rw);
        const IWall *r = &rw;
        int alongX = fabsf(r->z1 - r->z0) < 1e-4f;
        float len = alongX ? r->x1 - r->x0 : r->z1 - r->z0, x0, z0, x1, z1;
        if (len < 0.02f || r->door == 2) continue;                    /* gates: the speed gates stand there */
        if (r->door == 1) {
            float dTop = y0 + (r->doorKind == ID_INNER ? 2.6f : 2.15f);
            if (r->type == IB_WALL) {
                run_strip(r, 0.07f, &x0, &z0, &x1, &z1);
                ibox(m, L, x0, z0, x1, z1, dTop, top, F_NOBOT | F_BOT, wall);
            } else if (r->type == IB_GLASS) {
                run_strip(r, 0.015f, &x0, &z0, &x1, &z1);
                ibox(m, L, x0, z0, x1, z1, dTop + 0.08f, top - 0.06f, F_NOBOT, glass);
                run_strip(r, 0.04f, &x0, &z0, &x1, &z1);
                ibox(m, L, x0, z0, x1, z1, dTop, dTop + 0.08f, F_NOBOT | F_BOT, metal);
                ibox(m, L, x0, z0, x1, z1, top - 0.06f, top, F_NOBOT | F_BOT, metal);
            }
            /* an open door leaf, swung into the room */
            if (r->doorKind == ID_ROOM && r->side != 0) {
                float s = (float)r->side;
                if (alongX) ibox(m, L, r->x0, s > 0 ? r->z0 + 0.04f : r->z0 - len + 0.04f, r->x0 + 0.05f, s > 0 ? r->z0 + len : r->z0 - 0.04f, y0, y0 + 2.1f, F_NOBOT, door);
                else        ibox(m, L, s > 0 ? r->x0 + 0.04f : r->x0 - len + 0.04f, r->z0, s > 0 ? r->x0 + len : r->x0 - 0.04f, r->z0 + 0.05f, y0, y0 + 2.1f, F_NOBOT, door);
            }
            continue;
        }
        if (r->type == IB_WALL) {
            run_strip(r, 0.07f, &x0, &z0, &x1, &z1);
            ibox(m, L, x0, z0, x1, z1, y0, top, F_NOBOT, wall);
            continue;
        }
        float h = r->type == IB_BARRIER ? 1.0f : top - y0;
        run_strip(r, 0.015f, &x0, &z0, &x1, &z1);
        ibox(m, L, x0, z0, x1, z1, y0 + 0.06f, y0 + h - 0.06f, F_NOBOT, glass);
        run_strip(r, 0.03f, &x0, &z0, &x1, &z1);
        ibox(m, L, x0, z0, x1, z1, y0 + h - 0.06f, y0 + h, F_NOBOT | F_BOT, metal);
        ibox(m, L, x0, z0, x1, z1, y0, y0 + 0.06f, F_SIDES | F_TOP, metal);
        int np = (int)floorf(len / 1.5f);
        for (int p = 0; p <= np; ++p) {        /* posts every 1.5 m or so */
            float a = (alongX ? r->x0 : r->z0) + (np ? len * (float)p / (float)np : 0.0f);
            if (alongX) ibox(m, L, a - 0.03f, r->z0 - 0.03f, a + 0.03f, r->z0 + 0.03f, y0, y0 + h, F_SIDES, metal);
            else        ibox(m, L, r->x0 - 0.03f, a - 0.03f, r->x0 + 0.03f, a + 0.03f, y0, y0 + h, F_SIDES, metal);
        }
    }
    free(w);
}

/* A panel on one face of a lift or stair door's grid line, on the hall side. */
static void door_panel(Mesh *m, const ILayout *L, const IDoor *dr, float sg, float a0, float a1, float d0, float d1, float y0, float y1, Mat mt) {
    float lm = (dr->axis == 0 ? L->gz0 : L->gx0) + dr->line * IN_CELL;
    if (dr->axis == 0) ibox(m, L, a0, lm + sg * d0, a1, lm + sg * d1, y0, y1, F_NOBOT, mt);
    else               ibox(m, L, lm + sg * d0, a0, lm + sg * d1, a1, y0, y1, F_NOBOT, mt);
}

/* Pull each side of a frame rectangle in until it stands `inset` inside the
 * envelope, so a box on an outside wall stays behind the facade and its lining. */
static int fit_inside(const ILayout *L, float *x0, float *z0, float *x1, float *z1, float inset) {
    for (int pass = 0; pass < 2; ++pass)
        for (int side = 0; side < 4; ++side) {
            for (int step = 0; step < 20; ++step) {
                int ok = 1;
                for (int t = 0; t < 5 && ok; ++t) {
                    float f = pass ? 0.01f + 0.245f * (float)t : 0.15f + 0.175f * (float)t;   /* the middle first, then corners */
                    float x = *x0 + (*x1 - *x0) * f, z = *z0 + (*z1 - *z0) * f;
                    if (side == 0) z = *z0; else if (side == 1) x = *x1; else if (side == 2) z = *z1; else x = *x0;
                    ok = inside_by(L, x, z, inset);
                }
                if (ok) break;
                if (side == 0) *z0 += 0.04f; else if (side == 1) *x1 -= 0.04f; else if (side == 2) *z1 -= 0.04f; else *x0 += 0.04f;
            }
        }
    return *x1 - *x0 > 0.1f && *z1 - *z0 > 0.1f;
}

/* Lift shafts, stairs and risers as solids, with doors on the hall face. */
static void emit_solids(Mesh *m, const BGraph *g, const ILayout *L) {
    int16_t (*rc)[4] = (int16_t (*)[4])malloc(sizeof(int16_t) * 4 * 512);
    if (!rc) fatal("out of memory realising an interior");
    for (int r = 0; r < L->nreg; ++r) {
        if (L->reg[r].kind != IK_SOLID || !L->reg[r].cells) continue;
        int n = region_rects(L, r, 0, rc, 512);
        Mat mt = imat(g, L, IS_CORE, L->reg[r].type);
        for (int k = 0; k < n; ++k) {
            float x0 = L->gx0 + rc[k][0] * IN_CELL, z0 = L->gz0 + rc[k][1] * IN_CELL, x1 = L->gx0 + rc[k][2] * IN_CELL, z1 = L->gz0 + rc[k][3] * IN_CELL;
            if (fit_inside(L, &x0, &z0, &x1, &z1, 0.08f)) ibox(m, L, x0, z0, x1, z1, L->floorY, L->ceilY, F_SIDES, mt);
        }
    }
    free(rc);
    Mat metal = imat(g, L, IS_METAL, 0), amber = imat(g, L, IS_LIGHT, 1), white = imat(g, L, IS_LIGHT, 2), sign = imat(g, L, IS_SIGN, 0);
    for (int d = 0; d < L->ndoor; ++d) {
        const IDoor *dr = &L->door[d];
        if (dr->kind != ID_LIFT && dr->kind != ID_STAIR) continue;
        int lift = dr->kind == ID_LIFT;
        int i0 = dr->axis == 0 ? dr->at : dr->line - 1, j0 = dr->axis == 0 ? dr->line - 1 : dr->at;
        int lowHall = i0 >= 0 && j0 >= 0 && i0 < L->nx && j0 < L->nz && L->own[j0 * L->nx + i0] == dr->a;
        float sg = lowHall ? -1.0f : 1.0f;
        float a0 = (dr->axis == 0 ? L->gx0 : L->gz0) + dr->at * IN_CELL, ac = a0 + dr->len * IN_CELL * 0.5f;
        float hw = lift ? 0.5f : 0.45f, h = lift ? 2.2f : 2.1f, y0 = L->floorY;
        Mat leaf = imat(g, L, lift ? IS_LIFTDOOR : IS_DOOR, lift ? 0 : 1);
        door_panel(m, L, dr, sg, ac - hw, ac + hw, 0.0f, 0.03f, y0, y0 + h, leaf);
        door_panel(m, L, dr, sg, ac - hw - 0.08f, ac - hw, 0.0f, 0.06f, y0, y0 + h + 0.08f, metal);
        door_panel(m, L, dr, sg, ac + hw, ac + hw + 0.08f, 0.0f, 0.06f, y0, y0 + h + 0.08f, metal);
        door_panel(m, L, dr, sg, ac - hw, ac + hw, 0.0f, 0.06f, y0 + h, y0 + h + 0.08f, metal);
        if (lift) {
            door_panel(m, L, dr, sg, ac - 0.25f, ac + 0.25f, 0.0f, 0.04f, y0 + h + 0.25f, y0 + h + 0.38f, amber);   /* floor indicator */
            door_panel(m, L, dr, sg, ac + hw + 0.25f, ac + hw + 0.35f, 0.0f, 0.03f, y0 + 1.0f, y0 + 1.2f, white);  /* call button */
        } else {
            door_panel(m, L, dr, sg, ac - 0.18f, ac + 0.18f, 0.0f, 0.04f, y0 + h + 0.2f, y0 + h + 0.36f, sign);    /* exit sign */
        }
    }
}

/* The side of a fixture's footprint away from its front, `depth` deep. */
static void back_strip(const IFixture *f, float depth, float *x0, float *z0, float *x1, float *z1) {
    *x0 = f->x0; *x1 = f->x1; *z0 = f->z0; *z1 = f->z1;
    switch (f->facing) {
    case 0:  *z0 = f->z1 - depth; break;
    case 2:  *z1 = f->z0 + depth; break;
    case 1:  *x1 = f->x0 + depth; break;
    default: *x0 = f->x1 - depth; break;
    }
}

static void emit_fixture(Mesh *m, const BGraph *g, const ILayout *L, const IFixture *f, int idx) {
    float y0 = L->floorY + f->y0, y1 = L->floorY + f->y1, bx0, bz0, bx1, bz1;
    uint8_t var = (uint8_t)(hash_combine(L->seed, (uint32_t)idx) & 0xFF);
    switch (f->type) {
    case IX_DESK: case IX_COUNTER:
        /* the reception desk is dark polished stone, a cafe counter wood; both float on a lit toe-kick */
        ibox(m, L, f->x0 + 0.12f, f->z0 + 0.12f, f->x1 - 0.12f, f->z1 - 0.12f, y0, y0 + 0.08f, F_SIDES, imat(g, L, IS_LIGHT, 4));
        ibox(m, L, f->x0 + 0.05f, f->z0 + 0.05f, f->x1 - 0.05f, f->z1 - 0.05f, y0 + 0.08f, y1 - 0.04f, F_SIDES | F_BOT,
             f->type == IX_DESK ? imat(g, L, IS_STONE, 4) : imat(g, L, IS_WOOD, var));
        ibox(m, L, f->x0, f->z0, f->x1, f->z1, y1 - 0.04f, y1, F_NOBOT | F_BOT, imat(g, L, IS_STONE, f->type == IX_DESK ? 4 : 0));
        break;
    case IX_CHAIR:
        ibox(m, L, f->x0, f->z0, f->x1, f->z1, y0, y0 + 0.46f, F_NOBOT, imat(g, L, IS_FABRIC, var));
        back_strip(f, 0.08f, &bx0, &bz0, &bx1, &bz1);
        ibox(m, L, bx0, bz0, bx1, bz1, y0 + 0.46f, y1, F_NOBOT, imat(g, L, IS_FABRIC, var));
        break;
    case IX_SOFA: {
        Mat fab = imat(g, L, IS_FABRIC, (uint8_t)(L->seed >> 3));
        ibox(m, L, f->x0, f->z0, f->x1, f->z1, y0, y0 + 0.42f, F_NOBOT, fab);
        back_strip(f, 0.22f, &bx0, &bz0, &bx1, &bz1);
        ibox(m, L, bx0, bz0, bx1, bz1, y0 + 0.42f, y1, F_NOBOT, fab);
        break;
    }
    case IX_TABLE: {
        float cx = 0.5f * (f->x0 + f->x1), cz = 0.5f * (f->z0 + f->z1);
        ibox(m, L, f->x0, f->z0, f->x1, f->z1, y1 - 0.05f, y1, F_NOBOT | F_BOT, imat(g, L, IS_WOOD, var));
        ibox(m, L, cx - 0.08f, cz - 0.08f, cx + 0.08f, cz + 0.08f, y0, y1 - 0.05f, F_SIDES, imat(g, L, IS_METAL, 0));
        break;
    }
    case IX_PLANTER: {
        /* a stone pot, its rim standing proud of a top dressing; the plant in it is grown with the chunk (grow_plants) */
        float px0 = f->x0 + 0.1f, pz0 = f->z0 + 0.1f, px1 = f->x1 - 0.1f, pz1 = f->z1 - 0.1f, top = y0 + POT_HEIGHT, soil = top - 0.05f, t = 0.035f;
        Mat stone = imat(g, L, IS_STONE, 1);
        ibox(m, L, px0, pz0, px1, pz1, y0, soil, F_SIDES, stone);
        ibox(m, L, px0, pz0, px1, pz0 + t, soil, top, F_NOBOT, stone);
        ibox(m, L, px0, pz1 - t, px1, pz1, soil, top, F_NOBOT, stone);
        ibox(m, L, px0, pz0 + t, px0 + t, pz1 - t, soil, top, F_NOBOT, stone);
        ibox(m, L, px1 - t, pz0 + t, px1, pz1 - t, soil, top, F_NOBOT, stone);
        iflat(m, L, px0 + t, pz0 + t, px1 - t, pz1 - t, soil, 0, imat(g, L, IS_SOIL, 0));
        break;
    }
    case IX_FEATURE: {
        float cx = 0.5f * (f->x0 + f->x1), cz = 0.5f * (f->z0 + f->z1);
        ibox(m, L, f->x0, f->z0, f->x1, f->z1, y0, y0 + 0.4f, F_NOBOT, imat(g, L, IS_STONE, 0));
        ibox(m, L, cx - 0.3f, cz - 0.3f, cx + 0.3f, cz + 0.3f, y0 + 0.4f, y1, F_NOBOT, imat(g, L, IS_METAL, 1));
        ibox(m, L, cx - 0.32f, cz - 0.32f, cx + 0.32f, cz + 0.32f, y1 - 0.9f, y1 - 0.75f, F_NOBOT | F_BOT, imat(g, L, IS_LIGHT, 3));
        break;
    }
    case IX_FLIGHT: {
        float rise = y1 - y0;
        int n = (int)ceilf(rise / 0.18f);
        if (n < 2) n = 2;
        Mat step = imat(g, L, IS_STONE, 2), glass = imat(g, L, IS_GLASS, 0), metal = imat(g, L, IS_METAL, 0);
        int alongZ = f->facing == 0 || f->facing == 2;
        float r0 = alongZ ? f->z0 : f->x0, r1 = alongZ ? f->z1 : f->x1;      /* run, low end to high end */
        if (f->facing == 0 || f->facing == 3) { float t = r0; r0 = r1; r1 = t; }
        /* solid steps: each from the floor to its tread, over its own going only */
        for (int k = 0; k < n; ++k) {
            float a = r0 + (r1 - r0) * (float)k / (float)n, b = r0 + (r1 - r0) * (float)(k + 1) / (float)n;
            float lo = minf(a, b), hi = maxf(a, b), h = y0 + rise * (float)(k + 1) / (float)n;
            if (alongZ) ibox(m, L, f->x0, lo, f->x1, hi, y0, h, F_NOBOT, step);
            else        ibox(m, L, lo, f->z0, hi, f->z1, y0, h, F_NOBOT, step);
        }
        /* a glass balustrade with a handrail up each side, following the nosings */
        float yb0 = y0 + rise / (float)n, yb1 = y1, rail = 0.95f;
        for (int s = 0; s < 2; ++s) {
            float c = alongZ ? (s ? f->x1 - 0.04f : f->x0 + 0.04f) : (s ? f->z1 - 0.04f : f->z0 + 0.04f);
            for (int side = 0; side < 2; ++side) {
                float o = side ? 0.012f : -0.012f, nrm = side ? 1.0f : -1.0f;
                if (alongZ) iwall_slope(m, L, c + o, r0, c + o, r1, yb0, yb1, rail - 0.05f, nrm, 0.0f, glass);
                else        iwall_slope(m, L, r0, c + o, r1, c + o, yb0, yb1, rail - 0.05f, 0.0f, nrm, glass);
                o = side ? 0.03f : -0.03f;
                if (alongZ) iwall_slope(m, L, c + o, r0, c + o, r1, yb0 + rail - 0.05f, yb1 + rail - 0.05f, 0.06f, nrm, 0.0f, metal);
                else        iwall_slope(m, L, r0, c + o, r1, c + o, yb0 + rail - 0.05f, yb1 + rail - 0.05f, 0.06f, 0.0f, nrm, metal);
            }
        }
        break;
    }
    case IX_CUBICLE: {
        Mat part = imat(g, L, IS_LINING, 1);
        float t = 0.025f;
        if (f->facing == 0 || f->facing == 2) {
            ibox(m, L, f->x0, f->z0, f->x0 + t, f->z1, y0 + 0.15f, y1, F_NOBOT | F_BOT, part);
            ibox(m, L, f->x1 - t, f->z0, f->x1, f->z1, y0 + 0.15f, y1, F_NOBOT | F_BOT, part);
            float zf = f->facing == 0 ? f->z0 : f->z1 - t;
            ibox(m, L, f->x0 + t, zf, f->x1 - t, zf + t, y0 + 0.15f, y1, F_NOBOT | F_BOT, part);
        } else {
            ibox(m, L, f->x0, f->z0, f->x1, f->z0 + t, y0 + 0.15f, y1, F_NOBOT | F_BOT, part);
            ibox(m, L, f->x0, f->z1 - t, f->x1, f->z1, y0 + 0.15f, y1, F_NOBOT | F_BOT, part);
            float xf = f->facing == 3 ? f->x0 : f->x1 - t;
            ibox(m, L, xf, f->z0 + t, xf + t, f->z1 - t, y0 + 0.15f, y1, F_NOBOT | F_BOT, part);
        }
        break;
    }
    case IX_SCREEN:
        ibox(m, L, f->x0, f->z0, f->x1, f->z1, y0, y1, F_NOBOT | F_BOT, imat(g, L, IS_SCREEN, var));
        break;
    case IX_TURNSTILE:
        ibox(m, L, f->x0, f->z0, f->x1, f->z1, y0, y1 - 0.03f, F_NOBOT, imat(g, L, IS_METAL, 2));
        ibox(m, L, f->x0 + 0.02f, f->z0 + 0.02f, f->x1 - 0.02f, f->z1 - 0.02f, y1 - 0.03f, y1, F_NOBOT, imat(g, L, IS_LIGHT, 0));
        break;
    case IX_PALLET:
        ibox(m, L, f->x0, f->z0, f->x1, f->z1, y0, y0 + 0.15f, F_NOBOT, imat(g, L, IS_WOOD, 1));
        if (y1 > y0 + 0.3f) ibox(m, L, f->x0 + 0.05f, f->z0 + 0.05f, f->x1 - 0.05f, f->z1 - 0.05f, y0 + 0.15f, y1, F_NOBOT, imat(g, L, IS_FABRIC, 200));
        break;
    case IX_VANITY: case IX_UNIT:
        ibox(m, L, f->x0, f->z0, f->x1, f->z1, y0, y1, F_NOBOT, imat(g, L, f->region >= 0 && L->reg[f->region].type == IR_PLANT ? IS_METAL : IS_STONE, 3));
        break;
    default:   /* shelves, bins, racks, lockers */
        ibox(m, L, f->x0, f->z0, f->x1, f->z1, y0, y1, F_NOBOT, imat(g, L, IS_METAL, (uint8_t)(10 + f->type)));
        break;
    }
}

/* The building's mark on the core face the entrance looks at (the planning
 * frame's front is the low-z edge, so that face looks to -z): a stone panel on
 * the widest stretch of it that is clear of doors and faces open floor, into
 * which shadeInterior (IS_EMBLEM) inlays the mark and a wordmark. */
static void emit_emblem(Mesh *m, const BGraph *g, const ILayout *L) {
    int16_t (*rc)[4] = (int16_t (*)[4])malloc(sizeof(int16_t) * 4 * 512);
    if (!rc) fatal("out of memory realising an interior");
    float best = -1e9f, bx0 = 0.0f, bx1 = 0.0f, bz = 0.0f;
    for (int r = 0; r < L->nreg; ++r) {
        if (L->reg[r].kind != IK_SOLID || !L->reg[r].cells) continue;
        int n = region_rects(L, r, 0, rc, 512);
        for (int k = 0; k < n; ++k) {
            int j = rc[k][1], start = -1;
            if (j < 1) continue;
            for (int i = rc[k][0]; i <= rc[k][2]; ++i) {
                int ok = 0;
                if (i < rc[k][2]) {
                    int c = (j - 1) * L->nx + i;
                    ok = L->occ[c] == IO_USABLE && L->own[c] != 255 && L->reg[L->own[c]].kind == IK_OPEN && !(L->flag[c] & IF_EDGE);
                    for (int d = 0; d < L->ndoor && ok; ++d) {
                        const IDoor *dr = &L->door[d];
                        if (dr->axis == 0 && dr->line == j && i >= dr->at - 1 && i <= dr->at + dr->len) ok = 0;
                    }
                }
                if (ok && start < 0) start = i;
                if (!ok && start >= 0) {
                    float w = (float)(i - start) * IN_CELL, score = minf(w, 4.0f) - 0.02f * (float)j * IN_CELL;   /* wide, then near the front */
                    if (w >= 1.5f && score > best) { best = score; bx0 = L->gx0 + start * IN_CELL; bx1 = L->gx0 + i * IN_CELL; bz = L->gz0 + j * IN_CELL; }
                    start = -1;
                }
            }
        }
    }
    free(rc);
    if (best < -1e8f) return;
    float w = minf(bx1 - bx0 - 0.2f, 4.0f), cx = 0.5f * (bx0 + bx1);
    Mat mt = imat(g, L, IS_EMBLEM, 0);
    mt.p[2] = q_half(w);   /* the shader centres the mark on the panel */
    iwall(m, L, cx - w * 0.5f, bz - 0.02f, cx + w * 0.5f, bz - 0.02f, L->floorY, wall_top(L), 0, -1, mt);
}

void emit_interior(Mesh *m, const BGraph *g, const ILayout *L) {
    const BSection *s = &g->sec[L->section];
    const IAtrium *A = &L->atrium;
    int hasVoid = A->x1 > A->x0 && (A->mode == IA_GALLERY || A->mode == IA_PODIUM);
    float vx0 = L->gx0 + A->x0 * IN_CELL, vx1 = L->gx0 + A->x1 * IN_CELL, vz0 = L->gz0 + A->z0 * IN_CELL, vz1 = L->gz0 + A->z1 * IN_CELL;

    /* floor: the true footprint, with region finishes laid over it in whole cells */
    emit_cap(m, &s->foot, L->floorY + 0.01f, 0, iframe(m, imat(g, L, IS_FLOOR, 0)));
    int16_t (*rc)[4] = (int16_t (*)[4])malloc(sizeof(int16_t) * 4 * 1024);
    if (!rc) fatal("out of memory realising an interior");
    for (int r = 1; r < L->nreg; ++r) {
        if (L->reg[r].kind == IK_SOLID || !L->reg[r].cells) continue;
        int n = region_rects(L, r, 1, rc, 1024);
        Mat mt = iframe(m, imat(g, L, IS_ROOMFLOOR, L->reg[r].type));
        for (int k = 0; k < n; ++k)
            iflat(m, L, L->gx0 + rc[k][0] * IN_CELL, L->gz0 + rc[k][1] * IN_CELL, L->gx0 + rc[k][2] * IN_CELL, L->gz0 + rc[k][3] * IN_CELL, L->floorY + 0.02f, 0, mt);
    }
    free(rc);

    /* ceiling, open over a podium void */
    Mat ceiling = iframe(m, imat(g, L, IS_CEILING, 0));
    if (A->mode == IA_PODIUM && hasVoid) emit_cap_minus(m, L, &s->foot.outer, vx0, vz0, vx1, vz1, L->ceilY - 0.03f, 1, ceiling);
    else emit_cap(m, &s->foot, L->ceilY - 0.03f, 1, ceiling);

    emit_envelope_inside(m, g, L, s, L->section);
    emit_partitions(m, g, L);
    emit_solids(m, g, L);
    emit_emblem(m, g, L);
    const IFixture *stair = NULL;
    for (int f = 0; f < L->nfix; ++f) {
        emit_fixture(m, g, L, &L->fix[f], f);
        if (L->fix[f].type == IX_FLIGHT && L->fix[f].region == 0) stair = &L->fix[f];
    }

    if (hasVoid && A->mode == IA_GALLERY) {
        /* the gallery: a slab round the void at half the storey height */
        emit_cap_minus(m, L, &s->foot.outer, vx0, vz0, vx1, vz1, A->galleryY, 0, iframe(m, imat(g, L, IS_FLOOR, 1)));
        emit_cap_minus(m, L, &s->foot.outer, vx0, vz0, vx1, vz1, A->galleryY - 0.35f, 1, iframe(m, imat(g, L, IS_CEILING, 1)));
        emit_void_edges(m, g, L, A->galleryY - 0.35f, A->galleryY, 1.05f, stair);
    } else if (hasVoid) {
        /* a podium atrium: the void rises through the floors above to a glazed roof */
        Mat office = imat(g, L, IS_OFFICE, 0), edge = imat(g, L, IS_WALL, 1), glass = imat(g, L, IS_GLASS, 0), metal = imat(g, L, IS_METAL, 0);
        float y0 = L->ceilY - 0.03f, y1 = A->topY;
        iwall(m, L, vx0, vz0, vx1, vz0, y0, y1, 0, 1, office);
        iwall(m, L, vx1, vz0, vx1, vz1, y0, y1, -1, 0, office);
        iwall(m, L, vx1, vz1, vx0, vz1, y0, y1, 0, -1, office);
        iwall(m, L, vx0, vz1, vx0, vz0, y0, y1, 1, 0, office);
        for (int f = 0; f < A->levels; ++f) {
            float yl = L->ceilY + (float)f * g->floorH, e = 0.02f, r0 = yl + 0.05f, r1 = yl + 1.1f;
            iwall(m, L, vx0, vz0 + e, vx1, vz0 + e, yl - 0.35f, yl + 0.05f, 0, 1, edge);
            iwall(m, L, vx1 - e, vz0, vx1 - e, vz1, yl - 0.35f, yl + 0.05f, -1, 0, edge);
            iwall(m, L, vx1, vz1 - e, vx0, vz1 - e, yl - 0.35f, yl + 0.05f, 0, -1, edge);
            iwall(m, L, vx0 + e, vz1, vx0 + e, vz0, yl - 0.35f, yl + 0.05f, 1, 0, edge);
            ibox(m, L, vx0, vz0 + 0.05f, vx1, vz0 + 0.08f, r0, r1, F_NOBOT, glass);
            ibox(m, L, vx0, vz1 - 0.08f, vx1, vz1 - 0.05f, r0, r1, F_NOBOT, glass);
            ibox(m, L, vx0 + 0.05f, vz0 + 0.08f, vx0 + 0.08f, vz1 - 0.08f, r0, r1, F_NOBOT, glass);
            ibox(m, L, vx1 - 0.08f, vz0 + 0.08f, vx1 - 0.05f, vz1 - 0.08f, r0, r1, F_NOBOT, glass);
            ibox(m, L, vx0, vz0 + 0.03f, vx1, vz0 + 0.1f, r1, r1 + 0.05f, F_NOBOT | F_BOT, metal);
            ibox(m, L, vx0, vz1 - 0.1f, vx1, vz1 - 0.03f, r1, r1 + 0.05f, F_NOBOT | F_BOT, metal);
        }
        iflat(m, L, vx0, vz0, vx1, vz1, y1 - 0.02f, 1, imat(g, L, IS_SKYLIGHT, 0));
        for (float x = vx0 + 1.5f; x < vx1 - 0.5f; x += 1.5f) ibox(m, L, x - 0.04f, vz0, x + 0.04f, vz1, y1 - 0.2f, y1 - 0.03f, F_SIDES | F_BOT, metal);
    }
}

/* Steel doors the building graph does not hold: a synthesised service
 * entrance, and the escape stair's fire exits. */
static void emit_steel_door(Mesh *m, const BGraph *g, const BLoop *l, int face, float u, float w, uint32_t seed) {
    const float gy = G0;
    Xf sv = xf_save(m);
    float len = enter_face(m, l, face);
    Face f = face_local(len);
    float u0 = clampf(u - w * 0.5f, 0.2f, len - 0.2f - w), u1 = u0 + w, y1 = gy + 2.2f;
    emit_box(m, v3(u0 - 0.1f, gy, 0.0f), v3(u1 + 0.1f, y1 + 0.1f, 0.06f), F_NOBOT, mat(MAT_METAL, g->seed, 3, 0, 0), UV_FACADE);
    emit_face_panel(m, &f, u0, u1, gy, y1, 0.08f, mat(MAT_DOOR, seed, DOOR_SERVICE, q_half(w), q_half(2.2f)));
    emit_emissive_box(m, v3(0.5f * (u0 + u1), y1 + 0.35f, 0.2f), 0.12f, 0, 2, 0);
    xf_restore(m, sv);
}

void emit_interior_doors(Mesh *m, const BGraph *g, const ILayout *L) {
    const BLoop *l = &g->sec[L->section].foot.outer;
    if (L->synthFace >= 0) emit_steel_door(m, g, l, L->synthFace, L->synthU, L->synthW, hash_combine(L->seed, 0x5EDu));
    for (int d = 0; d < L->ndoor; ++d) {
        const IDoor *dr = &L->door[d];
        if (dr->kind != ID_FIRE) continue;
        float a = (dr->at + dr->len * 0.5f) * IN_CELL, lm = dr->line * IN_CELL, fx, fz, x, z;
        if (dr->axis == 0) { fx = L->gx0 + a; fz = L->gz0 + lm; } else { fx = L->gx0 + lm; fz = L->gz0 + a; }
        interior_to_local(L, fx, fz, &x, &z);
        int best = -1;
        float bd = 1.5f, bu = 0.0f;
        for (int k = 0; k < l->n; ++k) {
            EdgeInfo e = edge_of(l, k);
            float t = (x - e.ax) * e.ex + (z - e.az) * e.ez;
            if (t < 0.8f || t > e.len - 0.8f) continue;
            float px = e.ax + e.ex * t - x, pz = e.az + e.ez * t - z, dd = sqrtf(px * px + pz * pz);
            if (dd < bd) { bd = dd; best = k; bu = t; }
        }
        if (best >= 0) emit_steel_door(m, g, l, best, bu, 1.0f, hash_combine(L->seed, (uint32_t)d));
    }
}
