#include "city_internal.h"

/* ===========================================================================
 * Overhead cables: power and data lines strung across streets from facade to
 * facade (singly or in loose bundles, now and then high between towers), from
 * lamp to lamp along the sidewalk, from lamps up to the wall behind them, and
 * criss-crossing the alleys and courtyards between buildings.
 *
 * Street stations are spaced along each road by a stream that depends on the
 * road alone, alley stations along each wall by a stream that depends on that
 * wall alone, and each station rolls its own choices from its own hash, so a
 * cable comes out the same from whichever chunk it is generated. A crossing
 * belongs to the chunk holding its station on the centre line, a lamp line to
 * the chunk holding its first lamp, an alley cable to its first building's.
 *
 * Ends are found by casting horizontally into the building graphs, so every
 * cable ends on a wall, and never on glass: an end slides along the wall it
 * hit to the nearest spot the facade shader draws solid (the band between two
 * rows of windows, the pier between two bays, the pier between two shopfronts
 * or a lobby mullion), clear of door canopies and the neon in the windows.
 *
 * Heights passed between buildings are world Y; each building's walls are
 * tested in its own frame, from its base (building_base). Clearances count
 * from the ground under the cable (city_ground_y).
 * ========================================================================= */

#define CABLE_CLEAR 5.0f  /* lowest a crossing sags: clear of trucks (and of lamps, checked apart) */
#define ALLEY_CLEAR 3.4f   /* lowest an alley cable sags: clear of heads */
#define STREET_GAP  0.12f  /* solid wall needed round an end over a street (the bracket and a little)... */
#define ALLEY_GAP_U 0.3f   /* ...and in an alley, either side, where a skewed cable runs close along the wall... */
#define ALLEY_GAP_V 0.15f  /* ...and above and below, where it leaves the wall near level */

typedef struct Cable { vec3 a, b; float sag, thick; } Cable;
/* distance (< 0: none), building, section, loop (1: courtyard), edge, metres along the edge */
typedef struct WallHit { double t; int b, s, li, k; float u; } WallHit;

static int slab(double o, double u, double lo, double hi, double *t0, double *t1) {
    if (fabs(u) < 1e-12) return o >= lo && o <= hi;
    double a = (lo - o) / u, b = (hi - o) / u;
    if (a > b) { double t = a; a = b; b = t; }
    *t0 = fmax(*t0, a); *t1 = fmin(*t1, b);
    return *t0 <= *t1;
}

/* The nearest wall standing at height y (world) along a horizontal ray from p
 * (world, unit dir), within maxT: outer walls and courtyard walls. */
static WallHit facade_ray(ChunkGen *g, DVec2 p, DVec2 dir, float y, double maxT) {
    WallHit h = { -1.0, -1, -1, 0, 0, 0.0f };
    double best = maxT;
    int back = 0;
    for (int i = 0; i < g->nb; ++i) {
        const Building *b = &g->bld[i];
        float by = y - building_base(b);   /* in the building's frame */
        if (by > b->height + 30.0f) continue;
        double dx = p.x - b->ox, dz = p.z - b->oz;
        double lx = b->c * dx + b->s * dz, lz = -b->s * dx + b->c * dz;
        double ux = b->c * dir.x + b->s * dir.z, uz = -b->s * dir.x + b->c * dir.z;
        const double pad = 12.0;   /* projections reach past the plot */
        double t0 = 0.0, t1 = best;
        if (!slab(lx, ux, b->foot.x0 - pad, b->foot.x1 + pad, &t0, &t1) ||
            !slab(lz, uz, b->foot.z0 - pad, b->foot.z1 + pad, &t0, &t1)) continue;
        const BGraph *bg = chunk_gen_graph(g, i);
        for (int si = 0; si < bg->nsec; ++si) {
            const BSection *s = &bg->sec[si];
            if (by < s->y0 || by > s->y1 || !bsec_facade(s)) continue;
            const BLoop *loops[2] = { &s->foot.outer, &s->foot.hole };
            for (int li = 0; li < 2; ++li) {
                const BLoop *l = loops[li];
                for (int k = 0; k < l->n; ++k) {
                    int k1 = (k + 1) % l->n;
                    double ex = l->x[k1] - l->x[k], ez = l->z[k1] - l->z[k];
                    double den = ux * ez - uz * ex;
                    if (fabs(den) < 1e-9) continue;
                    double wx = l->x[k] - lx, wz = l->z[k] - lz;
                    double t = (wx * ez - wz * ex) / den, u = (uz * wx - ux * wz) / den;
                    if (u >= 0.0 && u <= 1.0 && t > 1e-3 && t < best) {
                        best = t; back = den < 0.0;
                        h.t = t; h.b = i; h.s = si; h.li = li; h.k = k; h.u = (float)(u * sqrt(ex * ex + ez * ez));
                    }
                }
            }
        }
    }
    /* outward normals are (-dz, dx): reaching a wall from behind means the ray started inside a building */
    if (back) h.t = -1.0;
    return h;
}

