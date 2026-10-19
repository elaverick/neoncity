#include "city_internal.h"

/* ===========================================================================
 * Arcologies (src/gen/arcology.c): the concourse level, its market blocks and the
 * canyon realised in the building frame. As for a lobby, the planning frame is
 * the building frame turned in quarter turns, so its rectangles are boxes.
 * Every MAT_ARCOLOGY vertex carries its stratum height, so shadeArcology can
 * make the street stratum wet, grimy and sodium-lit and the upper strata
 * sparse and warm. Signs, screens and lamp heads reuse the street's neon,
 * screen and emissive materials.
 * ========================================================================= */

static Mat amat(const ALayout *A, int surf, int var, float t) {
    return mat(MAT_ARCOLOGY, A->seed, (uint8_t)surf, (uint8_t)var, (uint8_t)(clampf(t, 0.0f, 1.0f) * 255.0f + 0.5f));
}

/* Frame face bits (F_NX .. F_PZ) as the building frame's. */
static int afaces(const ALayout *A, int faces) {
    static const float N[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
    int out = faces & (F_TOP | F_BOT);
    for (int f = 0; f < 4; ++f)
        if (faces & (1 << f)) {
            float x, z;
            arcology_to_local(A, N[f][0], N[f][1], &x, &z);
            out |= 1 << (x < -0.5f ? 0 : x > 0.5f ? 1 : z < -0.5f ? 2 : 3);
        }
    return out;
}

static void abox(Mesh *m, const ALayout *A, float x0, float z0, float x1, float z1, float y0, float y1, int faces, Mat mt) {
    float ax, az, bx, bz;
    arcology_to_local(A, x0, z0, &ax, &az);
    arcology_to_local(A, x1, z1, &bx, &bz);
    if (y1 - y0 < 1e-3f || fabsf(ax - bx) < 1e-3f || fabsf(az - bz) < 1e-3f) return;
    emit_box(m, v3(minf(ax, bx), y0, minf(az, bz)), v3(maxf(ax, bx), y1, maxf(az, bz)), afaces(A, faces), mt, UV_FACADE);
}

static void abox_r(Mesh *m, const ALayout *A, ARect r, float y0, float y1, int faces, Mat mt) { abox(m, A, r.x0, r.z0, r.x1, r.z1, y0, y1, faces, mt); }

/* A flat frame rectangle at y; uv building metres, or given per frame corner (x0z0, x1z0, x1z1, x0z1). */
static void aflat(Mesh *m, const ALayout *A, ARect r, float y, int down, const float (*uvIn)[2], Mat mt) {
    float px[4] = { r.x0, r.x1, r.x1, r.x0 }, pz[4] = { r.z0, r.z0, r.z1, r.z1 }, uv[4][2];
    vec3 p[4];
    if (r.x1 - r.x0 < 1e-3f || r.z1 - r.z0 < 1e-3f) return;
    for (int i = 0; i < 4; ++i) {
        float x, z;
        arcology_to_local(A, px[i], pz[i], &x, &z);
        p[i] = v3(x, y, z);
        uv[i][0] = uvIn ? uvIn[i][0] : x; uv[i][1] = uvIn ? uvIn[i][1] : z;
    }
    emit_quad(m, p[0], p[1], p[2], p[3], v3(0, down ? -1.0f : 1.0f, 0), uv, mt);
}

/* A vertical quad on the frame line (x0,z0)-(x1,z1), y0..y1, facing frame (nx, nz); uv u0..u1 along it, v0..v1 up. */
static void awall(Mesh *m, const ALayout *A, float x0, float z0, float x1, float z1, float y0, float y1, float nx, float nz,
                  float u0, float u1, float v0, float v1, Mat mt) {
    float ax, az, bx, bz, lnx, lnz;
    if (y1 - y0 < 1e-3f || (fabsf(x1 - x0) < 1e-3f && fabsf(z1 - z0) < 1e-3f)) return;
    arcology_to_local(A, x0, z0, &ax, &az);
    arcology_to_local(A, x1, z1, &bx, &bz);
    arcology_to_local(A, nx, nz, &lnx, &lnz);
    float uv[4][2] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } };
    emit_quad(m, v3(ax, y0, az), v3(bx, y0, bz), v3(bx, y1, bz), v3(ax, y1, az), v3(lnx, 0, lnz), uv, mt);
}

/* A sign plane facing (nx, nz), its uv running 0..1 left to right as seen from the front. */
static void asign_quad(Mesh *m, const ALayout *A, float x0, float z0, float x1, float z1, float y0, float y1, float nx, float nz, Mat mt) {
    if ((x1 - x0) * nz - (z1 - z0) * nx < 0.0f) { float t = x0; x0 = x1; x1 = t; t = z0; z0 = z1; z1 = t; }
    awall(m, A, x0, z0, x1, z1, y0, y1, nx, nz, 0, 1, 0, 1, mt);
}

/* Greedy rectangles over the cells whose use is in useMask (and in block blk, if >= 0). */
int arc_rects(const ALayout *A, uint32_t useMask, int blk, ARect *out, int max) {
    int N = A->nx * A->nz, n = 0;
    uint8_t *used = (uint8_t *)calloc((size_t)N, 1);
    if (!used) fatal("out of memory realising an arcology");
#define AWANT(i, j) ((useMask & (1u << A->use[(j) * A->nx + (i)])) && (blk < 0 || A->blk[(j) * A->nx + (i)] == blk) && !used[(j) * A->nx + (i)])
    for (int j = 0; j < A->nz && n < max; ++j)
        for (int i = 0; i < A->nx && n < max; ++i) {
            if (!AWANT(i, j)) continue;
            int i1 = i + 1, j1 = j + 1;
            while (i1 < A->nx && AWANT(i1, j)) i1++;
            for (;;) {
                if (j1 >= A->nz) break;
                int ok = 1;
                for (int a = i; a < i1 && ok; ++a) ok = AWANT(a, j1);
                if (!ok) break;
                j1++;
            }
            for (int b = j; b < j1; ++b) for (int a = i; a < i1; ++a) used[b * A->nx + a] = 1;
            out[n].x0 = A->gx0 + (float)i * AR_CELL; out[n].z0 = A->gz0 + (float)j * AR_CELL;
            out[n].x1 = A->gx0 + (float)i1 * AR_CELL; out[n].z1 = A->gz0 + (float)j1 * AR_CELL;
            n++;
        }
#undef AWANT
    free(used);
    return n;
}

/* A footprint outline (building frame) minus frame rectangles, as caps facing up or down. */
typedef struct CapPiece { int n; float p[2 * BG_MAX_PV][2]; } CapPiece;

void emit_cap_holes(Mesh *m, const ALayout *A, const BLoop *outer, const ARect *holes, int nh, float y, int down, Mat mt) {
    enum { MAXP = 48 };
    CapPiece *cur = (CapPiece *)malloc(sizeof(CapPiece) * MAXP), *nxt = (CapPiece *)malloc(sizeof(CapPiece) * MAXP);
    if (!cur || !nxt) fatal("out of memory realising an arcology");
    int nc = 1;
    cur[0].n = outer->n;
    for (int i = 0; i < outer->n; ++i) { cur[0].p[i][0] = outer->x[i]; cur[0].p[i][1] = outer->z[i]; }
    for (int h = 0; h < nh; ++h) {
        float ax, az, bx, bz;
        arcology_to_local(A, holes[h].x0, holes[h].z0, &ax, &az);
        arcology_to_local(A, holes[h].x1, holes[h].z1, &bx, &bz);
        float x0 = minf(ax, bx), x1 = maxf(ax, bx), z0 = minf(az, bz), z1 = maxf(az, bz);
        int nn = 0;
        for (int c = 0; c < nc; ++c) {
            float b[2 * BG_MAX_PV][2], t[2 * BG_MAX_PV][2];
            for (int piece = 0; piece < 4 && nn < MAXP; ++piece) {
                CapPiece *o = &nxt[nn];
                if (piece == 0)      o->n = clip_half((const float (*)[2])cur[c].p, cur[c].n, -1, 0, x0, o->p);
                else if (piece == 1) o->n = clip_half((const float (*)[2])cur[c].p, cur[c].n, 1, 0, -x1, o->p);
                else {
                    int k = clip_half((const float (*)[2])cur[c].p, cur[c].n, 1, 0, -x0, b);
                    k = clip_half((const float (*)[2])b, k, -1, 0, x1, t);
                    o->n = piece == 2 ? clip_half((const float (*)[2])t, k, 0, -1, z0, o->p) : clip_half((const float (*)[2])t, k, 0, 1, -z1, o->p);
                }
                if (o->n >= 3) nn++;
            }
        }
        CapPiece *sw = cur; cur = nxt; nxt = sw;
        nc = nn;
    }
    for (int c = 0; c < nc; ++c) {
        if (cur[c].n > BG_MAX_PV) continue;
        BFoot f;
        memset(&f, 0, sizeof(f));
        f.outer.n = cur[c].n;
        for (int i = 0; i < cur[c].n; ++i) { f.outer.x[i] = cur[c].p[i][0]; f.outer.z[i] = cur[c].p[i][1]; }
        if (fabsf(bloop_area(&f.outer)) < 0.01f) continue;
        emit_cap(m, &f, y, down, mt);
    }
    free(cur); free(nxt);
}

/* Runs (from, to along the axis) of a concourse's line `off` across its centre that lie over street, not the canyon floor. */
static int arc_street_runs(const ALayout *A, const AConcourse *c, float off, float (*run)[2], int max) {
    float mid = (c->axis ? 0.5f * (c->r.x0 + c->r.x1) : 0.5f * (c->r.z0 + c->r.z1)) + off;
    float a0 = c->axis ? c->r.z0 : c->r.x0, a1 = c->axis ? c->r.z1 : c->r.x1, start = 0.0f;
    int n = 0, inRun = 0;
    for (float a = a0; a <= a1 + 1e-3f; a += AR_CELL) {
        int u = a < a1 ? (c->axis ? arcology_use_at(A, mid, a + 0.25f) : arcology_use_at(A, a + 0.25f, mid)) : AU_OUT;
        int ok = u == AU_CONCOURSE || u == AU_COLONNADE;
        if (ok && !inRun) { inRun = 1; start = a; }
        if (!ok && inRun) { inRun = 0; if (a - start >= 1.0f && n < max) { run[n][0] = start; run[n][1] = a; n++; } }
    }
    return n;
}

/* Sodium lamps along a concourse, for the floor's analytic pools (shadeArcology):
 * step | phase << 8 | offset across << 16 | height << 24, in decimetres. */
static uint32_t lamp_pack(const ALayout *A, const AConcourse *c) {
    uint32_t s = (uint32_t)clampf(c->lampStep * 10.0f + 0.5f, 1.0f, 255.0f), p = (uint32_t)clampf(c->lampPhase * 10.0f + 0.5f, 0.0f, 255.0f);
    uint32_t o = (uint32_t)clampf(c->lampOff * 10.0f + 0.5f, 0.0f, 255.0f), h = (uint32_t)clampf((A->lamp[0].y - A->floorY) * 10.0f + 0.5f, 0.0f, 255.0f);
    if (!A->nlamp) h = 50;
    return s | p << 8 | o << 16 | h << 24;
}

