#include "building_internal.h"

/* ===========================================================================
 * Footprint grammar: a small family of base shapes, built from parameters so
 * setbacks, slices and variants are always regenerated rather than edited
 * ========================================================================= */

static const char *const FAM_NAMES[FAM_COUNT] = { "rectangle", "chamfered", "L", "T", "U", "cross", "drum", "courtyard" };

void make_foot(const FootSpec *s, BFoot *f) {
    memset(f, 0, sizeof(*f));       /* whole graphs compare byte for byte */
    BLoop *l = &f->outer;
    float w = (s->quarter & 1) ? s->d : s->w, d = (s->quarter & 1) ? s->w : s->d;
    float hw = w * 0.5f, hd = d * 0.5f;
    switch (s->fam) {
    case FAM_CHAMFER: {
        float c = s->p1 * minf(w, d);
        loop_add(l, -hw + c, -hd); loop_add(l, hw - c, -hd); loop_add(l, hw, -hd + c); loop_add(l, hw, hd - c);
        loop_add(l, hw - c, hd); loop_add(l, -hw + c, hd); loop_add(l, -hw, hd - c); loop_add(l, -hw, -hd + c);
        break;
    }
    case FAM_L: {
        float aw = s->p1 * w, ad = s->p2 * d;      /* notch at the +x,+z corner */
        loop_add(l, -hw, -hd); loop_add(l, hw, -hd); loop_add(l, hw, -hd + ad);
        loop_add(l, -hw + aw, -hd + ad); loop_add(l, -hw + aw, hd); loop_add(l, -hw, hd);
        break;
    }
    case FAM_T: {
        float st = s->p1 * w, bar = s->p2 * d;     /* bar along +z, stem towards -z */
        loop_add(l, -st * 0.5f, -hd); loop_add(l, st * 0.5f, -hd); loop_add(l, st * 0.5f, hd - bar);
        loop_add(l, hw, hd - bar); loop_add(l, hw, hd); loop_add(l, -hw, hd);
        loop_add(l, -hw, hd - bar); loop_add(l, -st * 0.5f, hd - bar);
        break;
    }
    case FAM_U: {
        float arm = s->p1 * w, base = s->p2 * d;   /* opening towards +z */
        loop_add(l, -hw, -hd); loop_add(l, hw, -hd); loop_add(l, hw, hd); loop_add(l, hw - arm, hd);
        loop_add(l, hw - arm, -hd + base); loop_add(l, -hw + arm, -hd + base); loop_add(l, -hw + arm, hd); loop_add(l, -hw, hd);
        break;
    }
    case FAM_CROSS: {
        float ax = s->p1 * w * 0.5f, az = s->p2 * d * 0.5f;
        loop_add(l, -ax, -hd); loop_add(l, ax, -hd); loop_add(l, ax, -az); loop_add(l, hw, -az);
        loop_add(l, hw, az); loop_add(l, ax, az); loop_add(l, ax, hd); loop_add(l, -ax, hd);
        loop_add(l, -ax, az); loop_add(l, -hw, az); loop_add(l, -hw, -az); loop_add(l, -ax, -az);
        break;
    }
    case FAM_DRUM: {
        int n = (int)s->p1;
        n = n < 8 ? 8 : n > 24 ? 24 : n;
        for (int i = 0; i < n; ++i) {
            float a = 2.0f * PI_F * ((float)i + 0.5f) / (float)n;
            loop_add(l, hw * cosf(a), hd * sinf(a));
        }
        break;
    }
    case FAM_COURT: {
        loop_add(l, -hw, -hd); loop_add(l, hw, -hd); loop_add(l, hw, hd); loop_add(l, -hw, hd);
        float iw = s->p1 * w * 0.5f, id = s->p2 * d * 0.5f;
        BLoop *h = &f->hole;
        loop_add(h, -iw, -id); loop_add(h, iw, -id); loop_add(h, iw, id); loop_add(h, -iw, id);
        break;
    }
    default:
        loop_add(l, -hw, -hd); loop_add(l, hw, -hd); loop_add(l, hw, hd); loop_add(l, -hw, hd);
        break;
    }
    float a = s->ang + (float)s->quarter * PI_F * 0.5f;
    float c = cosf(a), sn = sinf(a);
    BLoop *loops[2] = { &f->outer, &f->hole };
    for (int k = 0; k < 2; ++k)
        for (int i = 0; i < loops[k]->n; ++i) {
            float x = loops[k]->x[i], z = loops[k]->z[i];
            loops[k]->x[i] = s->cx + c * x - sn * z;
            loops[k]->z[i] = s->cz + sn * x + c * z;
        }
    loop_orient(&f->outer, 1);
    if (f->hole.n) loop_orient(&f->hole, 0);
}

static FootSpec spec_inset(FootSpec s, float m) { s.w -= 2.0f * m; s.d -= 2.0f * m; return s; }
static FootSpec spec_scale(FootSpec s, float k) { s.w *= k; s.d *= k; return s; }