/* Door canopies stand out from the ground floor around each door. */
static int near_door(const BGraph *bg, int si, int li, int k, float u, float y) {
    if (li) return 0;
    for (int d = 0; d < bg->ndoor; ++d) {
        const BDoor *dr = &bg->door[d];
        if (dr->section == si && dr->face == k && fabsf(u - dr->u) < dr->width * 0.5f + 1.2f && y < SIDEWALK_HEIGHT + dr->height + 1.0f)
            return 1;
    }
    return 0;
}

/* The solid spot nearest (u, y) on face k of section si (u in metres along
 * the face, as the shader's uv.x), within 2.5 m, with at least mu of solid wall
 * either side of it and mv above and below, by the facade rules of
 * shadeFacade in shaders/city.frag:
 *   ground floor   glass between the piers of 6 m shop bays (1.5 m lobby
 *                  modules, whose glass stops short of the floor above);
 *   upper floors   a window per bay per floor: the band between two rows of
 *                  windows and the pier between two bays are solid (a
 *                  curtain wall's spandrels and mullions included), and so
 *                  are plant floors.
 * Black- and blue-glass cladding with windows punched in it is glass all over
 * above the ground floor; as a curtain wall, its grid still counts.
 * Shop boards stop 0.4 m short of the shop piers; the neon signs in the
 * windows of the lower floors (face_signs) are kept clear of. */
/* One face's window rhythm, as shadeFacade lays it out. */
typedef struct Rhythm {
    float winW, winH, sill;   /* window share of a bay and of a floor; sill height share */
    float base, fh, bw;       /* first floor's height, floor height, bay width (stretched to whole bays) */
    float glassTop;           /* ground floor: top of the shop or lobby glass */
    float step, edge;         /* ground floor: shop bay or lobby module, and its solid margin each side */
    int   atGround, cladding; /* the section stands on the ground; glass cladding with punched windows */
} Rhythm;

static Rhythm rhythm(const BGraph *bg, int si, float faceW) {
    const BSection *s = &bg->sec[si];
    uint32_t sh = style_hash(bg->seed);
    float h4 = hash_float(hash_combine(sh, 4u)), h5 = hash_float(hash_combine(sh, 5u)), h9 = hash_float(hash_combine(sh, 9u));
    Rhythm r;
    switch (s->win) {
    case WIN_CURTAIN: r.winW = 0.94f; r.winH = 0.86f; break;
    case WIN_RIBBON:  r.winW = 1.0f;  r.winH = lerpf(0.4f, 0.55f, h9); break;
    case WIN_SLIT:    r.winW = lerpf(0.2f, 0.32f, h5); r.winH = lerpf(0.7f, 0.85f, h9); break;
    default:          r.winW = lerpf(0.45f, 0.7f, h5); r.winH = lerpf(0.45f, 0.65f, h9); break;
    }
    r.sill = (1.0f - r.winH) * 0.42f;
    r.base = SIDEWALK_HEIGHT + bg->groundH;
    r.fh = bg->floorH;
    float bw0 = bg->fine ? 1.6f + 0.8f * h4 : 2.4f + 2.6f * h4;
    r.bw = faceW / maxf(1.0f, floorf(faceW / bw0 + 0.5f));
    int retail = bg->groundMode == GF_RETAIL;
    r.glassTop = r.base - (retail ? 1.5f : bg->groundMode == GF_DOUBLE ? 1.2f : 0.7f);
    r.step = retail ? 6.0f : 1.5f;
    r.edge = retail ? 0.3f : 0.06f;
    r.atGround = bsec_grounded(s);
    r.cladding = (s->palette == 4 || s->palette == 7) && s->win != WIN_CURTAIN;
    return r;
}

/* Any glass within mu along and mv up or down of (u, y) on a face of section
 * si? Off the section counts as glass: what is drawn there is another
 * section's business. */