/* The concourse level's floor: base, streets (with their lamp pools), plaza, hall, yard, stalls. */
static void emit_arc_floor(Mesh *m, const BGraph *g, const ALayout *A) {
    const BSection *gs = &g->sec[A->tier[0].section];
    float fy = A->floorY;
    emit_cap(m, &gs->foot, fy + 0.01f, 0, iframe(m, amat(A, ARS_FLOOR, 5, 0.0f)));
    for (int c = 0; c < A->nconc; ++c) {
        const AConcourse *cc = &A->conc[c];
        float run[16][2];
        int nr = arc_street_runs(A, cc, 0.0f, run, 16);
        float mid = cc->axis ? 0.5f * (cc->r.x0 + cc->r.x1) : 0.5f * (cc->r.z0 + cc->r.z1), hw = cc->axis ? 0.5f * (cc->r.x1 - cc->r.x0) : 0.5f * (cc->r.z1 - cc->r.z0);
        float a0 = cc->axis ? cc->r.z0 : cc->r.x0;
        Mat mt = amat(A, ARS_STREET, cc->boulevard ? 1 : 0, 0.0f);
        mt.aux = lamp_pack(A, cc);
        for (int r = 0; r < nr; ++r) {
            ARect q;
            float uv[4][2];
            if (cc->axis) {
                q = (ARect){ mid - hw, run[r][0], mid + hw, run[r][1] };
                float u0 = run[r][0] - a0, u1 = run[r][1] - a0;
                uv[0][0] = u0; uv[0][1] = -hw; uv[1][0] = u0; uv[1][1] = hw; uv[2][0] = u1; uv[2][1] = hw; uv[3][0] = u1; uv[3][1] = -hw;
            } else {
                q = (ARect){ run[r][0], mid - hw, run[r][1], mid + hw };
                float u0 = run[r][0] - a0, u1 = run[r][1] - a0;
                uv[0][0] = u0; uv[0][1] = -hw; uv[1][0] = u1; uv[1][1] = -hw; uv[2][0] = u1; uv[2][1] = hw; uv[3][0] = u0; uv[3][1] = hw;
            }
            aflat(m, A, q, fy + 0.02f, 0, (const float (*)[2])uv, mt);
        }
    }
    ARect *rc = (ARect *)malloc(sizeof(ARect) * 1024);
    if (!rc) fatal("out of memory realising an arcology");
    static const struct { int use, var; } F[] = { { AU_COLONNADE, 4 }, { AU_PLAZA, 0 }, { AU_HALL, 2 }, { AU_YARD, 1 }, { AU_STALL, 3 }, { AU_COREHALL, 6 }, { AU_KIOSK, 7 } };
    for (int f = 0; f < (int)ARRAY_LEN(F); ++f) {
        int n = arc_rects(A, 1u << F[f].use, -1, rc, 1024);
        Mat mt = iframe(m, amat(A, ARS_FLOOR, F[f].var, 0.0f));
        for (int k = 0; k < n; ++k) aflat(m, A, rc[k], fy + (F[f].use == AU_COLONNADE ? 0.02f : 0.025f), 0, NULL, mt);
    }
    free(rc);
}

/* The concourse ceiling (open where the canyon rises), with stepped beams on the column lines. */
static void emit_arc_ceiling(Mesh *m, const BGraph *g, const ALayout *A) {
    const BSection *gs = &g->sec[A->tier[0].section];
    float top = A->ceilY - 0.4f, s = A->colSpacing;
    emit_cap_holes(m, A, &gs->foot.outer, A->voids, A->nvoid, top, 1, iframe(m, amat(A, ARS_CEILING, 0, 0.0f)));
    Mat beam = amat(A, ARS_BEAM, 0, 0.0f), transfer = amat(A, ARS_BEAM, 1, 0.0f);
    float gx1 = A->gx0 + (float)A->nx * AR_CELL, gz1 = A->gz0 + (float)A->nz * AR_CELL;
    for (int axis = 0; axis < 2; ++axis) {
        float o = axis ? A->gridOx : A->gridOz, lo = axis ? A->gx0 : A->gz0, hi = axis ? gx1 : gz1;
        for (float line = o - ceilf((o - lo) / s) * s; line <= hi; line += s) {
            if (line < lo + 1.0f || line > hi - 1.0f) continue;
            float a0 = axis ? A->gz0 : A->gx0, a1 = axis ? gz1 : gx1, start = 0.0f;
            int inRun = 0;
            for (float a = a0; a <= a1 + 1e-3f; a += AR_CELL) {
                float x = axis ? line : a + 0.25f, z = axis ? a + 0.25f : line;
                int ok = a < a1 && arcology_use_at(A, x, z) != AU_OUT;
                for (int v = 0; v < A->nvoid && ok; ++v) ok = !(x > A->voids[v].x0 - 0.8f && x < A->voids[v].x1 + 0.8f && z > A->voids[v].z0 - 0.8f && z < A->voids[v].z1 + 0.8f);
                if (ok && !inRun) { inRun = 1; start = a; }
                if (!ok && inRun) {
                    inRun = 0;
                    if (a - start < 2.0f) continue;
                    if (axis) { abox(m, A, line - 0.35f, start, line + 0.35f, a, top - 1.3f, top - 0.6f, F_SIDES | F_BOT, beam);
                                abox(m, A, line - 0.7f, start, line + 0.7f, a, top - 0.6f, top, F_SIDES | F_BOT, beam); }
                    else      { abox(m, A, start, line - 0.35f, a, line + 0.35f, top - 1.3f, top - 0.6f, F_SIDES | F_BOT, beam);
                                abox(m, A, start, line - 0.7f, a, line + 0.7f, top - 0.6f, top, F_SIDES | F_BOT, beam); }
                }
            }
        }
    }
    for (int b = 0; b < A->nbeam; ++b) {
        const ABeam *bm = &A->beam[b];
        if (fabsf(bm->z1 - bm->z0) < 0.01f) abox(m, A, minf(bm->x0, bm->x1), bm->z0 - 0.8f, maxf(bm->x0, bm->x1), bm->z0 + 0.8f, top - 1.8f, top, F_SIDES | F_BOT, transfer);
        else if (fabsf(bm->x1 - bm->x0) < 0.01f) abox(m, A, bm->x0 - 0.8f, minf(bm->z0, bm->z1), bm->x0 + 0.8f, maxf(bm->z0, bm->z1), top - 1.8f, top, F_SIDES | F_BOT, transfer);
    }
}

/* The inside of the envelope: glazing and mullions on the 1.5 m module where a
 * street meets the facade, a lining where a block stands against it, and the
 * streetlight slatted through the mullions onto the floor. */