/* Feature sizes have to make sense at building scale (arms and courts >= 8 m). */
static int spec_ok(const FootSpec *s) {
    float mn = minf(s->w, s->d);
    if (s->w < 8.0f || s->d < 8.0f) return 0;
    switch (s->fam) {
    case FAM_CHAMFER: return s->p1 * mn >= 1.5f;
    case FAM_L: return s->p1 * s->w >= 8.0f && s->p2 * s->d >= 8.0f && (1.0f - s->p1) * s->w >= 6.0f && (1.0f - s->p2) * s->d >= 6.0f;
    case FAM_T: return s->p1 * s->w >= 8.0f && s->p2 * s->d >= 8.0f && (1.0f - s->p2) * s->d >= 6.0f;
    case FAM_U: return s->p1 * s->w >= 8.0f && s->p2 * s->d >= 8.0f && (1.0f - 2.0f * s->p1) * s->w >= 8.0f;
    case FAM_CROSS: return s->p1 * s->w >= 8.0f && s->p2 * s->d >= 8.0f && (1.0f - s->p1) * s->w >= 8.0f;
    case FAM_DRUM: return mn >= 14.0f;
    case FAM_COURT: return s->p1 * s->w >= 10.0f && s->p2 * s->d >= 10.0f &&
                           (1.0f - s->p1) * s->w * 0.5f >= 7.0f && (1.0f - s->p2) * s->d * 0.5f >= 7.0f;
    default: return 1;
    }
}

/* Section 4.2's minimum width: whether a spec's footprint is at least
 * c->minW across in every direction (its narrowest calliper width). */
static int spec_wide(const Ctx *c, const FootSpec *s) {
    BFoot f;
    make_foot(s, &f);
    return bloop_width(&f.outer) >= c->minW - 0.01f;
}

/* A tower's spec grown about its centre until it is wide enough, if that
 * keeps it within w by d (what it stands on). */
static int widen_spec(Ctx *c, FootSpec *s, float w, float d) {
    if (spec_wide(c, s)) return 1;
    BFoot f;
    make_foot(s, &f);
    float have = bloop_width(&f.outer);
    if (have <= 0.0f) return 0;
    FootSpec g = spec_scale(*s, c->minW / have * 1.002f);
    if (g.w > w + 0.01f || g.d > d + 0.01f || !spec_wide(c, &g)) {
        blog_printf(c->log, "  rejected: tower %.1f m across, under four bays (%.1f m), with no room to widen\n", have, c->minW);
        return 0;
    }
    *s = g;
    return 1;
}

/* ===========================================================================
 * Building grammar
 * ========================================================================= */

float lvl(const Ctx *c, int k) { return G0 + c->g->groundH + (float)k * c->g->floorH; }

int pick(Rng *r, const float *w, int n) {
    float t = 0.0f;
    for (int i = 0; i < n; ++i) t += w[i];
    if (t <= 0.0f) return 0;
    float x = rng_float(r) * t;
    for (int i = 0; i < n; ++i) { if (x < w[i]) return i; x -= w[i]; }
    return n - 1;
}

int add_sec(Ctx *c, int type, float y0, float y1, const BFoot *f, int b0, int b1) {
    BGraph *g = c->g;
    if (g->nsec >= BG_MAX_SECT) return -1;
    BSection *s = &g->sec[g->nsec];
    memset(s, 0, sizeof(*s));
    s->type = type; s->y0 = y0; s->y1 = y1; s->foot = *f;
    if (b0 >= 0) s->below[s->nbelow++] = b0;
    if (b1 >= 0) s->below[s->nbelow++] = b1;
    s->arr = s->nbelow ? ARR_STACK : ARR_GROUND;
    s->win = c->defWin; s->palette = c->defPal; s->bands = c->defBands; s->fins = c->defFins; s->ledTop = c->defLed;
    return g->nsec++;
}

int add_support(Ctx *c, int type, int sec, float x0, float y0, float z0, float x1, float y1, float z1, float size) {
    BGraph *g = c->g;
    if (g->nsup >= BG_MAX_SUPP) return -1;
    BSupport *s = &g->sup[g->nsup];
    s->type = type; s->section = sec;
    s->x0 = x0; s->y0 = y0; s->z0 = z0; s->x1 = x1; s->y1 = y1; s->z1 = z1; s->size = size;
    return g->nsup++;
}

/* Columns under the outer edges of section `sec`, inset from its edge, on
 * every edge (mask < 0) or on the edges whose outward normals are in `mask`
 * (SIDE_* bits), skipping any that would stand inside something already
 * built in the storeys they pass through. */
static void columns_under(Ctx *c, int sec, float inset, float spacing, float y0, float size, int mask) {
    const BLoop *l = &c->g->sec[sec].foot.outer;
    float y1 = c->g->sec[sec].y0;
    for (int k = 0; k < l->n; ++k) {
        int k1 = (k + 1) % l->n;
        float ex = l->x[k1] - l->x[k], ez = l->z[k1] - l->z[k], len = sqrtf(ex * ex + ez * ez);
        if (len < 1.0f) continue;
        ex /= len; ez /= len;
        float nx = -ez, nz = ex;              /* outward */
        if (mask >= 0) {
            int side = fabsf(nx) > fabsf(nz) ? (nx < 0 ? SIDE_W : SIDE_E) : (nz < 0 ? SIDE_S : SIDE_N);
            if (!(mask & side)) continue;
        }
        int n = (int)ceilf(len / spacing);
        for (int j = 0; j < n; ++j) {
            float u = clampf(len * (float)j / (float)n, inset, len - inset);
            float x = l->x[k] + ex * u - nx * inset, z = l->z[k] + ez * u - nz * inset;
            if (!bfoot_contains(&c->g->sec[sec].foot, x, z)) continue;
            int blocked = 0;
            for (int t = 0; t < c->g->nsec && !blocked; ++t) {
                const BSection *o = &c->g->sec[t];
                if (t == sec || o->y0 >= y1 - 0.1f || o->y1 <= y0 + 0.1f) continue;
                blocked = bfoot_contains(&o->foot, x, z) || bfoot_boundary_dist(&o->foot, x, z, NULL, NULL) < size * 0.5f + 0.4f;
            }
            for (int t = 0; t < c->g->nsup && !blocked; ++t)
                blocked = c->g->sup[t].type == SUP_COLUMN && fabsf(c->g->sup[t].x0 - x) < size && fabsf(c->g->sup[t].z0 - z) < size;
            if (!blocked) add_support(c, SUP_COLUMN, sec, x, y0, z, x, y1, z, size);
        }
    }
}