static int glass_near(const BGraph *bg, int si, float faceW, float u, float y, float mu, float mv) {
    const BSection *s = &bg->sec[si];
    if (y - mv < s->y0 || y + mv > s->y1) return 1;
    Rhythm r = rhythm(bg, si, faceW);
    const float gy = SIDEWALK_HEIGHT;
    float u0 = u - mu, u1 = u + mu, y0 = y - mv, y1 = y + mv;
#define HITS(a0, a1, b0, b1) (u0 < (a1) && u1 > (a0) && y0 < (b1) && y1 > (b0))
    if (r.atGround && y0 < r.base) {
        float gb = gy + (r.step > 2.0f ? 0.45f : 0.08f);
        for (int j = (int)floorf(u0 / r.step); j <= (int)floorf(u1 / r.step); ++j)
            if (HITS((float)j * r.step + r.edge, (float)(j + 1) * r.step - r.edge, gb, r.glassTop)) return 1;
    }
    if (!r.atGround || y1 > r.base) {
        if (r.cladding) return 1;
        int every = 6 + 3 * (s->plant & 15);
        for (int k = (int)floorf((y0 - r.base) / r.fh); k <= (int)floorf((y1 - r.base) / r.fh); ++k) {
            if (r.atGround && k < 0) continue;
            if ((s->plant & 15) && k > 0 && k % every == every - 1) continue;   /* louvred plant floor */
            float b0 = r.base + r.fh * ((float)k + r.sill), b1 = b0 + r.fh * r.winH;
            for (int j = (int)floorf(u0 / r.bw); j <= (int)floorf(u1 / r.bw); ++j) {
                float a0 = r.bw * ((float)j + 0.5f - 0.5f * r.winW), a1 = r.bw * ((float)j + 0.5f + 0.5f * r.winW);
                if (HITS(a0, a1, b0, b1)) return 1;
            }
        }
    }
#undef HITS
    return 0;
}

static int solid_spot(const BGraph *bg, int si, int li, int k, float faceW, float u, float y, float mu, float mv, float *uo, float *yo) {
    const BSection *s = &bg->sec[si];
    Rhythm r = rhythm(bg, si, faceW);
    const float gy = SIDEWALK_HEIGHT;
    float winW = r.winW, winH = r.winH, sill = r.sill, base = r.base, fh = r.fh, bw = r.bw;
    int atGround = r.atGround;
    /* clear of the section's bottom edge, and of its top unless another section carries the face on up */
    const BLoop *l = li ? &s->foot.hole : &s->foot.outer;
    EdgeInfo e = edge_of(l, k);
    float px = e.ax + e.ex * clampf(u, 0.0f, faceW) + e.ez * 0.05f, pz = e.az + e.ez * clampf(u, 0.0f, faceW) - e.ex * 0.05f;   /* just inside */
    int carried = 0;
    for (int t = 0; t < bg->nsec && !carried; ++t)
        carried = t != si && fabsf(bg->sec[t].y0 - s->y1) < 0.05f && bfoot_contains(&bg->sec[t].foot, px, pz);
    float lo = s->y0 + (atGround ? 0.3f : maxf(mv, 0.15f)), hi = s->y1 - (carried ? maxf(mv, 0.15f) : 0.6f);
    float umin = 0.35f, umax = faceW - 0.35f;
    float best = 2.5f * 2.5f;
    int found = 0;
#define TRY(cu, cy) do { \
        float cu_ = (cu), cy_ = (cy), d_ = (cu_ - u) * (cu_ - u) + (cy_ - y) * (cy_ - y); \
        if (cu_ >= umin && cu_ <= umax && cy_ >= lo && cy_ <= hi && d_ < best && !near_door(bg, si, li, k, cu_, cy_)) { \
            best = d_; *uo = cu_; *yo = cy_; found = 1; } \
    } while (0)

    if (atGround) {
        float cy = clampf(y, gy + 0.6f, base - 0.3f);
        int j = (int)floorf(u / r.step);
        if (r.edge >= mu - 0.07f) {   /* the pier, less half a bracket, covers the clearance */
            TRY((float)j * r.step, cy);
            TRY((float)(j + 1) * r.step, cy);
        }
        if (bg->groundMode != GF_RETAIL && 0.5f * (base - r.glassTop) >= mv)
            TRY(u, 0.5f * (r.glassTop + base));   /* lobby glass stops short of the floor above */
    }
    if (!r.cladding) {
        float ylo = maxf(atGround ? base + 0.05f : lo, lo), uc = clampf(u, umin, umax);
        int kf = (int)floorf((y - base) / fh);
        for (int j = kf - 1; j <= kf + 1; ++j) {
            /* solid from the head of floor j's windows to the sill of floor j + 1's, less the clearance */
            float a = maxf(base + fh * ((float)j + sill + winH) + mv, ylo);
            float c = minf(base + fh * ((float)j + 1.0f + sill) - mv, hi);
            if (a <= c) TRY(uc, clampf(y, a, c));
        }
        if (0.5f * (1.0f - winW) * bw >= mu - 0.05f) {   /* wide enough for the bracket and clearance */
            float cy = maxf(y, ylo), rel = cy - base;
            float inFloor = rel - fh * floorf(rel / fh);
            int signs = rel >= 0.0f && rel < 5.0f * fh && inFloor > 0.9f && inFloor < 2.3f;
            if (!signs) {
                int j = (int)floorf(u / bw);
                TRY((float)j * bw, cy);
                TRY((float)(j + 1) * bw, cy);
            }
        }
        int every = 6 + 3 * (s->plant & 15);
        if ((s->plant & 15) && kf > 0 && kf % every == every - 1)
            TRY(u, clampf(y, base + (float)kf * fh + maxf(mv, 0.3f), base + (float)(kf + 1) * fh - maxf(mv, 0.3f)));
    }