static void emit_arc_envelope(Mesh *m, const BGraph *g, const ALayout *A) {
    int si = A->tier[0].section;
    const BLoop *l = &g->sec[si].foot.outer;
    Behind bh = { NULL, A, si };
    Mat lining = amat(A, ARS_LINING, 0, 0.0f), head = amat(A, ARS_LINING, 1, 0.0f), metal = amat(A, ARS_METAL, 7, 0.0f);
    Mat glass = amat(A, ARS_RAIL, 3, 0.0f), slat = amat(A, ARS_SLAT, 0, 0.0f);
    float y0 = A->floorY, gt = A->ceilY - 1.2f, yc = A->ceilY - 0.4f;
    float uvq[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    for (int k = 0; k < l->n; ++k) {
        EdgeInfo e = edge_of(l, k);
        if (e.len < 0.3f) continue;
        float span[64][2], op[8][3];
        uint8_t lined[64];
        int ns = envelope_spans(g, &bh, l, 0, k, span, lined, 64), nop = face_openings(g, &bh, 0, k, op, 8);
        Xf sv = xf_save(m);
        enter_face(m, l, k);
        for (int q = 0; q < ns; ++q) {
            float u0 = span[q][0], u1 = q == ns - 1 ? e.len : span[q][1];
            if (u1 - u0 < 0.01f) continue;
            if (lined[q]) {
                emit_quad(m, v3(u0, y0, -0.05f), v3(u1, y0, -0.05f), v3(u1, yc, -0.05f), v3(u0, yc, -0.05f), v3(0, 0, -1), uvq, lining);
                emit_quad(m, v3(u0, y0, -0.05f), v3(u1, y0, -0.05f), v3(u1, yc, -0.05f), v3(u0, yc, -0.05f), v3(0, 0, 1), uvq, lining);
                continue;
            }
            emit_quad(m, v3(u0, gt, -0.03f), v3(u1, gt, -0.03f), v3(u1, yc, -0.03f), v3(u0, yc, -0.03f), v3(0, 0, -1), uvq, head);
            /* glass, cut round the openings (glazed above them up to the head) */
            float cur = u0;
            for (int o = 0; o <= nop; ++o) {
                float a = cur, c = o < nop ? minf(u1, op[o][0]) : u1;
                if (o < nop && op[o][1] < u0) continue;
                if (c - a > 0.01f) {
                    emit_quad(m, v3(a, y0 + 0.08f, -0.015f), v3(c, y0 + 0.08f, -0.015f), v3(c, gt, -0.015f), v3(a, gt, -0.015f), v3(0, 0, -1), uvq, glass);
                    /* streetlight through this glass: slats on the floor, keyed to the mullions */
                    float suv[4][2] = { { a, 0.1f }, { c, 0.1f }, { c, 4.5f }, { a, 4.5f } };
                    emit_quad(m, v3(a, y0 + 0.03f, -0.1f), v3(c, y0 + 0.03f, -0.1f), v3(c, y0 + 0.03f, -4.5f), v3(a, y0 + 0.03f, -4.5f), v3(0, 1, 0), suv, slat);
                }
                if (o < nop) {
                    float b0 = maxf(u0, op[o][0]), b1 = minf(u1, op[o][1]);
                    if (b1 - b0 > 0.01f && gt > op[o][2] + 0.1f)
                        emit_quad(m, v3(b0, op[o][2], -0.015f), v3(b1, op[o][2], -0.015f), v3(b1, gt, -0.015f), v3(b0, gt, -0.015f), v3(0, 0, -1), uvq, glass);
                    cur = maxf(cur, op[o][1]);
                }
            }
            float m0 = q > 0 ? u0 - 0.05f : u0 + 0.05f, m1 = q < ns - 1 ? u1 + 0.05f : u1 - 0.05f;
            for (float um = ceilf(m0 / 1.5f) * 1.5f; um < m1; um += 1.5f) {
                int inOpen = 0;
                for (int o = 0; o < nop && !inOpen; ++o) inOpen = um > op[o][0] - 0.05f && um < op[o][1] + 0.05f;
                float mb = y0;
                for (int o = 0; o < nop && inOpen; ++o) if (um > op[o][0] - 0.05f && um < op[o][1] + 0.05f) mb = op[o][2];
                emit_box(m, v3(um - 0.05f, mb, -0.18f), v3(um + 0.05f, gt, -0.01f), F_SIDES, metal, UV_FACADE);
            }
            emit_box(m, v3(u0, gt - 0.08f, -0.14f), v3(u1, gt, -0.01f), F_SIDES | F_BOT, metal, UV_FACADE);
        }
        xf_restore(m, sv);
    }
}

/* A megastructure column: plinth, shaft and a stepped (ziggurat) capital. */
static void emit_arc_column(Mesh *m, const ALayout *A, const AColumn *c) {
    float h = c->size * 0.5f, fy = A->floorY, top = A->ceilY - 0.4f, c0 = top - 1.7f;
    Mat col = amat(A, ARS_COLUMN, c->ring, 0.0f), cap = amat(A, ARS_CAPITAL, c->ring, 0.0f);
    abox(m, A, c->x - h * 1.15f, c->z - h * 1.15f, c->x + h * 1.15f, c->z + h * 1.15f, fy, fy + 0.6f, F_NOBOT, cap);
    abox(m, A, c->x - h, c->z - h, c->x + h, c->z + h, fy + 0.6f, c0, F_SIDES, col);
    static const float STEP[3][2] = { { 1.25f, 0.45f }, { 1.6f, 0.55f }, { 2.0f, 0.7f } };
    float y = c0;
    for (int s = 0; s < 3; ++s) {
        float w = h * STEP[s][0];
        abox(m, A, c->x - w, c->z - w, c->x + w, c->z + w, y, y + STEP[s][1], F_SIDES | F_BOT, cap);
        y += STEP[s][1];
    }
    if (c->ring) {   /* the setback piers wear a neon band, so the nested colonnades read */
        float w = h + 0.03f;
        abox(m, A, c->x - w, c->z - w, c->x + w, c->z + w, fy + 3.0f, fy + 3.18f, F_SIDES, mat(MAT_LEDSTRIP, A->seed, A->palette[1], 0, 0));
    }
}

/* Lift doors in a row on side `d` of rect r: n doors of width w. */
static void emit_arc_lifts(Mesh *m, const ALayout *A, ARect r, int d, int n, float w, float h, int var) {
    float fy = A->floorY;
    float a0 = (d == 0 || d == 2) ? r.x0 : r.z0, a1 = (d == 0 || d == 2) ? r.x1 : r.z1, line = d == 0 ? r.z0 : d == 2 ? r.z1 : d == 1 ? r.x1 : r.x0;
    float nx = d == 1 ? 1.0f : d == 3 ? -1.0f : 0.0f, nz = d == 0 ? -1.0f : d == 2 ? 1.0f : 0.0f;
    float pitch = (a1 - a0) / (float)n;
    if (pitch < w + 0.6f) { n = (int)((a1 - a0) / (w + 0.6f)); pitch = n ? (a1 - a0) / (float)n : 0.0f; }
    Mat door = amat(A, ARS_LIFTDOOR, var, 0.0f), frame = amat(A, ARS_METAL, 4, 0.0f), lamp = amat(A, ARS_LIFTDOOR, 9, 0.0f);
    for (int i = 0; i < n; ++i) {
        float c = a0 + pitch * ((float)i + 0.5f), o = 0.03f;
        if (d == 0 || d == 2) {
            asign_quad(m, A, c - w * 0.5f, line + nz * o, c + w * 0.5f, line + nz * o, fy, fy + h, nx, nz, door);
            abox(m, A, c - w * 0.5f - 0.12f, line + nz * 0.0f, c - w * 0.5f, line + nz * 0.08f, fy, fy + h + 0.12f, F_SIDES | F_TOP, frame);
            abox(m, A, c + w * 0.5f, line + nz * 0.0f, c + w * 0.5f + 0.12f, line + nz * 0.08f, fy, fy + h + 0.12f, F_SIDES | F_TOP, frame);
            abox(m, A, c - w * 0.5f, line, c + w * 0.5f, line + nz * 0.08f, fy + h, fy + h + 0.12f, F_SIDES | F_TOP | F_BOT, frame);
            asign_quad(m, A, c - 0.3f, line + nz * 0.05f, c + 0.3f, line + nz * 0.05f, fy + h + 0.3f, fy + h + 0.45f, nx, nz, lamp);
        } else {
            asign_quad(m, A, line + nx * o, c - w * 0.5f, line + nx * o, c + w * 0.5f, fy, fy + h, nx, nz, door);
            abox(m, A, line, c - w * 0.5f - 0.12f, line + nx * 0.08f, c - w * 0.5f, fy, fy + h + 0.12f, F_SIDES | F_TOP, frame);
            abox(m, A, line, c + w * 0.5f, line + nx * 0.08f, c + w * 0.5f + 0.12f, fy, fy + h + 0.12f, F_SIDES | F_TOP, frame);
            abox(m, A, line, c - w * 0.5f, line + nx * 0.08f, c + w * 0.5f, fy + h, fy + h + 0.12f, F_SIDES | F_TOP | F_BOT, frame);
            asign_quad(m, A, line + nx * 0.05f, c - 0.3f, line + nx * 0.05f, c + 0.3f, fy + h + 0.3f, fy + h + 0.45f, nx, nz, lamp);
        }
    }
}

/* Market block masses: walls along every edge between mass and anything else
 * (but the facade), flat roofs on top. mass_id: the block, 254 outside, 255 anything else. */
static int mass_id(const ALayout *A, int i, int j) {
    if (i < 0 || j < 0 || i >= A->nx || j >= A->nz) return 254;
    int c = j * A->nx + i;
    return A->use[c] == AU_BLOCK ? A->blk[c] : A->use[c] == AU_OUT ? 254 : 255;
}

static void emit_arc_blocks(Mesh *m, const ALayout *A) {
    float fy = A->floorY;
    for (int axis = 0; axis < 2; ++axis) {
        int nLines = axis == 0 ? A->nz + 1 : A->nx + 1, nAlong = axis == 0 ? A->nx : A->nz;
        for (int line = 0; line < nLines; ++line) {
            int ra = 255, rb = 255, start = 0;
            for (int k = 0; k <= nAlong; ++k) {
                int a = 255, b = 255;
                if (k < nAlong) {
                    if (axis == 0) { a = mass_id(A, k, line - 1); b = mass_id(A, k, line); }
                    else           { a = mass_id(A, line - 1, k); b = mass_id(A, line, k); }
                    if (a == b) a = b = 255;
                }
                if (k < nAlong && a == ra && b == rb) continue;
                if (k > 0 && (ra < 254 || rb < 254)) {
                    float lm = (axis == 0 ? A->gz0 : A->gx0) + (float)line * AR_CELL;
                    float s0 = (axis == 0 ? A->gx0 : A->gz0) + (float)start * AR_CELL, s1 = (axis == 0 ? A->gx0 : A->gz0) + (float)k * AR_CELL;
                    for (int side = 0; side < 2; ++side) {
                        int id = side ? rb : ra, other = side ? ra : rb;
                        if (id >= 254 || other == 254) continue;          /* none against the facade: it is opaque there */
                        const ABlock *B = &A->block[id];
                        float dir = side ? -1.0f : 1.0f;     /* the wall faces away from its mass */
                        Mat mt = amat(A, ARS_BLOCK, B->use, 0.0f);
                        mt.seed = hash_combine(A->seed, (uint32_t)id);
                        if (axis == 0) awall(m, A, s0, lm, s1, lm, fy, fy + B->height, 0.0f, dir, s0, s1, 0.0f, B->height, mt);
                        else           awall(m, A, lm, s0, lm, s1, fy, fy + B->height, dir, 0.0f, s0, s1, 0.0f, B->height, mt);
                    }
                }
                ra = a; rb = b; start = k;
            }
        }
    }
    ARect *rc = (ARect *)malloc(sizeof(ARect) * 512);
    if (!rc) fatal("out of memory realising an arcology");
    for (int b = 0; b < A->nblock; ++b) {
        const ABlock *B = &A->block[b];
        if (B->use == AB_KIOSK || B->use == AB_YARD) continue;
        int n = arc_rects(A, 1u << AU_BLOCK, b, rc, 512);
        Mat roof = iframe(m, amat(A, ARS_ROOF, B->use, 0.0f));
        for (int k = 0; k < n; ++k) aflat(m, A, rc[k], fy + B->height, 0, NULL, roof);
    }
    free(rc);
}

/* A stall: side walls, a lit back, a canopy, and a shutter if it is closed. */
static void emit_arc_stall(Mesh *m, const ALayout *A, const AStall *st) {
    float fy = A->floorY, h = 3.2f, t = 0.08f;
    ARect r = st->r;
    int d = st->facing;
    Mat in = amat(A, ARS_STALL, st->kind, 0.0f), roof = amat(A, ARS_STALLROOF, st->kind, 0.0f), shut = amat(A, ARS_SHUTTER, 0, 0.0f);
    Mat out = amat(A, ARS_BLOCK, A->block[st->block].use, 0.0f);
    in.seed = roof.seed = shut.seed = st->seed;
    out.seed = hash_combine(A->seed, (uint32_t)st->block);
    static const int FRONT[4] = { F_NZ, F_PX, F_PZ, F_NX };
    if (d == 0 || d == 2) {
        /* side walls: lit shelves inside, the block's own face outside */
        awall(m, A, r.x0 + t, r.z0, r.x0 + t, r.z1, fy, fy + h, 1.0f, 0.0f, r.z0, r.z1, 0.0f, h, in);
        awall(m, A, r.x1 - t, r.z0, r.x1 - t, r.z1, fy, fy + h, -1.0f, 0.0f, r.z0, r.z1, 0.0f, h, in);
        awall(m, A, r.x0, r.z0, r.x0, r.z1, fy, fy + h, -1.0f, 0.0f, r.z0, r.z1, 0.0f, h, out);
        awall(m, A, r.x1, r.z0, r.x1, r.z1, fy, fy + h, 1.0f, 0.0f, r.z0, r.z1, 0.0f, h, out);
        abox(m, A, r.x0, d == 0 ? r.z0 : r.z1 - 0.01f, r.x0 + t, d == 0 ? r.z0 + 0.01f : r.z1, fy, fy + h, d == 0 ? F_NZ : F_PZ, out);
        abox(m, A, r.x1 - t, d == 0 ? r.z0 : r.z1 - 0.01f, r.x1, d == 0 ? r.z0 + 0.01f : r.z1, fy, fy + h, d == 0 ? F_NZ : F_PZ, out);
        float back = d == 0 ? r.z1 - 0.02f : r.z0 + 0.02f, nz = d == 0 ? -1.0f : 1.0f;
        awall(m, A, r.x0, back, r.x1, back, fy, fy + h, 0.0f, nz, 0.0f, r.x1 - r.x0, 0.0f, h, in);
        if (st->closed) { float fz = d == 0 ? r.z0 + 0.12f : r.z1 - 0.12f; awall(m, A, r.x0 + t, fz, r.x1 - t, fz, fy, fy + h, 0.0f, nz, 0.0f, r.x1 - r.x0, 0.0f, h, shut); }
    } else {
        awall(m, A, r.x0, r.z0 + t, r.x1, r.z0 + t, fy, fy + h, 0.0f, 1.0f, r.x0, r.x1, 0.0f, h, in);
        awall(m, A, r.x0, r.z1 - t, r.x1, r.z1 - t, fy, fy + h, 0.0f, -1.0f, r.x0, r.x1, 0.0f, h, in);
        awall(m, A, r.x0, r.z0, r.x1, r.z0, fy, fy + h, 0.0f, -1.0f, r.x0, r.x1, 0.0f, h, out);
        awall(m, A, r.x0, r.z1, r.x1, r.z1, fy, fy + h, 0.0f, 1.0f, r.x0, r.x1, 0.0f, h, out);
        abox(m, A, d == 3 ? r.x0 : r.x1 - 0.01f, r.z0, d == 3 ? r.x0 + 0.01f : r.x1, r.z0 + t, fy, fy + h, d == 3 ? F_NX : F_PX, out);
        abox(m, A, d == 3 ? r.x0 : r.x1 - 0.01f, r.z1 - t, d == 3 ? r.x0 + 0.01f : r.x1, r.z1, fy, fy + h, d == 3 ? F_NX : F_PX, out);
        float back = d == 1 ? r.x0 + 0.02f : r.x1 - 0.02f, nx = d == 1 ? 1.0f : -1.0f;
        awall(m, A, back, r.z0, back, r.z1, fy, fy + h, nx, 0.0f, 0.0f, r.z1 - r.z0, 0.0f, h, in);
        if (st->closed) { float fx = d == 1 ? r.x1 - 0.12f : r.x0 + 0.12f; awall(m, A, fx, r.z0 + t, fx, r.z1 - t, fy, fy + h, nx, 0.0f, 0.0f, r.z1 - r.z0, 0.0f, h, shut); }
    }
    abox(m, A, r.x0, r.z0, r.x1, r.z1, fy + h, fy + h + 0.15f, F_TOP | F_BOT | FRONT[d], roof);
}

static void emit_arc_fixture(Mesh *m, const ALayout *A, const AFix *f) {
    float fy = A->floorY, y0 = fy + f->y0, y1 = fy + f->y1;
    Mat metal = amat(A, ARS_METAL, 3, 0.0f);
    switch (f->type) {
    case AX_COUNTER: {
        Mat c = amat(A, ARS_COUNTER, f->facing, 0.0f);
        c.seed = f->seed;
        abox(m, A, f->x0, f->z0, f->x1, f->z1, y0, y1, F_NOBOT, c);
        break;
    }
    case AX_AWNING: {
        /* sloping down and out from the stall front, with a valance and a string of lights */
        int d = f->facing;
        float hi = y1, lo = y0;
        Mat cloth = amat(A, ARS_CRATE, 10, 0.0f), lights = mat(MAT_LEDSTRIP, f->seed, A->palette[f->seed % 3], (uint8_t)((f->seed >> 3) & 1), 0);
        cloth.seed = f->seed;
        float ax0, az0, ax1, az1, bx0, bz0, bx1, bz1;   /* inner edge (a) at the stall, outer edge (b) */
        if (d == 0)      { ax0 = f->x0; ax1 = f->x1; az0 = az1 = f->z1; bx0 = f->x0; bx1 = f->x1; bz0 = bz1 = f->z0; }
        else if (d == 2) { ax0 = f->x0; ax1 = f->x1; az0 = az1 = f->z0; bx0 = f->x0; bx1 = f->x1; bz0 = bz1 = f->z1; }
        else if (d == 1) { az0 = f->z0; az1 = f->z1; ax0 = ax1 = f->x0; bz0 = f->z0; bz1 = f->z1; bx0 = bx1 = f->x1; }
        else             { az0 = f->z0; az1 = f->z1; ax0 = ax1 = f->x1; bz0 = f->z0; bz1 = f->z1; bx0 = bx1 = f->x0; }
        float lx[4], lz[4];
        arcology_to_local(A, ax0, az0, &lx[0], &lz[0]); arcology_to_local(A, ax1, az1, &lx[1], &lz[1]);
        arcology_to_local(A, bx1, bz1, &lx[2], &lz[2]); arcology_to_local(A, bx0, bz0, &lx[3], &lz[3]);
        vec3 p[4] = { v3(lx[0], hi, lz[0]), v3(lx[1], hi, lz[1]), v3(lx[2], lo, lz[2]), v3(lx[3], lo, lz[3]) };
        vec3 n = v3_norm(v3_cross(v3_sub(p[1], p[0]), v3_sub(p[3], p[0])));
        if (n.y < 0.0f) n = v3_scale(n, -1.0f);
        float len = fabsf(ax1 - ax0) + fabsf(az1 - az0);
        float uv[4][2] = { { 0, 1 }, { len, 1 }, { len, 0 }, { 0, 0 } };
        emit_quad(m, p[0], p[1], p[2], p[3], n, uv, cloth);
        emit_quad(m, p[0], p[1], p[2], p[3], v3_scale(n, -1.0f), uv, cloth);
        float onx = (float)(d == 1) - (float)(d == 3), onz = (float)(d == 2) - (float)(d == 0);
        awall(m, A, bx0, bz0, bx1, bz1, lo - 0.3f, lo, onx, onz, 0.0f, len, 0.0f, 0.3f, cloth);
        awall(m, A, bx0 + onx * 0.01f, bz0 + onz * 0.01f, bx1 + onx * 0.01f, bz1 + onz * 0.01f, lo - 0.34f, lo - 0.28f, onx, onz, 0.0f, len, 0.0f, 1.0f, lights);
        break;
    }
    case AX_GATE:
        abox(m, A, f->x0, f->z0, f->x1, f->z1, y0, y1 - 0.04f, F_NOBOT, metal);
        abox(m, A, f->x0 + 0.03f, f->z0 + 0.03f, f->x1 - 0.03f, f->z1 - 0.03f, y1 - 0.04f, y1, F_NOBOT, mat(MAT_LEDSTRIP, f->seed, 3, 0, 0));
        break;
    case AX_BOOTH:
        abox(m, A, f->x0, f->z0, f->x1, f->z1, y0, y0 + 1.1f, F_NOBOT, amat(A, ARS_KIOSK, 1, 0.0f));
        abox(m, A, f->x0 + 0.05f, f->z0 + 0.05f, f->x1 - 0.05f, f->z1 - 0.05f, y0 + 1.1f, y1 - 0.2f, F_SIDES, amat(A, ARS_RAIL, 3, 0.0f));
        abox(m, A, f->x0 - 0.1f, f->z0 - 0.1f, f->x1 + 0.1f, f->z1 + 0.1f, y1 - 0.2f, y1, F_NOBOT | F_BOT, amat(A, ARS_KIOSK, 1, 0.0f));
        break;
    case AX_BENCH: {
        Mat seat = amat(A, ARS_METAL, 8, 0.0f);
        abox(m, A, f->x0, f->z0, f->x1, f->z1, y1 - 0.08f, y1, F_NOBOT | F_BOT, seat);
        int alongX = f->x1 - f->x0 > f->z1 - f->z0;
        for (int s = 0; s < 2; ++s) {
            float a = s ? 0.85f : 0.15f;
            if (alongX) { float x = f->x0 + (f->x1 - f->x0) * a; abox(m, A, x - 0.05f, f->z0 + 0.05f, x + 0.05f, f->z1 - 0.05f, y0, y1 - 0.08f, F_SIDES, seat); }
            else        { float z = f->z0 + (f->z1 - f->z0) * a; abox(m, A, f->x0 + 0.05f, z - 0.05f, f->x1 - 0.05f, z + 0.05f, y0, y1 - 0.08f, F_SIDES, seat); }
        }
        break;
    }
    case AX_VENT:
        abox(m, A, f->x0, f->z0, f->x1, f->z1, y0, y1, F_NOBOT, amat(A, ARS_METAL, 5, 0.0f));
        break;
    case AX_STEAM: {
        /* two crossed planes, seen from both sides */
        Mat st = amat(A, ARS_STEAM, 0, 0.0f);
        st.seed = f->seed;
        float cx = 0.5f * (f->x0 + f->x1), cz = 0.5f * (f->z0 + f->z1);
        for (int s = 0; s < 2; ++s) {
            awall(m, A, f->x0, cz, f->x1, cz, y0, y1, 0.0f, s ? 1.0f : -1.0f, 0.0f, 1.0f, 0.0f, 1.0f, st);
            awall(m, A, cx, f->z0, cx, f->z1, y0, y1, s ? 1.0f : -1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, st);
        }
        break;
    }
    case AX_PUDDLE: {
        float cx = 0.5f * (f->x0 + f->x1), cz = 0.5f * (f->z0 + f->z1), r = 0.5f * (f->x1 - f->x0);
        Mat pm = amat(A, ARS_PUDDLE, q_half(r), 0.0f);
        pm.seed = f->seed;
        float lx, lz;
        arcology_to_local(A, cx, cz, &lx, &lz);
        for (int k = 0; k < 8; ++k) {
            float a0 = (float)k * PI_F * 0.25f, a1 = (float)(k + 1) * PI_F * 0.25f;
            float w0 = 1.0f + 0.25f * hash_float(hash_combine(f->seed, (uint32_t)k)), w1 = 1.0f + 0.25f * hash_float(hash_combine(f->seed, (uint32_t)(k + 1) & 7u));
            float uv[3][2] = { { 0, 0 }, { cosf(a0) * r * w0, sinf(a0) * r * w0 }, { cosf(a1) * r * w1, sinf(a1) * r * w1 } };
            emit_tri(m, v3(lx, fy + 0.03f, lz), v3(lx + uv[1][0], fy + 0.03f, lz + uv[1][1]), v3(lx + uv[2][0], fy + 0.03f, lz + uv[2][1]), v3(0, 1, 0), uv, pm);
        }
        break;
    }
    case AX_KIOSK: {
        Mat k = amat(A, ARS_KIOSK, 0, 0.0f);
        k.seed = f->seed;
        abox(m, A, f->x0, f->z0, f->x1, f->z1, y0, y1, F_SIDES, k);
        abox(m, A, f->x0 - 0.3f, f->z0 - 0.3f, f->x1 + 0.3f, f->z1 + 0.3f, y1, y1 + 0.18f, F_NOBOT | F_BOT, amat(A, ARS_METAL, 3, 0.0f));
        Mat sg = mat(MAT_NEON, f->seed, (uint8_t)(A->palette[f->seed % 3] | (1 << 3)), q_half(f->x1 - f->x0 - 0.2f), q_half(0.5f));
        if (f->facing == 0) {
            sg.p[2] = q_half(f->x1 - f->x0 - 0.2f);
            asign_quad(m, A, f->x0 + 0.1f, f->z0 - 0.31f, f->x1 - 0.1f, f->z0 - 0.31f, y1 + 0.2f, y1 + 0.7f, 0.0f, -1.0f, sg);
            asign_quad(m, A, f->x0 + 0.1f, f->z1 + 0.31f, f->x1 - 0.1f, f->z1 + 0.31f, y1 + 0.2f, y1 + 0.7f, 0.0f, 1.0f, sg);
        } else {
            sg.p[2] = q_half(f->z1 - f->z0 - 0.2f);
            asign_quad(m, A, f->x0 - 0.31f, f->z0 + 0.1f, f->x0 - 0.31f, f->z1 - 0.1f, y1 + 0.2f, y1 + 0.7f, -1.0f, 0.0f, sg);
            asign_quad(m, A, f->x1 + 0.31f, f->z0 + 0.1f, f->x1 + 0.31f, f->z1 - 0.1f, y1 + 0.2f, y1 + 0.7f, 1.0f, 0.0f, sg);
        }
        break;
    }
    default: {   /* crates, bins, plant, tanks */
        Mat c = amat(A, ARS_CRATE, f->type, 0.0f);
        c.seed = f->seed;
        abox(m, A, f->x0, f->z0, f->x1, f->z1, y0, y1, F_NOBOT, c);
        if (f->type == AX_PLANT) abox(m, A, f->x0 + 0.15f, f->z0 + 0.15f, f->x1 - 0.15f, f->z1 - 0.15f, y1, y1 + 0.08f, F_NOBOT, amat(A, ARS_METAL, 5, 0.0f));
        break;
    }
    }
}

static void emit_arc_signs(Mesh *m, const ALayout *A) {
    for (int s = 0; s < A->nsign; ++s) {
        const ASign *g = &A->sign[s];
        float w = sqrtf((g->x1 - g->x0) * (g->x1 - g->x0) + (g->z1 - g->z0) * (g->z1 - g->z0)), h = g->y1 - g->y0;
        if (w < 0.2f || h < 0.2f) continue;
        float t = arcology_t(A, g->y0);
        Mat back = amat(A, ARS_METAL, 7, t);
        if (g->kind == AG_SCREEN) {
            Mat sc = mat(MAT_SCREEN, g->seed, 0, q_half(w), q_half(h));
            asign_quad(m, A, g->x0, g->z0, g->x1, g->z1, g->y0, g->y1, (float)g->nx, (float)g->nz, sc);
            /* a frame standing behind it */
            float x0 = minf(g->x0, g->x1), x1 = maxf(g->x0, g->x1), z0 = minf(g->z0, g->z1), z1 = maxf(g->z0, g->z1);
            if (g->nx) { float x = g->x0, b = x - (float)g->nx * 0.25f; x0 = minf(x, b) + (g->nx < 0 ? 0.01f : -0.01f); x1 = maxf(x, b) + (g->nx < 0 ? 0.01f : -0.01f); z0 -= 0.3f; z1 += 0.3f; }
            else       { float z = g->z0, b = z - (float)g->nz * 0.25f; z0 = minf(z, b) + (g->nz < 0 ? 0.01f : -0.01f); z1 = maxf(z, b) + (g->nz < 0 ? 0.01f : -0.01f); x0 -= 0.3f; x1 += 0.3f; }
            abox(m, A, x0, z0, x1, z1, g->y0 - 0.3f, g->y1 + 0.3f, F_SIDES | F_TOP | F_BOT, back);
            continue;
        }
        Mat nm = mat(MAT_NEON, g->seed, (uint8_t)((g->color & 7) | ((g->style & 7) << 3) | ((g->flick & 3) << 6)), q_half(w), q_half(h));
        if (g->kind == AG_BLADE) {
            /* a board projecting off a column face, lit on both sides */
            float dx = (g->x1 - g->x0) / w, dz = (g->z1 - g->z0) / w, px = -dz, pz = dx, th = 0.07f;
            abox(m, A, minf(g->x0, g->x1) - fabsf(px) * th, minf(g->z0, g->z1) - fabsf(pz) * th, maxf(g->x0, g->x1) + fabsf(px) * th, maxf(g->z0, g->z1) + fabsf(pz) * th,
                 g->y0 - 0.05f, g->y1 + 0.05f, F_NOBOT | F_BOT, back);
            for (int sd = -1; sd <= 1; sd += 2) {
                float o = (th + 0.01f) * (float)sd;
                asign_quad(m, A, g->x0 + px * o, g->z0 + pz * o, g->x1 + px * o, g->z1 + pz * o, g->y0, g->y1, px * (float)sd, pz * (float)sd, nm);
            }
            continue;
        }
        /* boards: a dark backing and the neon on its face */
        float nx = (float)g->nx, nz = (float)g->nz;
        asign_quad(m, A, g->x0, g->z0, g->x1, g->z1, g->y0, g->y1, nx, nz, nm);
        asign_quad(m, A, g->x0 - nx * 0.06f, g->z0 - nz * 0.06f, g->x1 - nx * 0.06f, g->z1 - nz * 0.06f, g->y0 - 0.04f, g->y1 + 0.04f, -nx, -nz, back);
        if (g->kind == AG_BANNER) {   /* hangers up to the slab edge */
            float y = g->y1 + 0.45f;
            abox(m, A, g->x0 - nx * 0.06f - 0.02f, g->z0 - nz * 0.06f - 0.02f, g->x0 - nx * 0.06f + 0.02f, g->z0 - nz * 0.06f + 0.02f, g->y1, y, F_SIDES, back);
            abox(m, A, g->x1 - nx * 0.06f - 0.02f, g->z1 - nz * 0.06f - 0.02f, g->x1 - nx * 0.06f + 0.02f, g->z1 - nz * 0.06f + 0.02f, g->y1, y, F_SIDES, back);
        }
    }
}

static void emit_arc_retrofit(Mesh *m, const ALayout *A) {
    float top = A->ceilY - 0.4f;
    for (int k = 0; k < A->nduct; ++k) {
        const ADuct *d = &A->duct[k];
        int alongX = fabsf(d->z1 - d->z0) < 1e-3f;
        float len = alongX ? d->x1 - d->x0 : d->z1 - d->z0, hs = d->size * 0.5f, t = arcology_t(A, d->y);
        Mat mt = amat(A, ARS_METAL, d->kind, t);
        float th = d->kind == AD_TRAY ? 0.12f : d->size;
        if (alongX) abox(m, A, d->x0, d->z0 - hs, d->x1, d->z0 + hs, d->y, d->y + th, F_NOBOT | F_BOT, mt);
        else        abox(m, A, d->x0 - hs, d->z0, d->x0 + hs, d->z1, d->y, d->y + th, F_NOBOT | F_BOT, mt);
        if (d->kind == AD_PIPE) continue;
        /* flanges on ducts, hangers up to the ceiling */
        float step = d->kind == AD_DUCT ? 3.0f : 2.4f;
        for (float a = 0.8f; a < len - 0.4f; a += step) {
            float x = alongX ? d->x0 + a : d->x0, z = alongX ? d->z0 : d->z0 + a;
            if (d->kind == AD_DUCT) {
                float f = hs + 0.06f;
                if (alongX) abox(m, A, x - 0.06f, z - f, x + 0.06f, z + f, d->y - 0.06f, d->y + th + 0.06f, F_NOBOT | F_BOT, mt);
                else        abox(m, A, x - f, z - 0.06f, x + f, z + 0.06f, d->y - 0.06f, d->y + th + 0.06f, F_NOBOT | F_BOT, mt);
            }
            if (top - (d->y + th) > 0.1f) {
                float o = hs - 0.05f;
                if (alongX) { abox(m, A, x - 0.025f, z - o - 0.025f, x + 0.025f, z - o + 0.025f, d->y + th, top, F_SIDES, mt);
                              abox(m, A, x - 0.025f, z + o - 0.025f, x + 0.025f, z + o + 0.025f, d->y + th, top, F_SIDES, mt); }
                else        { abox(m, A, x - o - 0.025f, z - 0.025f, x - o + 0.025f, z + 0.025f, d->y + th, top, F_SIDES, mt);
                              abox(m, A, x + o - 0.025f, z - 0.025f, x + o + 0.025f, z + 0.025f, d->y + th, top, F_SIDES, mt); }
            }
        }
    }
    /* sodium lamps on drop rods, under the ducts where a duct runs */
    Mat metal = amat(A, ARS_METAL, 6, 0.0f), head = mat(MAT_EMISSIVE, 0, 0, 2, 0);
    for (int k = 0; k < A->nlamp; ++k) {
        const ALamp *l = &A->lamp[k];
        float rodTop = top;
        for (int d = 0; d < A->nduct; ++d) {
            const ADuct *dt = &A->duct[d];
            if (dt->kind != AD_DUCT) continue;
            int alongX = fabsf(dt->z1 - dt->z0) < 1e-3f;
            int over = alongX ? fabsf(l->z - dt->z0) < dt->size * 0.5f && l->x >= dt->x0 && l->x <= dt->x1
                              : fabsf(l->x - dt->x0) < dt->size * 0.5f && l->z >= dt->z0 && l->z <= dt->z1;
            if (over) rodTop = minf(rodTop, dt->y);
        }
        abox(m, A, l->x - 0.03f, l->z - 0.03f, l->x + 0.03f, l->z + 0.03f, l->y + 0.25f, rodTop, F_SIDES, metal);
        abox(m, A, l->x - 0.38f, l->z - 0.24f, l->x + 0.38f, l->z + 0.24f, l->y + 0.06f, l->y + 0.26f, F_NOBOT | F_BOT, metal);
        abox(m, A, l->x - 0.28f, l->z - 0.15f, l->x + 0.28f, l->z + 0.15f, l->y, l->y + 0.06f, F_SIDES | F_BOT, head);
    }
}

/* The gallery strips round void v: side 0..3 (-z, +x, +z, -x); none against the spine. */
static int arc_side_open(const ALayout *A, int v, int side) {
    ARect r = A->voids[v];
    float mx = (side == 0 || side == 2) ? 0.5f * (r.x0 + r.x1) : side == 1 ? r.x1 : r.x0;
    float mz = (side == 1 || side == 3) ? 0.5f * (r.z0 + r.z1) : side == 0 ? r.z0 : r.z1;
    if (A->canyon == AC_SPINE) return 1;
    ARect s = A->spine;
    return !(mx >= s.x0 - 0.1f && mx <= s.x1 + 0.1f && mz >= s.z0 - 0.1f && mz <= s.z1 + 0.1f);
}

static void emit_arc_galleries(Mesh *m, const ALayout *A) {
    if (!A->nvoid) return;
    float gd = A->galleryD, slab = 0.4f;
    for (int v = 0; v < A->nvoid; ++v) {
        ARect r = A->voids[v];
        int open[4];
        for (int s = 0; s < 4; ++s) open[s] = arc_side_open(A, v, s);
        for (int side = 0; side < 4; ++side) {
            if (!open[side]) continue;
            ARect strip;
            float fl, bl;                 /* fascia line, back line */
            float a0, a1, b0, b1;         /* fascia extent; back wall extent */
            float nx = 0, nz = 0;         /* into the void */
            if (side == 0)      { strip = (ARect){ r.x0 - gd, r.z0 - gd, r.x1 + gd, r.z0 }; fl = r.z0; bl = r.z0 - gd; nz = 1; a0 = r.x0; a1 = r.x1; b0 = r.x0 - gd; b1 = r.x1 + gd; }
            else if (side == 2) { strip = (ARect){ r.x0 - gd, r.z1, r.x1 + gd, r.z1 + gd }; fl = r.z1; bl = r.z1 + gd; nz = -1; a0 = r.x0; a1 = r.x1; b0 = r.x0 - gd; b1 = r.x1 + gd; }
            else {
                float z0 = open[0] ? r.z0 : r.z0 - gd, z1 = open[2] ? r.z1 : r.z1 + gd;
                if (side == 1) { strip = (ARect){ r.x1, z0, r.x1 + gd, z1 }; fl = r.x1; bl = r.x1 + gd; nx = -1; }
                else           { strip = (ARect){ r.x0 - gd, z0, r.x0, z1 }; fl = r.x0; bl = r.x0 - gd; nx = 1; }
                a0 = r.z0; a1 = r.z1; b0 = r.z0 - gd; b1 = r.z1 + gd;
            }
            int alongX = side == 0 || side == 2;
            for (int k = 0; k < A->nlevel; ++k) {
                float y = A->levelY[k], yb = y - slab, t = arcology_t(A, y);
                int st = arcology_stratum(A, t);
                float yTop = k + 1 < A->nlevel ? A->levelY[k + 1] - slab : A->crownY;
                aflat(m, A, strip, y, 0, NULL, iframe(m, amat(A, ARS_GALLERY, st, t)));
                if (k > 0) aflat(m, A, strip, yb, 1, NULL, iframe(m, amat(A, ARS_SOFFIT, st, t)));
                /* the slab edge, and a light strip on it in the lower strata */
                Mat fas = amat(A, ARS_FASCIA, st, t);
                if (alongX) awall(m, A, a0, fl, a1, fl, yb, y, nx, nz, a0, a1, 0.0f, slab, fas);
                else        awall(m, A, fl, a0, fl, a1, yb, y, nx, nz, a0, a1, 0.0f, slab, fas);
                /* light strips on some slab edges: neon in the street stratum, cold white in habitat */
                uint32_t hk = hash_combine(A->seed ^ 0x1ED5u, (uint32_t)(k * 8 + side + v * 4));
                int strip = st == AS_STREET ? hash_float(hk) < 0.7f : st == AS_HABITAT ? hash_float(hk) < 0.35f : 0;
                if (strip) {
                    uint8_t col = st == AS_STREET ? A->palette[(hk >> 8) % 3] : 6;
                    if (st == AS_STREET && hash_float(hk >> 3) < 0.25f) col = (uint8_t)((hk >> 12) & 7);
                    Mat led = mat(MAT_LEDSTRIP, A->seed, col, (uint8_t)(st == AS_STREET && hash_float(hk >> 5) < 0.15f), 0);
                    float e = 0.012f;
                    if (alongX) awall(m, A, a0, fl + nz * e, a1, fl + nz * e, yb + 0.1f, yb + 0.22f, nx, nz, 0.0f, a1 - a0, 0.0f, 1.0f, led);
                    else        awall(m, A, fl + nx * e, a0, fl + nx * e, a1, yb + 0.1f, yb + 0.22f, nx, nz, 0.0f, a1 - a0, 0.0f, 1.0f, led);
                }
                /* the balustrade (glass; a solid parapet in the corporate stratum), gapped for bridges */
                float gaps[8][2];
                int ng = 0;
                for (int b = 0; b < A->nbridge && ng < 8; ++b) {
                    const ABridge *br = &A->bridge[b];
                    if (br->level != k) continue;
                    ARect q = br->r;
                    int touches = side == 0 ? (q.z0 <= fl + 0.01f && q.z1 >= fl - 0.01f && br->axis == 1) : side == 2 ? (q.z1 >= fl - 0.01f && q.z0 <= fl + 0.01f && br->axis == 1)
                                : side == 1 ? (q.x1 >= fl - 0.01f && q.x0 <= fl + 0.01f && br->axis == 0) : (q.x0 <= fl + 0.01f && q.x1 >= fl - 0.01f && br->axis == 0);
                    if (!touches) continue;
                    gaps[ng][0] = alongX ? q.x0 : q.z0; gaps[ng][1] = alongX ? q.x1 : q.z1; ng++;
                }
                float rail = 1.05f, ri = 0.08f;
                Mat glass = amat(A, ARS_RAIL, st == AS_CORPORATE ? 2 : 0, t), top = amat(A, ARS_RAIL, 1, t);
                float cur = a0;
                for (int q = 0; q <= ng; ++q) {
                    float s0 = cur, s1 = q < ng ? gaps[q][0] : a1;
                    if (q < ng) cur = gaps[q][1];
                    if (s1 - s0 < 0.1f) continue;
                    float L = fl + (alongX ? nz : nx) * ri;
                    for (int fs = 0; fs < 2; ++fs) {
                        float sx = fs ? -1.0f : 1.0f;
                        if (alongX) awall(m, A, s0, L, s1, L, y, y + rail, nx * sx, nz * sx, s0, s1, 0.0f, rail, glass);
                        else        awall(m, A, L, s0, L, s1, y, y + rail, nx * sx, nz * sx, s0, s1, 0.0f, rail, glass);
                    }
                    if (alongX) abox(m, A, s0, L - 0.03f, s1, L + 0.03f, y + rail, y + rail + 0.05f, F_TOP | (nz > 0 ? F_PZ : F_NZ), top);
                    else        abox(m, A, L - 0.03f, s0, L + 0.03f, s1, y + rail, y + rail + 0.05f, F_TOP | (nx > 0 ? F_PX : F_NX), top);
                }
                /* the back wall: unit fronts (shopfronts, flat doors, glass) at the fine window pitch */
                Mat units = amat(A, ARS_UNITS, st, t);
                if (alongX) awall(m, A, b0, bl, b1, bl, y, yTop, nx, nz, b0, b1, 0.0f, yTop - y, units);
                else        awall(m, A, bl, b0, bl, b1, y, yTop, nx, nz, b0, b1, 0.0f, yTop - y, units);
                /* end walls where the gallery stops at the spine */
                if (alongX) {
                    if (!open[1]) awall(m, A, r.x1 + gd, bl, r.x1 + gd, fl, y, yTop, -1.0f, 0.0f, 0.0f, gd, 0.0f, yTop - y, units);
                    if (!open[3]) awall(m, A, r.x0 - gd, bl, r.x0 - gd, fl, y, yTop, 1.0f, 0.0f, 0.0f, gd, 0.0f, yTop - y, units);
                } else {
                    if (!open[0]) awall(m, A, bl, r.z0 - gd, fl, r.z0 - gd, y, yTop, 0.0f, 1.0f, 0.0f, gd, 0.0f, yTop - y, units);
                    if (!open[2]) awall(m, A, bl, r.z1 + gd, fl, r.z1 + gd, y, yTop, 0.0f, -1.0f, 0.0f, gd, 0.0f, yTop - y, units);
                }
            }
            /* closed over the top level, at the crown */
            aflat(m, A, strip, A->crownY, 1, NULL, iframe(m, amat(A, ARS_SOFFIT, AS_CORPORATE, arcology_t(A, A->crownY))));
        }
    }
    /* bridges */
    for (int b = 0; b < A->nbridge; ++b) {
        const ABridge *br = &A->bridge[b];
        float y = A->levelY[br->level], t = arcology_t(A, y);
        int st = arcology_stratum(A, t);
        ARect q = br->r;
        abox_r(m, A, q, y - 0.35f, y, F_TOP | F_BOT | (br->axis ? (F_NX | F_PX) : (F_NZ | F_PZ)), amat(A, ARS_BRIDGE, st, t));
        Mat glass = amat(A, ARS_RAIL, st == AS_CORPORATE ? 2 : 0, t), top = amat(A, ARS_RAIL, 1, t);
        for (int s = 0; s < 2; ++s) {
            float L = br->axis ? (s ? q.x1 - 0.06f : q.x0 + 0.06f) : (s ? q.z1 - 0.06f : q.z0 + 0.06f);
            for (int fs = 0; fs < 2; ++fs) {
                float sg = fs ? -1.0f : 1.0f;
                if (br->axis) awall(m, A, L, q.z0, L, q.z1, y, y + 1.05f, sg, 0.0f, q.z0, q.z1, 0.0f, 1.05f, glass);
                else          awall(m, A, q.x0, L, q.x1, L, y, y + 1.05f, 0.0f, sg, q.x0, q.x1, 0.0f, 1.05f, glass);
            }
            if (br->axis) abox(m, A, L - 0.03f, q.z0, L + 0.03f, q.z1, y + 1.05f, y + 1.1f, F_TOP | F_NX | F_PX, top);
            else          abox(m, A, q.x0, L - 0.03f, q.x1, L + 0.03f, y + 1.05f, y + 1.1f, F_TOP | F_NZ | F_PZ, top);
            if (st <= AS_HABITAT) {
                Mat led = mat(MAT_LEDSTRIP, A->seed, A->palette[(br->level + s) % 3], 0, 0);
                if (br->axis) awall(m, A, s ? q.x1 + 0.01f : q.x0 - 0.01f, q.z0, s ? q.x1 + 0.01f : q.x0 - 0.01f, q.z1, y - 0.3f, y - 0.2f, s ? 1.0f : -1.0f, 0.0f, 0.0f, q.z1 - q.z0, 0.0f, 1.0f, led);
                else          awall(m, A, q.x0, s ? q.z1 + 0.01f : q.z0 - 0.01f, q.x1, s ? q.z1 + 0.01f : q.z0 - 0.01f, y - 0.3f, y - 0.2f, 0.0f, s ? 1.0f : -1.0f, 0.0f, q.x1 - q.x0, 0.0f, 1.0f, led);
            }
        }
    }
}

/* Frame point in the chunk's space (through the mesh's emission transform), for vertex-animated geometry. */
static void chunk_xz(const Mesh *m, float x, float z, float *cx, float *cz) {
    if (m->xf) { *cx = m->xx + m->xc * x - m->xs * z; *cz = m->xz + m->xs * x + m->xc * z; }
    else       { *cx = x; *cz = z; }
}

/* A searchlight beam: a tapered box hanging straight down from its pivot,
 * swung to and fro along the canyon by the vertex shader. uv: the pivot less
 * the vertex (chunk xz); aux: how far along the beam | the drop below the
 * pivot << 16 (1/64 m); params z its swing direction in the chunk (of 2 pi),
 * w its phase; seed: amplitude | speed << 8. */
static void emit_arc_searchlight(Mesh *m, const ALayout *A, const ASearch *s) {
    float px, pz, dx, dz;
    arcology_to_local(A, s->x, s->z, &px, &pz);
    arcology_to_local(A, cosf(s->az), sinf(s->az), &dx, &dz);
    float cdx = m->xf ? m->xc * dx - m->xs * dz : dx, cdz = m->xf ? m->xs * dx + m->xc * dz : dz;
    float az = atan2f(cdz, cdx);
    if (az < 0.0f) az += 6.2831853f;
    vec3 P = v3(px, s->y, pz), D = v3(0, -1, 0);
    vec3 U = v3_norm(v3(dx, 0, dz)), W = v3_norm(v3_cross(U, D));
    float r0 = 0.3f, r1 = s->len * 0.06f, a0 = 0.8f, a1 = s->len;
    vec3 c[2][4];
    for (int e = 0; e < 2; ++e) {
        float a = e ? a1 : a0, r = e ? r1 : r0;
        vec3 o = v3_add(P, v3_scale(D, a));
        c[e][0] = v3_add(o, v3_add(v3_scale(U, -r), v3_scale(W, -r)));
        c[e][1] = v3_add(o, v3_add(v3_scale(U, r), v3_scale(W, -r)));
        c[e][2] = v3_add(o, v3_add(v3_scale(U, r), v3_scale(W, r)));
        c[e][3] = v3_add(o, v3_add(v3_scale(U, -r), v3_scale(W, r)));
    }
    float cpx, cpz;
    chunk_xz(m, P.x, P.z, &cpx, &cpz);
    uint8_t prm[4] = { MAT_ARCOLOGY, ARS_SEARCH, (uint8_t)(az / 6.2831853f * 255.0f + 0.5f), (uint8_t)(s->phase / 6.2831853f * 255.0f) };
    uint32_t seed = (uint32_t)clampf(s->amp / 0.6f * 255.0f + 0.5f, 0.0f, 255.0f) | (uint32_t)clampf((s->speed - 0.1f) / 0.3f * 255.0f + 0.5f, 0.0f, 255.0f) << 8;
    seed |= hash_combine(A->seed, (uint32_t)(s->phase * 1000.0f)) & 0xFFFF0000u;
    for (int f = 0; f < 4; ++f) {
        vec3 q[4] = { c[0][f], c[0][(f + 1) & 3], c[1][(f + 1) & 3], c[1][f] };
        uint32_t along[4] = { 0u, 0u, 65535u, 65535u };
        vec3 n = v3_norm(v3_cross(v3_sub(q[1], q[0]), v3_sub(q[3], q[0])));
        static const int TRI[2][6] = { { 0, 1, 2, 0, 2, 3 }, { 0, 2, 1, 0, 3, 2 } };
        for (int side = 0; side < 2; ++side)
            for (int i = 0; i < 6; ++i) {
                int k = TRI[side][i];
                float vx, vz;
                chunk_xz(m, q[k].x, q[k].z, &vx, &vz);
                uint32_t drop = (uint32_t)clampf((P.y - q[k].y) * 64.0f + 0.5f, 0.0f, 65535.0f);
                mesh_push(m, q[k], side ? v3_scale(n, -1.0f) : n, cpx - vx, cpz - vz, seed, prm, along[k] | drop << 16);
            }
    }
}

/* The light crown: glazing over the canyon on a grid of transfer beams, cold
 * shafts falling from it and the searchlights hung under it. */
static void emit_arc_crown(Mesh *m, const ALayout *A) {
    if (!A->nvoid) return;
    float y = A->crownY, t = arcology_t(A, y);
    Mat glass = amat(A, ARS_CROWN, 0, t), beam = amat(A, ARS_BEAM, 2, t);
    ARect s = A->spine;
    for (int v = 0; v < A->nvoid; ++v) {
        ARect r = A->voids[v];
        if (A->canyon == AC_SPINE) {
            aflat(m, A, (ARect){ r.x0, r.z0, r.x1, s.z0 }, y - 0.05f, 1, NULL, glass);
            aflat(m, A, (ARect){ r.x0, s.z1, r.x1, r.z1 }, y - 0.05f, 1, NULL, glass);
            aflat(m, A, (ARect){ r.x0, s.z0, s.x0, s.z1 }, y - 0.05f, 1, NULL, glass);
            aflat(m, A, (ARect){ s.x1, s.z0, r.x1, s.z1 }, y - 0.05f, 1, NULL, glass);
        } else {
            aflat(m, A, r, y - 0.05f, 1, NULL, glass);
        }
        for (float x = r.x0 + 4.0f; x < r.x1 - 2.0f; x += 7.0f)
            if (A->canyon != AC_SPINE || x < s.x0 - 0.6f || x > s.x1 + 0.6f) abox(m, A, x - 0.45f, r.z0, x + 0.45f, r.z1, y - 1.8f, y - 0.05f, F_SIDES | F_BOT, beam);
        for (float z = r.z0 + 4.0f; z < r.z1 - 2.0f; z += 7.0f)
            if (A->canyon != AC_SPINE || z < s.z0 - 0.6f || z > s.z1 + 0.6f) abox(m, A, r.x0, z - 0.35f, r.x1, z + 0.35f, y - 1.4f, y - 0.05f, F_SIDES | F_BOT, beam);
    }
    for (int k = 0; k < A->nshaft; ++k) {
        /* uv: 0..1 across each face (so its edges can fade), 0 at the floor to 1 at the crown */
        const AShaft *sh = &A->shaft[k];
        Mat mt = amat(A, ARS_SHAFT, 0, t);
        mt.seed = hash_combine(A->seed, (uint32_t)k);
        ARect q = sh->r;
        for (int fs = 0; fs < 2; ++fs) {
            float sg = fs ? -1.0f : 1.0f;
            awall(m, A, q.x0, q.z0, q.x1, q.z0, sh->y0, sh->y1, 0.0f, -sg, 0.0f, 1.0f, 0.0f, 1.0f, mt);
            awall(m, A, q.x0, q.z1, q.x1, q.z1, sh->y0, sh->y1, 0.0f, sg, 0.0f, 1.0f, 0.0f, 1.0f, mt);
            awall(m, A, q.x0, q.z0, q.x0, q.z1, sh->y0, sh->y1, -sg, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, mt);
            awall(m, A, q.x1, q.z0, q.x1, q.z1, sh->y0, sh->y1, sg, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, mt);
        }
    }
    for (int k = 0; k < A->nsearch; ++k) {
        const ASearch *sl = &A->search[k];
        abox(m, A, sl->x - 0.5f, sl->z - 0.5f, sl->x + 0.5f, sl->z + 0.5f, sl->y - 0.3f, y - 0.05f, F_SIDES | F_BOT, beam);
        emit_arc_searchlight(m, A, sl);
    }
}

/* The interior of an arcology, for the near-only layer (city_build_interiors). */
void emit_arcology(Mesh *m, const BGraph *g, const ALayout *A) {
    float fy = A->floorY, top = A->ceilY - 0.4f;
    emit_arc_floor(m, g, A);
    emit_arc_ceiling(m, g, A);
    emit_arc_envelope(m, g, A);
    for (int c = 0; c < A->ncol; ++c) emit_arc_column(m, A, &A->col[c]);

    /* the spine core, a segment per tier so its finish follows the strata, with corner light bars */
    float spineTop = A->nvoid ? A->crownY : top;
    for (int t = 0; t < A->ntier; ++t) {
        float y0 = t ? A->tier[t].y0 : fy, y1 = minf(A->tier[t].y1, spineTop);
        if (y1 <= y0) break;
        abox_r(m, A, A->spine, y0, y1, F_SIDES, amat(A, ARS_SPINE, arcology_stratum(A, A->tier[t].t), A->tier[t].t));
    }
    for (int c = 0; c < 4; ++c) {
        float x = (c & 1) ? A->spine.x1 : A->spine.x0, z = (c & 2) ? A->spine.z1 : A->spine.z0;
        abox(m, A, x - 0.12f, z - 0.12f, x + 0.12f, z + 0.12f, fy + 0.4f, spineTop, F_SIDES, mat(MAT_LEDSTRIP, A->seed, A->palette[0], 1, 0));
    }
    emit_arc_lifts(m, A, A->spine, 0, A->expressLifts, 1.4f, 3.0f, 0);
    /* local cores with their lifts, escape stairs with an exit */
    for (int k = 0; k < A->ncore; ++k) {
        const ACore *c = &A->core[k];
        abox_r(m, A, c->r, fy, top, F_SIDES, amat(A, ARS_CORE, c->topTier, 0.0f));
        emit_arc_lifts(m, A, c->r, c->door, c->lifts, 1.1f, 2.4f, 1);
    }
    for (int k = 0; k < A->nstair; ++k) {
        const AStair *s = &A->stair[k];
        abox_r(m, A, s->r, fy, top, F_SIDES, amat(A, ARS_STAIR, 0, 0.0f));
        emit_arc_lifts(m, A, s->r, s->facing, 1, 1.2f, 2.2f, 2);
    }
    emit_arc_blocks(m, A);
    for (int s = 0; s < A->nstall; ++s) emit_arc_stall(m, A, &A->stall[s]);
    for (int f = 0; f < A->nfix; ++f) emit_arc_fixture(m, A, &A->fix[f]);
    emit_arc_retrofit(m, A);
    emit_arc_signs(m, A);
    emit_arc_galleries(m, A);
    emit_arc_crown(m, A);
}

/* The outside of the portals (base layer, seen from the street): stepped
 * surrounds, a light frame, a sign over each, a canopy over the main one. */
void emit_arc_portals(Mesh *m, const BGraph *g, const ALayout *A) {
    const BLoop *l = &g->sec[A->tier[0].section].foot.outer;
    const float gy = G0;
    for (int p = 0; p < A->nportal; ++p) {
        const APortal *pt = &A->portal[p];
        Xf sv = xf_save(m);
        float len = enter_face(m, l, pt->face);
        float u0 = pt->u - pt->width * 0.5f, u1 = pt->u + pt->width * 0.5f, top = gy + pt->height;
        uint32_t h = hash_combine(A->seed, (uint32_t)p);
        if (pt->service) {
            Mat steel = mat(MAT_METAL, g->seed, 3, 0, 0);
            emit_box(m, v3(u0 - 0.25f, gy, -0.4f), v3(u0, top + 0.25f, 0.15f), F_NOBOT, steel, UV_FACADE);
            emit_box(m, v3(u1, gy, -0.4f), v3(u1 + 0.25f, top + 0.25f, 0.15f), F_NOBOT, steel, UV_FACADE);
            emit_box(m, v3(u0, top, -0.4f), v3(u1, top + 0.25f, 0.15f), F_NOBOT | F_BOT, steel, UV_FACADE);
            emit_emissive_box(m, v3(pt->u, top + 0.5f, 0.3f), 0.15f, 0, 2, 0);
            xf_restore(m, sv);
            continue;
        }
        /* three nested frames stepping out and forward: the tiers again, in miniature */
        for (int s = 0; s < 3; ++s) {
            float w = 0.45f + 0.5f * (float)s, wi = s ? 0.45f + 0.5f * (float)(s - 1) : 0.0f;
            float zi = -0.7f + 0.15f * (float)s, zo = 0.45f - 0.12f * (float)s, up = 0.45f + 0.55f * (float)s;
            Mat fr = amat(A, ARS_PORTAL, s, 0.0f);
            emit_box(m, v3(u0 - w, gy, zi), v3(u0 - wi, top + up, zo), F_NOBOT, fr, UV_FACADE);
            emit_box(m, v3(u1 + wi, gy, zi), v3(u1 + w, top + up, zo), F_NOBOT, fr, UV_FACADE);
            emit_box(m, v3(u0 - wi, top + (s ? 0.45f + 0.55f * (float)(s - 1) : 0.0f), zi), v3(u1 + wi, top + up, zo), F_NOBOT | F_BOT, fr, UV_FACADE);
        }
        /* a light frame round the opening */
        Mat led = mat(MAT_LEDSTRIP, h, A->palette[p % 3], 0, 0);
        emit_box(m, v3(u0 - 0.06f, gy + 0.2f, 0.42f), v3(u0, top, 0.48f), F_SIDES, led, UV_FACADE);
        emit_box(m, v3(u1, gy + 0.2f, 0.42f), v3(u1 + 0.06f, top, 0.48f), F_SIDES, led, UV_FACADE);
        emit_box(m, v3(u0, top, 0.42f), v3(u1, top + 0.06f, 0.48f), F_SIDES | F_BOT, led, UV_FACADE);
        /* a sign over it */
        float sy0 = top + 1.75f, sy1 = minf(sy0 + 1.5f, gy + (A->ceilY - A->floorY) - 0.3f);
        if (sy1 - sy0 > 0.6f) {
            Face f = face_local(len);
            float w = minf(pt->width + 1.0f, len - 1.0f);
            emit_face_panel(m, &f, pt->u - w * 0.5f, pt->u + w * 0.5f, sy0, sy1, 0.5f,
                            mat(MAT_NEON, h, (uint8_t)(A->palette[(p + 1) % 3] | (1 << 3)), q_half(w), q_half(sy1 - sy0)));
        }
        if (pt->main) {
            Mat metal = mat(MAT_METAL, g->seed, 3, 0, 0);
            emit_box(m, v3(u0 - 1.8f, top + 1.0f, 0.0f), v3(u1 + 1.8f, top + 1.35f, 4.0f), F_NOBOT | F_BOT, metal, UV_FACADE);
            float uv[4][2] = { { 0, 0 }, { u1 - u0 + 3.4f, 0 }, { u1 - u0 + 3.4f, 1 }, { 0, 1 } };
            emit_quad(m, v3(u0 - 1.7f, top + 0.99f, 3.7f), v3(u1 + 1.7f, top + 0.99f, 3.7f), v3(u1 + 1.7f, top + 0.99f, 3.9f),
                      v3(u0 - 1.7f, top + 0.99f, 3.9f), v3(0, -1, 0), uv, mat(MAT_LEDSTRIP, h, A->palette[0], 1, 0));
        }
        xf_restore(m, sv);
    }
}

/* The arcology's street front (base layer): neon boards over the glazing
 * between the portals and blade signs off the facade above them, as dense as
 * the street stratum it belongs to. */
void emit_arc_frontage(Mesh *m, const BGraph *g, const ALayout *A) {
    int si = A->tier[0].section;
    const BLoop *l = &g->sec[si].foot.outer;
    Behind bh = { NULL, A, si };
    Rng r = rng_make(A->seed ^ 0xF20Eu);
    float top = A->ceilY;
    for (int k = 0; k < l->n; ++k) {
        float op[8][3];
        int nop = face_openings(g, &bh, 0, k, op, 8);
        Xf sv = xf_save(m);
        float len = enter_face(m, l, k);
        Face f = face_local(len);
        for (float u = 1.5f; u < len - 4.0f; u += rng_range(&r, 6.0f, 10.0f)) {
            float w = rng_range(&r, 3.0f, 6.0f);
            if (u + w > len - 1.0f) break;
            int clear = 1;
            for (int o = 0; o < nop && clear; ++o) clear = u + w < op[o][0] - 2.0f || u > op[o][1] + 2.0f;   /* the portals keep their own signs */
            if (!clear || rng_float(&r) > A->signDensity) continue;
            uint8_t col = rng_float(&r) < 0.7f ? A->palette[rng_int(&r, 0, 2)] : (uint8_t)rng_int(&r, 0, 7);
            uint32_t seed = rng_u32(&r);
            int flicker = rng_float(&r) < 0.1f;
            emit_face_panel(m, &f, u, u + w, top - 1.15f, top - 0.3f, 0.22f,
                            mat(MAT_NEON, seed, (uint8_t)(col | (1 << 3) | (flicker << 6)), q_half(w), q_half(0.85f)));
        }
        for (float u = 3.0f; u < len - 3.0f; u += rng_range(&r, 9.0f, 14.0f)) {
            if (rng_float(&r) > A->signDensity * 0.85f) continue;
            float h = rng_range(&r, 4.0f, 9.0f), y0 = top + rng_range(&r, 0.6f, 2.5f), depth = rng_range(&r, 1.2f, 2.0f), th = 0.12f;
            emit_box(m, v3(u - th, y0, 0.25f), v3(u + th, y0 + h, 0.25f + depth), F_NOBOT | F_BOT, mat(MAT_METAL, g->seed, 3, 0, 0), UV_FACADE);
            uint8_t col = rng_float(&r) < 0.7f ? A->palette[rng_int(&r, 0, 2)] : (uint8_t)rng_int(&r, 0, 7);
            uint8_t flick = (uint8_t)(rng_float(&r) < 0.15f ? 1 : 0);
            Mat nm = mat(MAT_NEON, rng_u32(&r), (uint8_t)(col | (flick << 6)), q_half(depth), q_half(h));
            float uvA[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } }, uvB[4][2] = { { 1, 0 }, { 0, 0 }, { 0, 1 }, { 1, 1 } };
            for (int sd = -1; sd <= 1; sd += 2) {
                float uu = u + (float)sd * (th + 0.01f);
                emit_quad(m, v3(uu, y0 + 0.1f, 0.3f), v3(uu, y0 + 0.1f, 0.2f + depth), v3(uu, y0 + h - 0.1f, 0.2f + depth), v3(uu, y0 + h - 0.1f, 0.3f),
                          v3((float)sd, 0, 0), sd < 0 ? uvB : uvA, nm);
            }
            sign_lights(m, v3(u, y0 + h * 0.5f, 0.25f + depth * 0.5f), v3(0, 0, 1), depth * h, nm);
        }
        xf_restore(m, sv);
    }
}