FootSpec site_spec(Ctx *c, int fam, float margin) {
    FootSpec s;
    memset(&s, 0, sizeof(s));
    s.fam = fam;
    s.w = c->W - 2.0f * margin;
    s.d = c->D - 2.0f * margin;
    Rng *r = &c->rf;
    switch (fam) {
    case FAM_CHAMFER: s.p1 = rng_range(r, 0.08f, 0.2f); break;
    case FAM_L:       s.p1 = rng_range(r, 0.4f, 0.6f); s.p2 = rng_range(r, 0.4f, 0.6f); break;
    case FAM_T:       s.p1 = rng_range(r, 0.35f, 0.5f); s.p2 = rng_range(r, 0.35f, 0.5f); break;
    case FAM_U:       s.p1 = rng_range(r, 0.25f, 0.33f); s.p2 = rng_range(r, 0.3f, 0.45f); break;
    case FAM_CROSS:   s.p1 = rng_range(r, 0.35f, 0.5f); s.p2 = rng_range(r, 0.35f, 0.5f); break;
    case FAM_DRUM:    s.p1 = minf(s.w, s.d) >= 30.0f ? 16.0f : 12.0f; break;
    case FAM_COURT:   s.p1 = rng_range(r, 0.35f, 0.5f); s.p2 = rng_range(r, 0.35f, 0.5f); break;
    default: break;
    }
    /* Turn notches and openings away from the street where possible. */
    if (fam == FAM_L || fam == FAM_T || fam == FAM_U) {
        static const int faceOfQuarter[4] = { SIDE_N, SIDE_W, SIDE_S, SIDE_E };
        int ok[4], nok = 0;
        for (int q = 0; q < 4; ++q) if (!(c->frontage & faceOfQuarter[q])) ok[nok++] = q;
        s.quarter = nok ? ok[rng_int(r, 0, nok - 1)] : rng_int(r, 0, 3);
    }
    return s;
}

/* Ground floor plus body up to floor nTop, optionally with an arcade: the
 * ground floor set back behind perimeter columns (on every side, or on the
 * street sides only). Returns the body (or the ground floor if nTop is 0). */
static int build_base(Ctx *c, const FootSpec *s, int nTop, int arcade, int streetOnly, int bodyType, int *groundOut) {
    BFoot body;
    make_foot(s, &body);
    float yg = lvl(c, 0);
    if (arcade && nTop > 0) {
        float sb = rng_range(&c->rf, 2.2f, 3.4f);
        FootSpec gs = *s;
        int mask = -1;
        if (streetOnly && s->fam == FAM_RECT && s->ang == 0.0f && !(s->quarter & 1)) {
            float x0 = s->cx - s->w * 0.5f, x1 = s->cx + s->w * 0.5f, z0 = s->cz - s->d * 0.5f, z1 = s->cz + s->d * 0.5f;
            if (c->frontage & SIDE_W) x0 += sb;
            if (c->frontage & SIDE_E) x1 -= sb;
            if (c->frontage & SIDE_S) z0 += sb;
            if (c->frontage & SIDE_N) z1 -= sb;
            gs.w = x1 - x0; gs.d = z1 - z0; gs.cx = (x0 + x1) * 0.5f; gs.cz = (z0 + z1) * 0.5f;
            mask = c->frontage;
        } else {
            gs = spec_inset(gs, sb);
        }
        if (spec_ok(&gs) && mask != 0) {
            BFoot gf;
            make_foot(&gs, &gf);
            int gi = add_sec(c, SEC_GROUND, G0, yg, &gf, -1, -1);
            int bi = add_sec(c, bodyType, yg, lvl(c, nTop), &body, gi, -1);
            if (bi < 0) return -1;
            c->g->sec[bi].arr = ARR_ARCADE;
            columns_under(c, bi, 0.7f, rng_range(&c->rf, 4.5f, 6.5f), G0, 0.6f, mask);
            blog_printf(c->log, "  arcade: ground floor set back %.1f m behind columns\n", sb);
            *groundOut = gi;
            return bi;
        }
    }
    int gi = add_sec(c, SEC_GROUND, G0, yg, &body, -1, -1);
    *groundOut = gi;
    if (nTop <= 0) return gi;
    return add_sec(c, bodyType, yg, lvl(c, nTop), &body, gi, -1);
}

/* A tower above section `on`: straight with setbacks, or a tapered, waisted,
 * flared or twisted stack of thin slices derived from the first. Returns the
 * top section. */