#undef TRY
    return found;
}

/* A cable end on solid wall: cast from p along dir at height y (world), slide
 * along the face that was hit to its nearest solid spot, and check nothing
 * stands in front of that. The end (just off the wall) and its height (world),
 * or 0; hit, if given, gets the face it is on (u: where along it). */
static int wall_anchor(ChunkGen *g, DVec2 p, DVec2 dir, float y, double maxT, float mu, float mv, DVec2 *at, float *ya, WallHit *hit) {
    WallHit h = facade_ray(g, p, dir, y, maxT);
    if (h.t < 0.0) return 0;
    const Building *b = &g->bld[h.b];
    const BGraph *bg = chunk_gen_graph(g, h.b);
    const BSection *s = &bg->sec[h.s];
    EdgeInfo e = edge_of(h.li ? &s->foot.hole : &s->foot.outer, h.k);
    float base = building_base(b), u2, y2;
    if (!solid_spot(bg, h.s, h.li, h.k, e.len, h.u, y - base, mu, mv, &u2, &y2)) return 0;
    y2 += base;
    DVec2 w = to_world(b, e.ax + e.ex * u2, e.az + e.ez * u2);
    DVec2 d = dsub(w, p);
    double dist = dlen(d);
    if (dist < 0.5) return 0;
    WallHit h2 = facade_ray(g, p, dscale(d, 1.0 / dist), y2, dist + 0.5);
    if (h2.t < 0.0 || h2.b != h.b || fabs(h2.t - dist) > 0.05) return 0;
    if (h2.s != h.s) {
        /* stacked sections share a face: the one there at this height must be solid at the same spot */
        const BSection *s2 = &bg->sec[h2.s];
        EdgeInfo e2 = edge_of(h2.li ? &s2->foot.hole : &s2->foot.outer, h2.k);
        float u3, y3;
        if (!solid_spot(bg, h2.s, h2.li, h2.k, e2.len, h2.u, y2 - base, mu, mv, &u3, &y3) || fabsf(u3 - h2.u) > 0.05f || fabsf(y3 - (y2 - base)) > 0.05f) return 0;
    }
    float nx = -e.ez, nz = e.ex;   /* outward, local */
    *at = dadd(w, dv(0.1 * (b->c * nx - b->s * nz), 0.1 * (b->s * nx + b->c * nz)));
    *ya = y2;
    if (hit) *hit = h2;
    return 1;
}

/* Clear of every lamp: never below the lamp tops within reach of a pole, arm included. */
static int clears_lamps(const ChunkGen *g, DVec2 a, float ya, DVec2 b, float yb, float sag) {
    DVec2 ab = dsub(b, a);
    double l2 = ddot(ab, ab);
    for (int i = 0; i < g->nl; ++i) {
        DVec2 q = dv(g->lamp[i].x, g->lamp[i].z);
        if (seg_dist(q, a, b) > 2.4) continue;
        float t = l2 > 1e-9 ? (float)clampf((float)(ddot(dsub(q, a), ab) / l2), 0.0f, 1.0f) : 0.0f;
        if (lerpf(ya, yb, t) - 4.0f * sag * t * (1.0f - t) < city_ground_y(q.x, q.z) + (LAMP_HEIGHT + 0.4f)) return 0;
    }
    return 1;
}

/* One length of cable, as emit_beam, but each end's vertices carry that end's
 * drop below the chord and the cable's sag in uv, for cableSway. */