/* The tier decks seen from outside (base layer): each sky lobby as a lit
 * glazed band round its tier's first floor, and its terrace on the setback
 * roof below, with planters along the tier's foot and lamps out by the edge,
 * warmer and fewer the higher the stratum. */
void emit_arc_terraces(Mesh *m, const BGraph *g, const ALayout *A) {
    for (int k = 0; k < A->nsky; ++k) {
        const ASkyLobby *sk = &A->sky[k];
        int t = sk->tier, st = A->tier[t].stratum;
        const BSection *up = &g->sec[A->tier[t].section], *below = &g->sec[A->tier[t - 1].section];
        const BLoop *l = &up->foot.outer;
        float y0 = sk->y, tt = A->tier[t].t;
        Mat band = amat(A, ARS_SKYLOBBY, st, tt), planter = amat(A, ARS_CRATE, 12, tt), plant = amat(A, ARS_CRATE, 13, tt), post = amat(A, ARS_METAL, 3, tt);
        uint8_t lampCol = (uint8_t)(st == AS_HABITAT ? 6 : st == AS_COMMERCE ? 3 : st == AS_CORPORATE ? 7 : 0);
        float pitch = st == AS_CORPORATE ? 3.5f : st == AS_COMMERCE ? 5.0f : 7.0f;
        for (int f = 0; f < l->n; ++f) {
            EdgeInfo e = edge_of(l, f);
            if (e.len < 2.0f) continue;
            float ox = -e.ez, oz = e.ex;                    /* out of the building */
            Xf sv = xf_save(m);
            enter_face(m, l, f);
            float uv[4][2] = { { 0.3f, 0.4f }, { e.len - 0.3f, 0.4f }, { e.len - 0.3f, A->floorH - 0.5f }, { 0.3f, A->floorH - 0.5f } };
            emit_quad(m, v3(0.3f, y0 + 0.4f, 0.07f), v3(e.len - 0.3f, y0 + 0.4f, 0.07f), v3(e.len - 0.3f, y0 + A->floorH - 0.5f, 0.07f),
                      v3(0.3f, y0 + A->floorH - 0.5f, 0.07f), v3(0, 0, 1), uv, band);
            for (float u = pitch * 0.5f; u < e.len - 1.0f; u += pitch) {
                /* on the terrace: out over the roof of the tier below, clear of its edge */
                float px = e.ax + e.ex * u + ox * 1.4f, pz = e.az + e.ez * u + oz * 1.4f;
                if (!bfoot_contains(&below->foot, px, pz) || bfoot_boundary_dist(&below->foot, px, pz, NULL, NULL) < 1.2f || bfoot_contains(&up->foot, px, pz)) continue;
                emit_box(m, v3(u - 0.8f, y0, 1.0f), v3(u + 0.8f, y0 + 0.65f, 1.7f), F_NOBOT, planter, UV_FACADE);
                emit_box(m, v3(u - 0.7f, y0 + 0.65f, 1.1f), v3(u + 0.7f, y0 + 0.65f + (st == AS_CORPORATE ? 1.1f : 0.6f), 1.6f), F_NOBOT, plant, UV_FACADE);
                float qx = e.ax + e.ex * u + ox * 3.6f, qz = e.az + e.ez * u + oz * 3.6f;
                if (((int)(u / pitch) & 1) || !bfoot_contains(&below->foot, qx, qz) || bfoot_boundary_dist(&below->foot, qx, qz, NULL, NULL) < 0.8f) continue;
                emit_box(m, v3(u - 0.05f, y0, 3.55f), v3(u + 0.05f, y0 + 2.6f, 3.65f), F_SIDES, post, UV_FACADE);
                emit_emissive_box(m, v3(u, y0 + 2.75f, 3.6f), 0.14f, lampCol, 0, 0);
            }
            xf_restore(m, sv);
        }
    }
}