static int build_tower(Ctx *c, FootSpec ts, int on, int k0, int kTop, int allowProfile) {
    static const char *PROF[] = { "straight", "tapered", "waisted", "flared", "twisted" };
    float pw[5] = { 0.5f, 0.15f, 0.1f, 0.1f, 0.15f };
    int prof = allowProfile && kTop - k0 >= 10 ? pick(&c->rf, pw, 5) : 0;
    if (prof == 1 || prof == 2 || prof == 3) {
        FootSpec thin = spec_scale(ts, prof == 1 ? 0.65f : 0.72f);   /* its narrowest slice */
        if (!spec_wide(c, &thin)) {
            blog_printf(c->log, "  rejected: %s profile would narrow under four bays (%.1f m); straight instead\n", PROF[prof], c->minW);
            prof = 0;
        }
    }
    blog_printf(c->log, "  tower: %s %s %.0fx%.0f m, floors %d-%d\n", PROF[prof], FAM_NAMES[ts.fam], ts.w, ts.d, k0, kTop);
    int below = on;
    if (prof == 0) {
        int steps = rng_float(&c->rf) < 0.3f ? rng_int(&c->rf, 1, 2) : 0;
        int k = k0;
        FootSpec cur = ts;
        for (int i = 0; i <= steps; ++i) {
            int kEnd = i == steps ? kTop : k + (int)((float)(kTop - k) * rng_range(&c->rf, 0.55f, 0.8f));
            if (kEnd <= k) continue;
            BFoot f;
            make_foot(&cur, &f);
            int si = add_sec(c, i ? SEC_SETBACK : SEC_TOWER, lvl(c, k), lvl(c, kEnd), &f, below, -1);
            if (si < 0) break;
            below = si;
            k = kEnd;
            FootSpec nx = spec_inset(cur, rng_range(&c->rf, 2.0f, 4.0f));
            if (!spec_ok(&nx) || !spec_wide(c, &nx)) { c->g->sec[si].y1 = lvl(c, kTop); break; }
            cur = nx;
        }
        return below;
    }
    int S = (int)clampf((float)((kTop - k0) / 2), 4.0f, 10.0f);
    float twist = 0.0f;
    if (prof == 4) {
        twist = rng_range(&c->rf, 0.35f, 1.1f);
        if (rng_float(&c->rf) < 0.5f) twist = -twist;
    }
    for (int i = 0; i < S; ++i) {
        float t = S > 1 ? (float)i / (float)(S - 1) : 0.0f;
        float sc = prof == 1 ? 1.0f - 0.35f * t : prof == 2 ? 1.0f - 0.28f * sinf(PI_F * t) : prof == 3 ? 0.72f + 0.28f * t : 1.0f;
        FootSpec sp = spec_scale(ts, sc);
        sp.ang = ts.ang + twist * t;
        int ka = k0 + (kTop - k0) * i / S, kb = k0 + (kTop - k0) * (i + 1) / S;
        if (kb <= ka) continue;
        BFoot f;
        make_foot(&sp, &f);
        int si = add_sec(c, SEC_SLICE, lvl(c, ka), lvl(c, kb), &f, below, -1);
        if (si < 0) break;
        if (i > 0) c->g->sec[si].arr = ARR_STEP;
        below = si;
    }
    return below;
}

static int add_crown(Ctx *c, int top, FootSpec base, int kFrom, int kTo, int spire) {
    if (kTo <= kFrom) return top;
    FootSpec cs = spec_inset(base, rng_range(&c->rf, 1.5f, 3.0f));
    if (!spec_ok(&cs) || !spec_wide(c, &cs)) return top;
    BFoot f;
    make_foot(&cs, &f);
    int si = add_sec(c, SEC_CROWN, lvl(c, kFrom), lvl(c, kTo), &f, top, -1);
    if (si < 0) return top;
    if (spire) c->g->sec[si].spire = (uint8_t)rng_int(&c->rr, 10, 28);
    c->g->sec[si].ledTop = 1;
    return si;
}

int arch_office(Ctx *c) {
    float fw[FAM_COUNT] = { 0.35f, 0.2f, 0.15f, 0.05f, 0.1f, 0.0f, 0.0f, 0.15f };
    int fam = pick(&c->rf, fw, FAM_COUNT);
    FootSpec s = site_spec(c, fam, rng_range(&c->rf, 0.0f, 1.2f));
    if (!spec_ok(&s) || !spec_wide(c, &s)) s = site_spec(c, FAM_RECT, 0.5f);
    blog_printf(c->log, "  footprint: %s %.0fx%.0f m\n", FAM_NAMES[s.fam], s.w, s.d);
    int arcade = (s.fam == FAM_RECT || s.fam == FAM_CHAMFER) && rng_float(&c->rf) < 0.12f;
    int setback = c->N >= 6 && rng_float(&c->rf) < 0.35f;
    int kLow = setback ? (int)maxf(2.0f, (float)c->N * rng_range(&c->rf, 0.55f, 0.8f)) : c->N;
    int gi, body = build_base(c, &s, kLow, arcade, 0, SEC_BODY, &gi);
    if (body < 0) return 0;
    if (setback && kLow < c->N) {
        FootSpec u = spec_inset(s, rng_range(&c->rf, 2.5f, 5.0f));
        if (spec_ok(&u) && spec_wide(c, &u)) {
            BFoot f;
            make_foot(&u, &f);
            add_sec(c, SEC_SETBACK, lvl(c, kLow), lvl(c, c->N), &f, body, -1);
        } else c->g->sec[body].y1 = lvl(c, c->N);
    }
    return 1;
}