static void emit_strand(Mesh *m, vec3 a, float da, vec3 b, float db, float sag, float size, Mat mt) {
    vec3 ax = v3_norm(v3_sub(b, a));
    vec3 h = v3_cross(ax, v3(0, 1, 0));
    h = v3_len(h) < 1e-3f ? v3(1, 0, 0) : v3_norm(h);
    vec3 v = v3_norm(v3_cross(h, ax));
    float r = size * 0.5f;
    vec3 o[4] = { v3_add(v3_scale(h, r), v3_scale(v, r)), v3_add(v3_scale(h, -r), v3_scale(v, r)),
                  v3_add(v3_scale(h, -r), v3_scale(v, -r)), v3_add(v3_scale(h, r), v3_scale(v, -r)) };
    float uv[4][2] = { { da, sag }, { da, sag }, { db, sag }, { db, sag } };
    for (int i = 0; i < 4; ++i) {
        int j = (i + 1) & 3;
        emit_quad(m, v3_add(a, o[i]), v3_add(a, o[j]), v3_add(b, o[j]), v3_add(b, o[i]), v3_norm(v3_add(o[i], o[j])), uv, mt);
    }
}

/* A parabolic sag (close enough to a catenary this taut), as thin beams. The
 * vertex shader swings it about its chord in the gusts (cableSway in
 * shaders/city.vert): params.w flags it, aux holds the direction it swings
 * in (across the chord, horizontal) as an angle in 16 bits. */
static void emit_cable(Mesh *m, const Cable *c, Mat mt) {
    vec3 d = v3_sub(c->b, c->a);
    float span = sqrtf(d.x * d.x + d.z * d.z);
    int n = (int)clampf(span / 3.5f, 5.0f, 12.0f);
    float ang = atan2f(d.x, -d.z);   /* of the horizontal perpendicular (-dz, dx) */
    mt.p[3] = 1;
    mt.aux = (uint32_t)((ang / (2.0f * PI_F) + 0.5f) * 65535.0f + 0.5f);
    vec3 prev = c->a;
    float prevDrop = 0.0f;
    for (int i = 1; i <= n; ++i) {
        float t = (float)i / (float)n, drop = 4.0f * c->sag * t * (1.0f - t);
        vec3 p = v3_add(c->a, v3_scale(d, t));
        p.y -= drop;
        emit_strand(m, prev, prevDrop, p, drop, c->sag, c->thick, mt);
        prev = p;
        prevDrop = drop;
    }
}

/* Wall anchor: a stubby bracket where a cable meets a facade. */
static void emit_anchor(Mesh *m, vec3 p, Mat mt) {
    emit_box(m, v3(p.x - 0.07f, p.y - 0.09f, p.z - 0.07f), v3(p.x + 0.07f, p.y + 0.06f, p.z + 0.07f), F_NOBOT | F_BOT, mt, UV_FACADE);
}

static vec3 chunk_pt(const ChunkGen *g, DVec2 p, float y) { return v3((float)(p.x - g->ox), y, (float)(p.z - g->oz)); }

/* How far a span sags: its share of the span, but never below `clear` (world Y). */
static float span_sag(DVec2 a, float ya, DVec2 b, float yb, float sagFrac, float clear) {
    return maxf(minf((float)dlen(dsub(b, a)) * sagFrac, minf(ya, yb) - clear), 0.1f);
}

/* Wall to wall, with brackets at both ends. */
static void emit_span(Mesh *m, const ChunkGen *g, DVec2 a, float ya, DVec2 b, float yb, float sag, float thick, Mat mt) {
    Cable c;
    c.a = chunk_pt(g, a, ya);
    c.b = chunk_pt(g, b, yb);
    c.sag = sag;
    c.thick = thick;
    emit_cable(m, &c, mt);
    emit_anchor(m, c.a, mt);
    emit_anchor(m, c.b, mt);
}

/* One strand across road `rd` from station q (on the centre line, chunk-owned),
 * over ground at gq. */
static void cross_strand(Mesh *m, ChunkGen *g, const Road *rd, DVec2 q, DVec2 dir, DVec2 nrm, float gq,
                         double skew, float ya, float yb, float sagFrac, float thick, Mat mt) {
    double maxT = rd->at.half + rd->at.walk + 14.0, minT = rd->at.half + 0.5;
    DVec2 qb = dadd(q, dscale(dir, skew)), back = dscale(nrm, -1.0), a, b;
    if (!wall_anchor(g, q, nrm, ya, maxT, STREET_GAP, STREET_GAP, &a, &ya, NULL) || !wall_anchor(g, qb, back, yb, maxT, STREET_GAP, STREET_GAP, &b, &yb, NULL)) return;
    if (ddot(dsub(a, q), nrm) < minT || ddot(dsub(b, qb), back) < minT || minf(ya, yb) < gq + (CABLE_CLEAR + 0.3f)) return;
    float sag = span_sag(a, ya, b, yb, sagFrac, gq + CABLE_CLEAR);
    if (!clears_lamps(g, a, ya, b, yb, sag)) return;
    emit_span(m, g, a, ya, b, yb, sag, thick, mt);
}