int arch_skyscraper(Ctx *c) {
    int N = c->N;
    if (N < 8) return 0;
    int P = (int)clampf((float)N * rng_range(&c->rf, 0.08f, 0.18f), 1.0f, 5.0f);
    int fam = rng_float(&c->rf) < 0.65f ? FAM_RECT : FAM_CHAMFER;
    FootSpec ps = site_spec(c, fam, rng_range(&c->rf, 0.0f, 1.0f));
    if (!spec_ok(&ps)) ps = site_spec(c, FAM_RECT, 0.5f);
    int gi, pod = build_base(c, &ps, P, rng_float(&c->rf) < 0.15f, 0, SEC_PODIUM, &gi);
    if (pod < 0) return 0;
    if (rng_float(&c->rs) < 0.4f) {      /* a masonry podium under a glass tower */
        uint8_t stone[4] = { 5, 6, 0, 3 };
        uint8_t p = stone[rng_int(&c->rs, 0, 3)];
        for (int i = 0; i < c->g->nsec; ++i) { c->g->sec[i].palette = p; if (c->g->sec[i].win == WIN_CURTAIN) c->g->sec[i].win = WIN_PUNCHED; }
    }
    float tfw[FAM_COUNT] = { 0.3f, 0.25f, 0.0f, 0.0f, 0.0f, 0.15f, 0.15f, 0.0f };
    FootSpec ts = site_spec(c, pick(&c->rf, tfw, FAM_COUNT), 0.0f);
    float k = rng_range(&c->rf, 0.5f, 0.78f);
    ts.w = ps.w * k; ts.d = ps.d * rng_range(&c->rf, 0.5f, 0.78f);
    if (ts.fam == FAM_CROSS || ts.fam == FAM_DRUM) ts.w = ts.d = minf(ts.w, ts.d) * 1.1f;
    ts.cx = ps.cx; ts.cz = ps.cz;
    if (rng_float(&c->rf) < 0.3f) {       /* off-centre over the podium */
        ts.cx += (ps.w - ts.w) * 0.5f * rng_range(&c->rf, -0.8f, 0.8f);
        ts.cz += (ps.d - ts.d) * 0.5f * rng_range(&c->rf, -0.8f, 0.8f);
    }
    if (rng_float(&c->rf) < 0.2f) {       /* turned against its podium */
        ts.ang = rng_range(&c->rf, 0.17f, 0.7f);
        ts = spec_scale(ts, 0.8f);
    }
    if (!spec_ok(&ts) || !widen_spec(c, &ts, ps.w, ps.d)) return 0;
    ts.cx = clampf(ts.cx, ps.cx - (ps.w - ts.w) * 0.5f, ps.cx + (ps.w - ts.w) * 0.5f);
    ts.cz = clampf(ts.cz, ps.cz - (ps.d - ts.d) * 0.5f, ps.cz + (ps.d - ts.d) * 0.5f);
    int crown = rng_float(&c->rf) < 0.55f && N - P >= 12 ? rng_int(&c->rf, 2, 4) : 0;
    int top = build_tower(c, ts, pod, P, N - crown, 1);
    if (crown) {
        /* crown follows the shape the tower ended with */
        FootSpec last = ts;
        const BSection *t = &c->g->sec[top];
        float x0, z0, x1, z1;
        loop_bounds(&t->foot.outer, &x0, &z0, &x1, &z1);
        if (t->type == SEC_SLICE) { last.fam = FAM_RECT; last.w = (x1 - x0) * 0.8f; last.d = (z1 - z0) * 0.8f; last.ang = 0; last.cx = (x0 + x1) * 0.5f; last.cz = (z0 + z1) * 0.5f; last.quarter = 0; }
        top = add_crown(c, top, last, N - crown, N, rng_float(&c->rr) < 0.45f);
    }
    for (int i = 0; i < c->g->nsec; ++i) {
        BSection *s = &c->g->sec[i];
        if (s->type == SEC_TOWER || s->type == SEC_SLICE || s->type == SEC_SETBACK) {
            if (N >= 24 && rng_float(&c->rs) < 0.5f) s->plant = (uint8_t)rng_int(&c->rs, 1, 4);
        }
    }
    for (int i = 0; i < c->g->nsec; ++i)
        if (c->g->sec[i].type == SEC_TOWER || c->g->sec[i].type == SEC_SLICE) {
            c->g->sec[i].accent = rng_float(&c->rs) < 0.5f;
            break;
        }
    return 1;
}

int arch_projection(Ctx *c) {
    int N = c->N;
    if (N < 4) return 0;
    float m = rng_range(&c->rf, 0.0f, 0.8f);
    float p = rng_range(&c->rf, 2.0f, 7.0f);
    float sw[3] = { 0.45f, 0.3f, 0.25f };
    int sup = pick(&c->rf, sw, 3);       /* columns, struts or the core alone */
    int sideMask;
    static const int sides[4] = { SIDE_W, SIDE_E, SIDE_S, SIDE_N };
    int first = sides[rng_int(&c->rf, 0, 3)];
    for (int i = 0; i < 4; ++i) if (c->frontage & sides[i]) { first = sides[i]; break; }
    sideMask = first;
    if (rng_float(&c->rf) < 0.3f) sideMask |= (first & (SIDE_W | SIDE_E)) ? (rng_float(&c->rf) < 0.5f ? SIDE_S : SIDE_N)
                                                                           : (rng_float(&c->rf) < 0.5f ? SIDE_W : SIDE_E);
    float x0 = -c->W * 0.5f + m, x1 = c->W * 0.5f - m, z0 = -c->D * 0.5f + m, z1 = c->D * 0.5f - m;
    FootSpec blk;
    memset(&blk, 0, sizeof(blk));
    blk.fam = FAM_RECT; blk.w = x1 - x0; blk.d = z1 - z0;
    if (sideMask & SIDE_W) x0 += p;
    if (sideMask & SIDE_E) x1 -= p;
    if (sideMask & SIDE_S) z0 += p;
    if (sideMask & SIDE_N) z1 -= p;
    FootSpec bs = blk;
    bs.w = x1 - x0; bs.d = z1 - z0; bs.cx = (x0 + x1) * 0.5f; bs.cz = (z0 + z1) * 0.5f;
    if (!spec_ok(&bs)) return 0;
    if (!spec_wide(c, &bs)) { blog_printf(c->log, "  rejected: base %.1f m across behind the projection, under four bays\n", minf(bs.w, bs.d)); return 0; }
    int k0 = rng_int(&c->rf, 1, N - 3 < 4 ? N - 3 : 4);
    int k1 = k0 + rng_int(&c->rf, 2, 6);
    if (k1 > N) k1 = N;
    static const char *SUP[] = { "columns", "raked struts", "core" };
    blog_printf(c->log, "  projection: %.1f m on %s%s, %s, floors %d-%d\n", p,
         (sideMask & (SIDE_W | SIDE_E)) && (sideMask & (SIDE_S | SIDE_N)) ? "a corner" : "one side", "", SUP[sup], k0, k1);
    int gi, base = build_base(c, &bs, k0, 0, 0, SEC_BODY, &gi);
    if (base < 0) return 0;
    BFoot bf, kf;
    make_foot(&bs, &bf);
    make_foot(&blk, &kf);
    int pb = add_sec(c, SEC_PROJECTION, lvl(c, k0), lvl(c, k1), &kf, base, -1);
    if (pb < 0) return 0;
    c->g->sec[pb].arr = ARR_PROJECTION;
    c->g->sec[pb].support = sup == 0 ? SUP_COLUMN : sup == 1 ? SUP_STRUT : SUP_CORE;
    if (sup == 0) {
        columns_under(c, pb, 0.6f, 6.0f, G0, 0.7f, sideMask);
    } else if (sup == 1) {
        /* raked struts from the face of the base up to the projection's outer edge */
        const BLoop *l = &kf.outer;
        for (int k = 0; k < l->n; ++k) {
            int k1i = (k + 1) % l->n;
            float ex = l->x[k1i] - l->x[k], ez = l->z[k1i] - l->z[k], len = sqrtf(ex * ex + ez * ez);
            ex /= len; ez /= len;
            float nx = -ez, nz = ex;
            int side = fabsf(nx) > fabsf(nz) ? (nx < 0 ? SIDE_W : SIDE_E) : (nz < 0 ? SIDE_S : SIDE_N);
            if (!(sideMask & side)) continue;
            int n = (int)ceilf(len / 6.0f);
            for (int j = 0; j <= n; ++j) {
                float u = clampf(len * (float)j / (float)n, 1.0f, len - 1.0f);
                float tx = l->x[k] + ex * u - nx * 0.4f, tz = l->z[k] + ez * u - nz * 0.4f;
                float bx = tx - nx * (p - 0.4f), bz = tz - nz * (p - 0.4f);
                float by = maxf(lvl(c, k0) - p * 1.3f, lvl(c, 0));
                if (!bfoot_contains(&bf, bx - nx * 0.3f, bz - nz * 0.3f)) continue;
                add_support(c, SUP_STRUT, pb, bx, by, bz, tx, lvl(c, k0), tz, 0.45f);
            }
        }
    }
    if (k1 < N) {
        if (c->b->kind == BK_TOWER && N - k1 >= 8) {
            FootSpec ts = bs;
            ts = spec_scale(ts, rng_range(&c->rf, 0.6f, 0.85f));
            ts.fam = rng_float(&c->rf) < 0.5f ? FAM_RECT : FAM_CHAMFER;
            ts.p1 = rng_range(&c->rf, 0.08f, 0.2f);
            if (spec_ok(&ts) && widen_spec(c, &ts, bs.w, bs.d)) build_tower(c, ts, pb, k1, N, 0);
        } else {
            add_sec(c, SEC_BODY, lvl(c, k1), lvl(c, N), &bf, pb, -1);
        }
    }
    return 1;
}