static void build_street_cables(Mesh *m, ChunkGen *g) {
    const double P = CHUNK_SIZE;
    Lamp lt[128];
    for (int ri = 0; ri < g->pa->nroads; ++ri) {
        const Road *rd = &g->pa->roads[ri];
        DVec2 d = dsub(rd->b, rd->a);
        double len = dlen(d);
        if (len < 1.0) continue;
        DVec2 dir = dscale(d, 1.0 / len), nrm = dv(-dir.z, dir.x);
        uint32_t seedR = hash3i(0xCAB1E5u ^ rd->at.id, (int32_t)floor(rd->a.x), (int32_t)floor(rd->a.z), (int32_t)floor(rd->b.x));

        /* Crossings, facade to facade. */
        Rng step = rng_make(seedR);
        double s1 = len - rd->trimB - 3.0;
        int k = 0;
        for (double s = rd->trimA + 3.0 + rng_range(&step, 0.0, 8.0); s < s1; s += rng_range(&step, 5.0f, 13.0f), ++k) {
            DVec2 q = dadd(rd->a, dscale(dir, s));
            if (!chunk_gen_owns(g, q.x, q.z)) continue;
            Rng r = rng_make(hash_combine(seedR, (uint32_t)k));
            float dens = plan_density(q.x, q.z);
            if (rng_float(&r) >= minf(0.1f + 0.7f * dens, 0.7f)) continue;
            float roll = rng_float(&r);
            int strands = roll < 0.55f ? 1 : roll < 0.82f ? 2 : 3;
            int high = rng_float(&r) < 0.3f * dens;
            float y = high ? rng_range(&r, 18.0f, 42.0f) : rng_range(&r, 5.8f, 15.5f);
            float dy = rng_range(&r, -2.0f, 2.0f);
            double skew = rng_range(&r, -5.0f, 5.0f);
            float sagFrac = rng_range(&r, 0.025f, 0.065f);
            float thick = rng_range(&r, 0.045f, 0.075f);
            float bunS = rng_range(&r, 0.25f, 0.6f), bunY = rng_range(&r, -0.45f, 0.45f);
            Mat mt = mat(MAT_METAL, seedR + (uint32_t)k, rng_float(&r) < 0.75f ? 3 : 2, 0, 0);
            float gq = city_ground_y(q.x, q.z);
            for (int j = 0; j < strands; ++j) {
                DVec2 qj = dadd(q, dscale(dir, bunS * j));
                float ya = gq + y + bunY * j, yb = maxf(ya + dy, gq + (CABLE_CLEAR + 0.8f));
                cross_strand(m, g, rd, qj, dir, nrm, gq, skew, ya, yb, sagFrac * (1.0f + 0.35f * j), thick * (j ? 0.8f : 1.0f), mt);
            }
        }

        /* Lamp to lamp along each sidewalk, and from lamps up to the wall behind. */
        int nl = city_road_lamps(g->pa, ri, g->ox - 60.0, g->oz - 60.0, g->ox + P + 60.0, g->oz + P + 60.0, lt, 128);
        for (int i = 0; i < nl; ++i) {
            const Lamp *A = &lt[i];
            if (!chunk_gen_owns(g, A->x, A->z)) continue;
            Rng r = rng_make(hash2i(seedR ^ 0x1A4Fu, (int32_t)floor(A->x * 4.0), (int32_t)floor(A->z * 4.0)));
            float dens = plan_density(A->x, A->z);
            const float ga = city_ground_y(A->x, A->z), top = ga + (LAMP_HEIGHT - 0.25f);   /* world */
            Mat mt = mat(MAT_METAL, seedR ^ (uint32_t)i, 3, 0, 0);
            if (rng_float(&r) < 0.3f + 0.35f * dens) {
                for (int j = i + 1; j < nl && j <= i + 3; ++j) {
                    const Lamp *B = &lt[j];
                    double dd = hypot(B->x - A->x, B->z - A->z);
                    if (A->dx * B->dx + A->dz * B->dz < 0.9f || dd > LAMP_SPACING * 1.3) continue;
                    Cable c;
                    c.a = chunk_pt(g, dv(A->x, A->z), top);
                    c.b = chunk_pt(g, dv(B->x, B->z), city_ground_y(B->x, B->z) + (LAMP_HEIGHT - 0.25f));
                    c.sag = (float)dd * rng_range(&r, 0.035f, 0.06f);
                    c.thick = rng_range(&r, 0.035f, 0.055f);
                    emit_cable(m, &c, mt);
                    break;
                }
            }
            if (rng_float(&r) < 0.12f + 0.25f * dens) {
                /* feeder: up from the lamp to the facade behind it */
                DVec2 back = dv(-A->dx, -A->dz), w;
                float y;
                if (wall_anchor(g, dv(A->x, A->z), back, ga + rng_range(&r, 8.2f, 12.0f), rd->at.walk + 14.0, STREET_GAP, STREET_GAP, &w, &y, NULL) && y > top + 0.5f) {
                    Cable c;
                    c.a = chunk_pt(g, dv(A->x, A->z), top);
                    c.b = chunk_pt(g, w, y);
                    c.sag = (float)dlen(dsub(w, dv(A->x, A->z))) * 0.08f;
                    c.thick = 0.045f;
                    emit_cable(m, &c, mt);
                    emit_anchor(m, c.b, mt);
                }
            }
        }
    }
}