int arch_twin(Ctx *c) {
    int alongX = c->W >= c->D;
    float L = alongX ? c->W : c->D, S = alongX ? c->D : c->W;
    float m = rng_range(&c->rf, 0.0f, 0.8f);
    float gap = rng_range(&c->rf, 9.0f, 14.0f);
    float tl = (L - 2.0f * m - gap) * 0.5f * rng_range(&c->rf, 0.85f, 1.0f);
    float tsd = (S - 2.0f * m) * rng_range(&c->rf, 0.6f, 0.9f);
    if (tl < maxf(10.0f, c->minW) || tsd < maxf(10.0f, c->minW) || c->N < 12) return 0;
    int P = rng_int(&c->rf, 1, 3);
    FootSpec ps = site_spec(c, FAM_RECT, m);
    int gi, pod = build_base(c, &ps, P, rng_float(&c->rf) < 0.15f, 0, SEC_PODIUM, &gi);
    if (pod < 0) return 0;
    float fw[FAM_COUNT] = { 0.4f, 0.35f, 0, 0, 0, 0, 0.25f, 0 };
    FootSpec ts = site_spec(c, pick(&c->rf, fw, FAM_COUNT), 0.0f);
    ts.w = alongX ? tl : tsd; ts.d = alongX ? tsd : tl;
    if (ts.fam == FAM_DRUM && minf(ts.w, ts.d) < 14.0f) ts.fam = FAM_RECT;
    float off = gap * 0.5f + tl * 0.5f;
    int crown = rng_float(&c->rf) < 0.5f ? rng_int(&c->rf, 2, 3) : 0;
    int NB = c->N - (rng_float(&c->rf) < 0.5f ? rng_int(&c->rf, 0, c->N / 8) : 0);
    int top[2];
    for (int t = 0; t < 2; ++t) {
        FootSpec tt = ts;
        if (alongX) tt.cx = t ? off : -off; else tt.cz = t ? off : -off;
        int nt = (t ? NB : c->N) - crown;
        top[t] = build_tower(c, tt, pod, P, nt, 0);
        if (crown) top[t] = add_crown(c, top[t], tt, nt, nt + crown, rng_float(&c->rr) < 0.4f);
    }
    /* sky bridge between the two shafts, bearing on both */
    int lowTop = NB < c->N ? NB : c->N;
    int kb = P + (int)((float)(lowTop - crown - P) * rng_range(&c->rf, 0.45f, 0.7f));
    float bw = minf(tsd * 0.5f, rng_range(&c->rf, 6.0f, 9.0f));
    FootSpec br;
    memset(&br, 0, sizeof(br));
    br.fam = FAM_RECT;
    float blen = gap + tl * 0.4f;
    br.w = alongX ? blen : bw; br.d = alongX ? bw : blen;
    BFoot bf;
    make_foot(&br, &bf);
    /* the tower sections the bridge rests on */
    int ta = -1, tb = -1;
    for (int i = 0; i < c->g->nsec; ++i) {
        const BSection *s = &c->g->sec[i];
        if (s->y0 > lvl(c, kb) + 0.01f || s->y1 < lvl(c, kb + 2) - 0.01f || i == pod || i == gi) continue;
        float x0, z0, x1, z1;
        loop_bounds(&s->foot.outer, &x0, &z0, &x1, &z1);
        float cpos = alongX ? (x0 + x1) * 0.5f : (z0 + z1) * 0.5f;
        if (cpos < 0.0f && ta < 0) ta = i;
        if (cpos > 0.0f && tb < 0) tb = i;
    }
    if (ta >= 0 && tb >= 0) {
        int bi = add_sec(c, SEC_BRIDGE, lvl(c, kb), lvl(c, kb + 2), &bf, ta, tb);
        if (bi >= 0) {
            c->g->sec[bi].arr = ARR_BRIDGE;
            c->g->sec[bi].win = WIN_CURTAIN;
            c->g->sec[bi].ledTop = 1;
            c->g->sec[bi].fins = 0;
            blog_printf(c->log, "  sky bridge: floors %d-%d, clear span %.1f m\n", kb, kb + 2, gap);
        }
    }
    return 1;
}

int arch_gateway(Ctx *c) {
    int alongX = c->W >= c->D;
    float L = alongX ? c->W : c->D, S = alongX ? c->D : c->W;
    float m = rng_range(&c->rf, 0.0f, 0.8f);
    float pw = L * rng_range(&c->rf, 0.2f, 0.3f);
    int ks = rng_int(&c->rf, 2, 4);
    int ke = ks + rng_int(&c->rf, 3, 7);
    if (ke > c->N) ke = c->N;
    if (ke <= ks || pw < 8.0f || S - 2.0f * m < 10.0f) return 0;
    blog_printf(c->log, "  gateway: piers %.0f m wide, clear span %.1f m, span floors %d-%d\n", pw, L - 2.0f * m - 2.0f * pw, ks, ke);
    int pier[2];
    for (int t = 0; t < 2; ++t) {
        FootSpec s;
        memset(&s, 0, sizeof(s));
        s.fam = FAM_RECT;
        float cpos = (t ? 1.0f : -1.0f) * (L * 0.5f - m - pw * 0.5f);
        s.w = alongX ? pw : S - 2.0f * m; s.d = alongX ? S - 2.0f * m : pw;
        if (alongX) s.cx = cpos; else s.cz = cpos;
        BFoot f;
        make_foot(&s, &f);
        pier[t] = add_sec(c, SEC_PIER, G0, lvl(c, ks), &f, -1, -1);
        if (pier[t] < 0) return 0;
    }
    FootSpec sp = site_spec(c, FAM_RECT, m + 0.3f);
    BFoot f;
    make_foot(&sp, &f);
    int span = add_sec(c, SEC_SPAN, lvl(c, ks), lvl(c, ke), &f, pier[0], pier[1]);
    if (span < 0) return 0;
    c->g->sec[span].arr = ARR_SPAN;
    /* transfer structure: a deep girder across the passage */
    float in = L * 0.5f - m - pw, y1 = lvl(c, ks), y0 = y1 - 1.6f, hs = (S - 2.0f * m) * 0.5f - 0.3f;
    if (alongX) add_support(c, SUP_TRANSFER, span, -in - 0.5f, y0, -hs, in + 0.5f, y1, hs, 0.0f);
    else        add_support(c, SUP_TRANSFER, span, -hs, y0, -in - 0.5f, hs, y1, in + 0.5f, 0.0f);
    if (c->b->kind == BK_TOWER && c->N - ke >= 8) {
        FootSpec ts = spec_scale(sp, rng_range(&c->rf, 0.5f, 0.75f));
        ts.fam = FAM_CHAMFER;
        ts.p1 = rng_range(&c->rf, 0.08f, 0.2f);
        if (spec_ok(&ts) && widen_spec(c, &ts, sp.w, sp.d)) build_tower(c, ts, span, ke, c->N, 1);
    }
    return 1;
}