/* Inside a block's buildable lot: an alley or courtyard, not a street or sidewalk. */
static int in_lot(const ChunkGen *g, DVec2 p) {
    for (int k = 0; k < g->pa->nblocks; ++k) {
        const Block *bk = &g->pa->blocks[k];
        if (!bk->lot.n || p.x < bk->x0 || p.x > bk->x1 || p.z < bk->z0 || p.z > bk->z1) continue;
        if (poly_contains(&bk->lot, p.x, p.z, 0.0)) return 1;
    }
    return 0;
}

/* A face in the world: its start corner, the unit vectors along and out of it,
 * and its building's base (heights on it count from there). */
typedef struct WallFrame { DVec2 o, t, n; float len, base; const BGraph *bg; int s; } WallFrame;

static WallFrame wall_frame(ChunkGen *g, int bi, int si, int li, int k) {
    const Building *b = &g->bld[bi];
    WallFrame w;
    w.bg = chunk_gen_graph(g, bi);
    w.s = si;
    const BSection *s = &w.bg->sec[si];
    EdgeInfo e = edge_of(li ? &s->foot.hole : &s->foot.outer, k);
    w.o = to_world(b, e.ax, e.az);
    w.t = dv(b->c * e.ex - b->s * e.ez, b->s * e.ex + b->c * e.ez);
    w.n = dv(-w.t.z, w.t.x);   /* outward is (-dz, dx) */
    w.len = e.len;
    w.base = building_base(b);
    return w;
}

/* In a narrow alley a cable runs close along the walls near its ends: wherever
 * it passes within 0.35 m of wall w, sag included, the facade behind it must be
 * solid, not just where it is fixed. Heights are world Y. */
static int clear_of_glass(const WallFrame *w, DVec2 a, float ya, DVec2 b, float yb, float sag) {
    for (int i = 0; i <= 32; ++i) {
        float t = (float)i / 32.0f;
        DVec2 p = dadd(a, dscale(dsub(b, a), t));
        double d = ddot(dsub(p, w->o), w->n), u = ddot(dsub(p, w->o), w->t);
        if (d > 0.35 || u < 0.0 || u > w->len) continue;
        if (glass_near(w->bg, w->s, w->len, (float)u, lerpf(ya, yb, t) - 4.0f * sag * t * (1.0f - t) - w->base, 0.06f, 0.06f)) return 0;
    }
    return 1;
}

/* Alleys and courtyards: from stations along each low wall of the chunk's
 * buildings to whatever wall faces it across the gap, skewed so the lines
 * criss-cross, and hung lower and slacker than over the streets. Stricter than
 * over the streets, where the walls are far apart: both ends must have solid
 * wall all round them, and the cable must stay clear of glass along the walls. */
static void build_alley_cables(Mesh *m, ChunkGen *g) {
    for (int i = 0; i < g->nb; ++i) {
        const Building *b = &g->bld[i];
        if (!chunk_gen_owns(g, b->ox, b->oz)) continue;
        const BGraph *bg = chunk_gen_graph(g, i);
        float pCable = 0.3f + 0.45f * b->density, base = building_base(b);
        for (int si = 0; si < bg->nsec; ++si) {
            const BSection *s = &bg->sec[si];
            if (s->y0 > 14.0f || s->y1 < 5.0f || !bsec_facade(s)) continue;
            const BLoop *loops[2] = { &s->foot.outer, &s->foot.hole };
            for (int li = 0; li < 2; ++li) {
                const BLoop *l = loops[li];
                for (int k = 0; k < l->n; ++k) {
                    EdgeInfo e = edge_of(l, k);
                    if (e.len < 3.0f) continue;
                    uint32_t seedE = hash3i(bg->seed ^ 0xA11E7u, si, li, k);
                    Rng step = rng_make(seedE);
                    int idx = 0;
                    for (float u = rng_range(&step, 0.8f, 3.0f); u < e.len - 0.8f; u += rng_range(&step, 2.5f, 6.0f), ++idx) {
                        Rng r = rng_make(hash_combine(seedE, (uint32_t)idx));
                        if (rng_float(&r) >= pCable) continue;
                        /* mostly low: the first few floors are where alleys tangle */
                        float ylo = maxf(3.8f, s->y0), yhi = minf(s->y1, 24.0f);
                        float ya0 = ylo + maxf(yhi - ylo, 0.0f) * powf(rng_float(&r), 1.8f), dy = rng_range(&r, -3.0f, 3.0f);
                        float skew = rng_range(&r, -0.7f, 0.7f), sagFrac = rng_range(&r, 0.04f, 0.1f);
                        float thick = rng_range(&r, 0.035f, 0.065f);
                        int strands = rng_float(&r) < 0.3f ? 2 : 1;
                        Mat mt = mat(MAT_METAL, seedE + (uint32_t)idx, rng_float(&r) < 0.7f ? 3 : 2, 0, 0);
                        for (int j = 0; j < strands; ++j) {
                            float uj, ya, yb;   /* ya in the building's frame, then world; yb world */
                            if (!solid_spot(bg, si, li, k, e.len, u + 0.3f * (float)j, ya0, ALLEY_GAP_U, ALLEY_GAP_V, &uj, &ya) ||
                                glass_near(bg, si, e.len, uj, ya, ALLEY_GAP_U - 0.05f, ALLEY_GAP_V - 0.05f)) continue;
                            /* the wall there must be this section's, not behind another one */
                            float lx = e.ax + e.ex * uj - e.ez * 0.02f, lz = e.az + e.ez * uj + e.ex * 0.02f;   /* outward is (-dz, dx) */
                            int covered = 0;
                            for (int sj = 0; sj < bg->nsec && !covered; ++sj)
                                covered = sj != si && ya >= bg->sec[sj].y0 && ya <= bg->sec[sj].y1 && bfoot_contains(&bg->sec[sj].foot, lx, lz);
                            if (covered) continue;
                            DVec2 pw = to_world(b, lx, lz);
                            float nx = -e.ez + e.ex * skew, nz = e.ex + e.ez * skew, nn = sqrtf(nx * nx + nz * nz);
                            DVec2 dir = dv((b->c * nx - b->s * nz) / nn, (b->s * nx + b->c * nz) / nn), pb;
                            WallHit hb;
                            if (!wall_anchor(g, pw, dir, base + maxf(ya + dy, 4.0f), 14.0, ALLEY_GAP_U, ALLEY_GAP_V, &pb, &yb, &hb) || dlen(dsub(pb, pw)) < 1.8) continue;
                            ya += base;
                            DVec2 mid = dscale(dadd(pw, pb), 0.5);
                            float gm = city_ground_y(mid.x, mid.z);
                            if (minf(ya, yb) < gm + (ALLEY_CLEAR + 0.4f) || !in_lot(g, mid)) continue;
                            WallFrame wa = wall_frame(g, i, si, li, k), wb = wall_frame(g, hb.b, hb.s, hb.li, hb.k);
                            if (glass_near(wb.bg, wb.s, wb.len, hb.u, yb - wb.base, ALLEY_GAP_U - 0.05f, ALLEY_GAP_V - 0.05f)) continue;
                            DVec2 pa = to_world(b, lx - e.ez * 0.08f, lz + e.ex * 0.08f);
                            float sag = span_sag(pa, ya, pb, yb, sagFrac * (1.0f + 0.4f * (float)j), gm + ALLEY_CLEAR);
                            if (!clear_of_glass(&wa, pa, ya, pb, yb, sag) || !clear_of_glass(&wb, pa, ya, pb, yb, sag)) continue;
                            emit_span(m, g, pa, ya, pb, yb, sag, thick, mt);
                        }
                    }
                }
            }
        }
    }
}

void build_cables(Mesh *m, ChunkGen *g) {
    build_street_cables(m, g);
    build_alley_cables(m, g);
}