int arch_arcology(Ctx *c) {
    FootSpec s = site_spec(c, rng_float(&c->rf) < 0.6f ? FAM_RECT : FAM_CHAMFER, 1.0f);
    if (!spec_ok(&s)) return 0;
    int steps = rng_int(&c->rf, 6, 9), below = -1, k = 0;
    float remaining = (float)c->N;
    for (int i = 0; i < steps; ++i) {
        int kEnd = i == steps - 1 ? c->N : k + (int)maxf(1.0f, remaining * rng_range(&c->rf, 0.16f, 0.26f));
        if (kEnd > c->N) kEnd = c->N;
        BFoot f;
        make_foot(&s, &f);
        int si = add_sec(c, i ? SEC_SETBACK : SEC_GROUND, i ? lvl(c, k) : G0, lvl(c, kEnd), &f, below, -1);
        if (si < 0) break;
        below = si;
        remaining -= (float)(kEnd - k);
        k = kEnd;
        if (k >= c->N) break;
        FootSpec nx = spec_inset(s, rng_range(&c->rf, 4.0f, 8.0f));
        if (nx.w < 14.0f || nx.d < 14.0f) { c->g->sec[si].y1 = lvl(c, c->N); break; }
        s = nx;
    }
    if (below >= 0) c->g->sec[below].spire = (uint8_t)rng_int(&c->rr, 30, 60);
    return below >= 0;
}

int arch_shophouse(Ctx *c) {
    FootSpec s = site_spec(c, FAM_RECT, 0.0f);
    int gi, body = build_base(c, &s, c->N, rng_float(&c->rf) < 0.55f, 1, SEC_BODY, &gi);
    return body >= 0;
}

/* Housing slab or point block, optionally lifted on a void deck: an open
 * ground floor of lift cores and columns under the block. */
int arch_housing(Ctx *c, int point) {
    FootSpec s;
    if (point) {
        float fw[FAM_COUNT] = { 0.15f, 0.3f, 0, 0, 0, 0.45f, 0.1f, 0 };
        float m = rng_range(&c->rf, 0.0f, 1.0f);
        s = site_spec(c, pick(&c->rf, fw, FAM_COUNT), m);
        s.w = s.d = minf(s.w, s.d);
    } else {
        s = site_spec(c, rng_float(&c->rf) < 0.7f ? FAM_RECT : FAM_CHAMFER, 0.0f);
        if (s.fam == FAM_CHAMFER) s.p1 = rng_range(&c->rf, 0.05f, 0.1f);
    }
    if (!spec_ok(&s)) s = site_spec(c, FAM_RECT, 0.0f);
    int crown = point && rng_float(&c->rf) < 0.5f && c->N >= 12 ? 2 : 0;
    if (c->N - crown < 1) return 0;
    int voidDeck = rng_float(&c->rf) < (point ? 0.3f : 0.4f);
    int top;
    if (voidDeck) {
        c->g->groundMode = GF_VOID;
        BFoot body;
        make_foot(&s, &body);
        float yg = lvl(c, 0);
        int cores[2] = { -1, -1 }, nc = 0;
        int alongX = s.w >= s.d;
        float L = alongX ? s.w : s.d, S = alongX ? s.d : s.w;
        int want = !point && L >= 40.0f ? 2 : 1;
        for (int i = 0; i < want; ++i) {
            FootSpec cs;
            memset(&cs, 0, sizeof(cs));
            cs.fam = FAM_RECT;
            float cl = minf(8.0f, L * 0.3f), cw = minf(S - 4.0f, 8.0f);
            cs.w = alongX ? cl : cw; cs.d = alongX ? cw : cl;
            float cpos = want == 1 ? 0.0f : (i ? 1.0f : -1.0f) * L * 0.25f;
            cs.cx = s.cx + (alongX ? cpos : 0.0f); cs.cz = s.cz + (alongX ? 0.0f : cpos);
            BFoot cf;
            make_foot(&cs, &cf);
            cores[nc++] = add_sec(c, SEC_CORE, G0, yg, &cf, -1, -1);
        }
        top = add_sec(c, SEC_BODY, yg, lvl(c, c->N - crown), &body, cores[0], cores[1]);
        if (top < 0) return 0;
        c->g->sec[top].arr = ARR_ARCADE;
        columns_under(c, top, 0.8f, 6.0f, G0, 0.7f, -1);
        blog_printf(c->log, "  void deck: %d lift core(s) and columns\n", nc);
    } else {
        int gi;
        top = build_base(c, &s, c->N - crown, 0, 0, SEC_BODY, &gi);
        if (top < 0) return 0;
    }
    if (crown) {
        top = add_crown(c, top, s, c->N - crown, c->N, 0);
        c->g->sec[top].roof = ROOF_TANKS;
    }
    return 1;
}

int arch_shed(Ctx *c) {
    FootSpec s = site_spec(c, FAM_RECT, rng_range(&c->rf, 0.0f, 1.0f));
    if (!spec_ok(&s)) return 0;
    int gi, body = build_base(c, &s, c->N, 0, 0, SEC_BODY, &gi);
    return body >= 0;
}
