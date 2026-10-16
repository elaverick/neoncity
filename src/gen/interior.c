#include "interior_internal.h"

/* Office-lobby planner (docs/procedural-office-atrium-generation.md). Section
 * numbers in the comments refer to that document. */

#define G0    SIDEWALK_HEIGHT

const char *interior_region_name(int t) { return t >= 0 && t < IR_COUNT ? REG_NAMES[t] : "?"; }

int interior_kind(int t) {
    if (t <= IR_SECURE) return IK_OPEN;
    if (t <= IR_RISER) return IK_SOLID;
    return IK_ROOM;
}

/* ===========================================================================
 * Room catalogue (section 6.3)
 * ========================================================================= */

static const RoomSpec ROOMS[] = {
    { IR_TOILET,     5.0f, 8.0f, 15.0f, 2.6f, PREF_HALL,     0 },
    { IR_CLEANER,    2.0f, 3.0f,  4.0f, 1.8f, PREF_HALL,     0 },
    { IR_SECURITY,   3.5f, 5.0f, 10.0f, 2.6f, PREF_ENTRANCE, 0 },
    { IR_MAIL,       3.0f, 5.0f,  9.0f, 2.4f, PREF_ENTRANCE, 0 },
    { IR_FACILITIES, 3.5f, 5.5f, 11.0f, 2.6f, PREF_REAR,     0 },
    { IR_PLANT,      5.0f, 8.0f, 18.0f, 3.0f, PREF_REAR,     0 },
    { IR_MEETING,    4.0f, 6.0f, 13.0f, 3.0f, PREF_ENTRANCE, 1 },
    { IR_SHOWER,     5.0f, 8.0f, 18.0f, 3.0f, PREF_HALL,     0 },
    { IR_CYCLE,      5.0f, 9.0f, 20.0f, 3.5f, PREF_REAR,     0 },
    { IR_BINS,       3.5f, 6.0f,  9.0f, 2.4f, PREF_SERVICE,  0 },
    { IR_LOADING,    5.0f, 9.0f, 26.0f, 3.5f, PREF_SERVICE,  0 },
    { IR_ESCAPE,     6.0f, 6.0f,  0.0f, 3.0f, PREF_FAR,      0 },
};

const RoomSpec *room_spec(int type) {
    for (size_t i = 0; i < ARRAY_LEN(ROOMS); ++i) if (ROOMS[i].type == type) return &ROOMS[i];
    return &ROOMS[0];
}

/* ===========================================================================
 * Generation context
 * ========================================================================= */

/* ===========================================================================
 * Step 1: prepare
 * ========================================================================= */

static int side_of(float nx, float nz) {
    return fabsf(nx) > fabsf(nz) ? (nx > 0 ? 1 : 3) : (nz > 0 ? 2 : 0);
}

int interior_sections(const BGraph *g, int *out, int max) {
    if (g->groundMode != GF_LOBBY && g->groundMode != GF_DOUBLE) return 0;
    if (g->archetype == ARCH_ARCOLOGY) return 0;
    int n = 0, primary = -1;
    for (int d = 0; d < g->ndoor; ++d) if (g->door[d].type == DOOR_PRIMARY) primary = g->door[d].section;
    if (primary < 0) return 0;
    if (n < max) out[n++] = primary;
    for (int i = 0; i < g->nsec && n < max; ++i)
        if (i != primary && g->sec[i].y0 <= G0 + 0.01f && g->sec[i].type != SEC_CORE) out[n++] = i;
    return n;
}

/* The entrance: the primary door, else a service door, else a synthesised one
 * on the most street-facing edge. Gives the door's index (-1: synthesised)
 * and its face, centre u and width w. */
static int find_entrance(Gen *G, int *face, float *u, float *w) {
    const BGraph *g = G->g;
    ILayout *L = G->L;
    const BLoop *ol = &g->sec[G->si].foot.outer;
    int ed = -1;
    for (int d = 0; d < g->ndoor; ++d) if (g->door[d].section == G->si && g->door[d].type == DOOR_PRIMARY) ed = d;
    G->grammar = ed >= 0 ? IG_OFFICE : IG_SERVICE;
    L->grammar = G->grammar;
    if (ed < 0) for (int d = 0; d < g->ndoor && ed < 0; ++d) if (g->door[d].section == G->si) ed = d;
    L->synthFace = -1;
    if (ed >= 0) {
        *face = g->door[ed].face; *u = g->door[ed].u; *w = g->door[ed].width;
    } else {
        /* the most street-facing edge (section 4.1 default) */
        float best = -1.0f;
        int f = 0;
        for (int k = 0; k < ol->n; ++k) {
            int k1 = (k + 1) % ol->n;
            float ex = ol->x[k1] - ol->x[k], ez = ol->z[k1] - ol->z[k], len = sqrtf(ex * ex + ez * ez);
            if (len < 3.0f) continue;
            float nx = -ez / len, nz = ex / len;
            int side = fabsf(nx) > fabsf(nz) ? (nx < 0 ? SIDE_W : SIDE_E) : (nz < 0 ? SIDE_S : SIDE_N);
            float sc = len * ((G->b->frontage & side) ? 4.0f : 1.0f);
            if (sc > best) { best = sc; f = k; }
        }
        int k1 = (f + 1) % ol->n;
        float len = sqrtf((ol->x[k1] - ol->x[f]) * (ol->x[k1] - ol->x[f]) + (ol->z[k1] - ol->z[f]) * (ol->z[k1] - ol->z[f]));
        *face = f; *u = len * 0.5f; *w = 1.3f;
        L->synthFace = f; L->synthU = *u; L->synthW = *w;
    }
    return ed;
}

/* The frame: turned so the entrance (outward normal nx, nz) faces -z, and the
 * planning grid over the footprint in it. */
static int frame_and_grid(Gen *G, float nx, float nz, int *qOut) {
    ILayout *L = G->L;
    const BSection *s = &G->g->sec[G->si];
    static const float FX[4] = { 0, 1, 0, -1 }, FZ[4] = { -1, 0, 1, 0 };
    int q = 0;
    float bd = -2.0f;
    for (int k = 0; k < 4; ++k) {
        float d = nx * FX[k] + nz * FZ[k];
        if (d > bd + 1e-4f) { bd = d; q = k; }
    }
    L->quarter = q;
    foot_to_frame(q, &s->foot, &L->foot);
    float x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
    for (int i = 0; i < L->foot.outer.n; ++i) {
        x0 = minf(x0, L->foot.outer.x[i]); x1 = maxf(x1, L->foot.outer.x[i]);
        z0 = minf(z0, L->foot.outer.z[i]); z1 = maxf(z1, L->foot.outer.z[i]);
    }
    G->minx = x0; G->minz = z0; G->maxx = x1; G->maxz = z1;
    L->nx = (int)ceilf((x1 - x0) / CELL - 0.01f);
    L->nz = (int)ceilf((z1 - z0) / CELL - 0.01f);
    if (L->nx > IN_MAX_DIM || L->nz > IN_MAX_DIM || L->nx < 8 || L->nz < 8)
        return fail(G, "footprint %.0f x %.0f m is outside the planning grid", x1 - x0, z1 - z0);
    L->gx0 = x0; L->gz0 = z0;

    *qOut = q;
    return 1;
}

/* Occupancy: outside, usable or courtyard; edge cells are not wholly inside. */
static void mark_occupancy(Gen *G) {
    ILayout *L = G->L;
    int nUse = 0;
    for (int j = 0; j < L->nz; ++j)
        for (int i = 0; i < L->nx; ++i) {
            int c = IDX(L, i, j);
            float px = ccx(L, i), pz = ccz(L, j);
            int inO = bloop_inside(&L->foot.outer, px, pz), inH = L->foot.hole.n && bloop_inside(&L->foot.hole, px, pz);
            L->occ[c] = inO ? (inH ? IO_COURT : IO_USABLE) : IO_OUTSIDE;
            L->own[c] = NONE;
            uint16_t f = 0;
            if (L->occ[c] == IO_USABLE) {
                nUse++;
                const float h = CELL * 0.5f - 0.01f;
                for (int k = 0; k < 4; ++k) {
                    float qx = px + ((k & 1) ? h : -h), qz = pz + ((k & 2) ? h : -h);
                    if (!bfoot_contains(&L->foot, qx, qz)) f |= IF_EDGE;
                }
            }
            G->baseFlag[c] = f;
        }
    L->area = (float)nUse * CELL * CELL;

}

/* The entrance in the frame (centre cx, cz and outward normal nx, nz in the
 * building frame), the cell behind it, and the first 3 m inside kept clear. */
static int place_entry(Gen *G, int ed, int q, float cx, float cz, float nx, float nz, float w) {
    const BGraph *g = G->g;
    ILayout *L = G->L;
    SDoor *e = &G->entry;
    quarter_turn(4 - q, cx, cz, &e->x, &e->z);
    quarter_turn(4 - q, nx, nz, &e->nx, &e->nz);
    e->w = w; e->side = side_of(e->nx, e->nz); e->type = ed >= 0 ? g->door[ed].type : DOOR_SERVICE;
    G->entryCell = -1;
    for (float t = 0.6f; t < 3.0f && G->entryCell < 0; t += 0.25f) {
        int c = cell_at(L, e->x - e->nx * t, e->z - e->nz * t);
        if (c >= 0 && L->occ[c] == IO_USABLE && !(G->baseFlag[c] & IF_EDGE)) G->entryCell = c;
    }
    if (G->entryCell < 0) return fail(G, "no usable floor behind the entrance");
    /* keep the first 3 m inside the door clear */
    for (int j = 0; j < L->nz; ++j)
        for (int i = 0; i < L->nx; ++i) {
            float dx = ccx(L, i) - e->x, dz = ccz(L, j) - e->z;
            float along = fabsf(dx * -e->nz + dz * e->nx), in = -(dx * e->nx + dz * e->nz);
            if (in > -0.3f && in < 3.0f && along < e->w * 0.5f + 1.0f) G->baseFlag[IDX(L, i, j)] |= IF_ENTRY;
        }

    return 1;
}

/* Service and loading doors on this section (rooms are anchored to them). */
static void find_service_doors(Gen *G, int ed, int q) {
    const BGraph *g = G->g;
    const BLoop *ol = &g->sec[G->si].foot.outer;
    G->nserv = 0;
    for (int d = 0; d < g->ndoor; ++d) {
        const BDoor *dr = &g->door[d];
        if (dr->section != G->si || d == ed || dr->type == DOOR_PRIMARY) continue;
        int f1 = (dr->face + 1) % ol->n;
        float fx = ol->x[f1] - ol->x[dr->face], fz = ol->z[f1] - ol->z[dr->face], fl = sqrtf(fx * fx + fz * fz);
        fx /= fl; fz /= fl;
        SDoor *sd = &G->serv[G->nserv++];
        quarter_turn(4 - q, ol->x[dr->face] + fx * dr->u, ol->z[dr->face] + fz * dr->u, &sd->x, &sd->z);
        quarter_turn(4 - q, -fz, fx, &sd->nx, &sd->nz);
        sd->w = dr->width; sd->side = side_of(sd->nx, sd->nz); sd->type = dr->type;
    }

}

/* Towers above the base: each needs its own core. */
static void find_towers(Gen *G, int q) {
    const BGraph *g = G->g;
    const BSection *s = &g->sec[G->si];
    G->ntower = 0;
    for (int t = 0; t < g->nsec && G->ntower < IN_MAX_CORE; ++t) {
        const BSection *ts = &g->sec[t];
        if (!(ts->type == SEC_TOWER || (ts->type == SEC_SLICE && ts->arr != ARR_STEP))) continue;
        float mx = 0.0f, mz = 0.0f;
        for (int i = 0; i < ts->foot.outer.n; ++i) { mx += ts->foot.outer.x[i]; mz += ts->foot.outer.z[i]; }
        mx /= (float)ts->foot.outer.n; mz /= (float)ts->foot.outer.n;
        if (!bfoot_contains(&s->foot, mx, mz)) continue;
        foot_to_frame(q, &ts->foot, &G->tower[G->ntower++]);
    }

}

/* The section directly above the ground floor (podium atria). */
static void find_above(Gen *G, int q) {
    const BGraph *g = G->g;
    const BSection *s = &g->sec[G->si];
    G->hasAbove = 0;
    if (s->type != SEC_PIER) {
        for (int t = 0; t < g->nsec && !G->hasAbove; ++t) {
            const BSection *a = &g->sec[t];
            if (fabsf(a->y0 - s->y1) > 0.05f) continue;
            for (int k = 0; k < a->nbelow; ++k)
                if (a->below[k] == G->si && !a->foot.hole.n && !s->foot.hole.n) {
                    G->hasAbove = 1;
                    foot_to_frame(q, &a->foot, &G->above);
                    G->aboveFloors = (int)floorf((a->y1 - a->y0) / g->floorH + 0.5f);
                    G->aboveArea = -bloop_area(&a->foot.outer);
                }
        }
    }

}

/* Floors served, and the ground floor's levels. */
static void set_levels(Gen *G) {
    const BGraph *g = G->g;
    ILayout *L = G->L;
    float top = 0.0f;
    for (int t = 0; t < g->nsec; ++t) top = maxf(top, g->sec[t].y1);
    L->floors = 1.0f + floorf((top - (G0 + g->groundH)) / g->floorH + 0.5f);
    L->floorY = G0;
    L->ceilY = G0 + g->groundH;
    L->glassTop = G0 + g->groundH - (g->groundMode == GF_DOUBLE ? 1.2f : 0.7f);
}

static int prepare(Gen *G) {
    const BLoop *ol = &G->g->sec[G->si].foot.outer;
    int face;
    float u, w;
    int ed = find_entrance(G, &face, &u, &w);
    int k1 = (face + 1) % ol->n;
    float ex = ol->x[k1] - ol->x[face], ez = ol->z[k1] - ol->z[face], len = sqrtf(ex * ex + ez * ez);
    ex /= len; ez /= len;
    float nx = -ez, nz = ex;                                    /* outward */
    float cx = ol->x[face] + ex * u, cz = ol->z[face] + ez * u;

    int q;
    if (!frame_and_grid(G, nx, nz, &q)) return 0;
    mark_occupancy(G);
    if (!place_entry(G, ed, q, cx, cz, nx, nz, w)) return 0;
    find_service_doors(G, ed, q);
    find_towers(G, q);
    find_above(G, q);
    set_levels(G);
    return 1;
}

/* ===========================================================================
 * Step 2: parameters, drawn up front from named streams
 * ========================================================================= */

static void init_streams(Gen *G) {
    uint32_t s = G->L->seed, a = (uint32_t)G->attempt;
    G->rp = rng_make(hash_combine(hash_combine(s, 0x50415241u), a));      /* parameters */
    G->rc = rng_make(hash_combine(hash_combine(s, 0x434F5245u), a));      /* cores */
    G->rr = rng_make(hash_combine(hash_combine(s, 0x524F4F4Du), a));      /* rooms */
    G->rd = rng_make(hash_combine(hash_combine(s, 0x444F4F52u), a));      /* doors */
    G->rrec = rng_make(hash_combine(hash_combine(s, 0x52454345u), a));    /* reception */
    G->rwait = rng_make(hash_combine(hash_combine(s, 0x57414954u), a));   /* waiting */
    G->rcafe = rng_make(hash_combine(hash_combine(s, 0x43414645u), a));   /* cafe */
    G->rstair = rng_make(hash_combine(hash_combine(s, 0x53544149u), a));  /* feature stair */
    G->rdec = rng_make(hash_combine(hash_combine(s, 0x4445434Fu), a));    /* decoration */
}

static void choose_params(Gen *G) {
    ILayout *L = G->L;
    Rng *r = &G->rp;
    int at = G->attempt, office = G->grammar == IG_OFFICE;
    float A = L->area;
    G->vestibule = office && at < 4 && rng_float(r) < 0.65f;
    G->reception = office && at < 5 && A >= 200.0f && rng_float(r) < 0.9f;
    G->recSide = rng_float(r) < 0.4f;
    G->waiting = G->reception && at < 3 && A >= 300.0f && rng_float(r) < 0.8f;
    G->cafe = office && at < 3 && A >= 650.0f && rng_float(r) < 0.5f;
    G->gated = office && at < 4 && A >= 360.0f && L->floors >= 6.0f && rng_float(r) < 0.72f;
    G->lanes = (int)clampf(floorf(L->floors / 10.0f + 0.5f) + 2.0f, 2.0f, 6.0f);
    G->expansive = rng_float(r) < 0.5f;
    G->bandDepth = G->expansive ? rng_range(r, 4.5f, 5.5f) : rng_range(r, 5.0f, 6.5f);
    G->frontKeep = rng_range(r, 6.0f, 9.0f);
    G->coreRule = rng_int(r, 0, 2);
    G->coreSide = rng_int(r, 0, 1);
    float shortSide = minf(G->maxx - G->minx, G->maxz - G->minz);
    G->columns = at < 4 && shortSide >= 22.0f;
    G->colSpacing = rng_range(r, 7.8f, 9.0f);
    for (int i = 0; i < 3; ++i) G->bandOrder[i] = i;
    for (int i = 2; i > 0; --i) { int k = rng_int(r, 0, i), t = G->bandOrder[i]; G->bandOrder[i] = G->bandOrder[k]; G->bandOrder[k] = t; }
    G->voidShare = rng_range(r, 0.22f, 0.36f);

    /* atrium mode */
    int dbl = G->g->groundMode == GF_DOUBLE;
    float roll = rng_float(r);
    G->atrium = dbl ? IA_DOUBLE : IA_NONE;
    G->levels = 0;
    if (office && at < 4) {
        if (dbl) G->atrium = roll < 0.55f && !L->foot.hole.n ? IA_GALLERY : IA_DOUBLE;   /* the gallery slab has no courtyard */
        else if (G->hasAbove && G->aboveFloors >= 2 && G->aboveArea >= 480.0f && roll < 0.3f) {
            G->atrium = IA_PODIUM;
            G->levels = rng_int(r, 1, G->aboveFloors - 1 < 3 ? G->aboveFloors - 1 : 3);
        }
    }

    /* cores (Appendix A) */
    G->lifts = (int)clampf(floorf(L->floors / 7.0f + 0.5f) + 1.0f, 2.0f, 8.0f);
    G->facing = G->lifts >= 6 || (G->lifts >= 4 && rng_float(r) < 0.3f);
    G->stairs = G->facing ? 2 : (G->lifts >= 3 ? 2 : 1);
    G->hallD = rng_int(r, 7, 8);
    G->stairSide = rng_int(r, 0, 1);
    if (at >= 5) { G->lifts = G->lifts < 2 ? G->lifts : 2; G->facing = 0; G->stairs = 1; }
    if (!office) { G->lifts = 1; G->facing = 0; G->stairs = 1; }

    /* the room list: optional rooms by floor area and seeded chance */
    G->nrooms = 0;
    if (!office) {
        G->rooms[G->nrooms++] = IR_PLANT;
    } else if (at < 5) {
        struct { uint8_t type; float area, chance; int gatedOk; } opt[] = {
            { IR_TOILET, 180, 1.0f, 0 }, { IR_CLEANER, 280, 0.8f, 0 }, { IR_SECURITY, 500, 0.7f, 1 },
            { IR_MAIL, 420, 0.6f, 0 }, { IR_FACILITIES, 560, 0.55f, 0 }, { IR_MEETING, 620, 0.55f, 0 },
            { IR_MEETING, 950, 0.6f, 0 }, { IR_PLANT, 700, 0.45f, 0 }, { IR_SHOWER, 750, 0.45f, 0 },
            { IR_CYCLE, 800, 0.4f, 0 } };
        int kept = 0;
        for (size_t i = 0; i < ARRAY_LEN(opt); ++i) {
            float roll2 = rng_float(r), roll3 = rng_float(r);
            int ok = (A >= opt[i].area || (opt[i].gatedOk && G->gated)) && roll2 < opt[i].chance;
            if (!ok) continue;
            if (opt[i].type != IR_TOILET) {
                if (at == 3) continue;                         /* ladder step 4: toilets only */
                if (G->expansive && kept >= 2 && roll3 >= 0.5f) continue;
                kept++;
            }
            G->rooms[G->nrooms++] = opt[i].type;
        }
    }
    alog(G, "  info: params: %s%s%s%s%s%s lobby, %s, %d lift%s%s, %d stair%s%s\n",
         G->vestibule ? "vestibule, " : "", G->reception ? (G->recSide ? "side reception, " : "central reception, ") : "",
         G->waiting ? "waiting, " : "", G->cafe ? "cafe, " : "", G->gated ? "gated, " : "",
         G->expansive ? "expansive" : "compact", ATRIUM_NAMES[G->atrium], G->lifts, G->lifts == 1 ? "" : "s",
         G->facing ? " in facing banks" : "", G->stairs, G->stairs == 1 ? "" : "s", G->columns ? ", column grid" : "");

    /* frontage keep-clear (no back-of-house rooms) */
    for (int j = 0; j < L->nz; ++j)
        for (int i = 0; i < L->nx; ++i)
            if (ccz(L, j) - G->minz < G->frontKeep) L->flag[IDX(L, i, j)] |= IF_FRONT;
}

/* ===========================================================================
 * Driver: the retry ladder (step 7)
 * ========================================================================= */

static void finish_regions(ILayout *L) {
    for (int r = 0; r < L->nreg; ++r) {
        IRegion *R = &L->reg[r];
        R->x0 = R->z0 = 32767; R->x1 = R->z1 = -1; R->cells = 0;
    }
    for (int j = 0; j < L->nz; ++j)
        for (int i = 0; i < L->nx; ++i) {
            int o = L->own[IDX(L, i, j)];
            if (o == NONE || L->occ[IDX(L, i, j)] != IO_USABLE) continue;
            IRegion *R = &L->reg[o];
            R->cells++;
            if (i < R->x0) R->x0 = (int16_t)i;
            if (j < R->z0) R->z0 = (int16_t)j;
            if (i + 1 > R->x1) R->x1 = (int16_t)(i + 1);
            if (j + 1 > R->z1) R->z1 = (int16_t)(j + 1);
        }
    for (int r = 0; r < L->nreg; ++r) if (!L->reg[r].cells) { L->reg[r].x0 = L->reg[r].z0 = L->reg[r].x1 = L->reg[r].z1 = 0; }
}

static void reset_attempt(Gen *G) {
    ILayout *L = G->L;
    int N = L->nx * L->nz;
    for (int c = 0; c < N; ++c) { L->own[c] = NONE; L->flag[c] = G->baseFlag[c]; }
    L->nreg = L->ndoor = L->nfix = L->nzone = L->ncore = L->ncol = 0;
    memset(&L->atrium, 0, sizeof(L->atrium));
    memset(G->npath, 0, sizeof(G->npath));
    memset(G->gateAt, 0, sizeof(G->gateAt));
    G->nneed = 0;
    G->alen = 0;
    G->abuf[0] = 0;
    G->why[0] = 0;
    add_region(G, IR_LOBBY, 0);          /* region 0 is always the lobby: whatever remains */
}

static int attempt_layout(Gen *G) {
    ILayout *L = G->L;
    choose_params(G);
    in_place_columns(G);
    int ncores = G->grammar == IG_SERVICE ? 1 : G->ntower > 0 ? G->ntower : 1;
    for (int k = 0; k < ncores; ++k) if (!place_core(G, k)) { if (k == 0) return 0; alog(G, "  simplify: second core dropped\n"); break; }
    if (!place_vestibule(G)) return 0;
    place_void(G);
    for (int k = 0; k < L->ncore; ++k) if (!plan_route(G, k)) return 0;
    if (G->gated) for (int k = 0; k < L->ncore; ++k) place_gates(G, k);
    /* connectivity plan (step 4) */
    int hasVest = 0;
    for (int r = 0; r < L->nreg; ++r) hasVest |= L->reg[r].type == IR_VESTIBULE;
    alog(G, "  info: connections: entrance%s -> lobby", hasVest ? " -> vestibule" : "");
    for (int k = 0; k < L->ncore; ++k) alog(G, " -> hall %d%s", k, L->core[k].gated ? " (gates)" : "");
    alog(G, "\n");
    /* back of house: rooms anchored to service doors, the escape stair, the rest */
    for (int s = 0; s < G->nserv; ++s) {
        int type = G->serv[s].type == DOOR_LOADING ? IR_LOADING : IR_BINS;
        int r0 = L->nreg;
        if (place_room(G, type, &G->serv[s])) {
            const SDoor *sd = &G->serv[s];
            int side = sd->side;
            int axis = (side == 0 || side == 2) ? 0 : 1;
            float along = axis == 0 ? sd->x - L->gx0 : sd->z - L->gz0;
            int at = (int)floorf((along - sd->w * 0.5f) / CELL + 0.5f), len = cells_of(sd->w);
            int line = side == 0 ? (int)floorf((sd->z - L->gz0) / CELL + 0.5f) : side == 2 ? (int)floorf((sd->z - L->gz0) / CELL + 0.5f)
                     : (int)floorf((sd->x - L->gx0) / CELL + 0.5f);
            add_door(G, ID_SERVICE, axis, line, at, len, r0, -1);
        }
    }
    if (G->grammar == IG_OFFICE && G->attempt < 4 && L->area >= 650.0f && L->core[0].stairs < 2) place_room(G, IR_ESCAPE, NULL);
    for (int k = 0; k < G->nrooms; ++k) place_room(G, G->rooms[k], NULL);
    absorb_pockets(G);
    absorb_slivers(G);
    absorb_pockets(G);
    for (int c = 0; c < L->nx * L->nz; ++c) if (L->occ[c] == IO_USABLE && L->own[c] == NONE) L->own[c] = 0;
    finish_regions(L);
    if (!place_room_doors(G)) return 0;
    /* fixtures, in priority order */
    if (G->grammar == IG_OFFICE) {
        float rx = 0.0f, rz = 0.0f;
        need_doors(G);
        int hasRec = G->reception && place_reception(G, &rx, &rz);
        if (G->waiting) place_waiting(G, rx, rz, hasRec);
        if (G->cafe) place_cafe(G, rx, rz, hasRec);
        place_decoration(G);
    }
    for (int r = 0; r < L->nreg; ++r) if (L->reg[r].kind == IK_ROOM && L->reg[r].cells) furnish_room(G, r);
    return validate(G);
}

int interior_plan(const Building *b, const BGraph *g, int si, int variant, ILayout *L, BLog *log) {
    memset(L, 0, offsetof(ILayout, occ));
    L->section = si;
    L->synthFace = -1;
    int secs[4], ns = interior_sections(g, secs, 4), mine = 0;
    for (int k = 0; k < ns; ++k) mine |= secs[k] == si;
    if (!mine) return 0;
    Gen *G = (Gen *)calloc(1, sizeof(Gen));
    if (!G) fatal("out of memory planning an interior");
    G->b = b; G->g = g; G->L = L; G->log = log; G->si = si;
    L->seed = hash_combine(hash_combine(hash_combine(b->seed, 0x1A7E0000u), (uint32_t)si), (uint32_t)variant);
    blog_printf(log, "interior %08x: section %d\n", L->seed, si);
    if (!prepare(G)) {
        blog_printf(log, "  reject: %s; no interior\n", G->why);
        free(G);
        return 0;
    }
    size_t N = (size_t)L->nx * (size_t)L->nz;
    for (int k = 0; k < 4; ++k) G->sat[k] = (int *)malloc(sizeof(int) * (size_t)(L->nx + 1) * (size_t)(L->nz + 1));
    G->dist = (float *)malloc(sizeof(float) * N);
    G->cost = (float *)malloc(sizeof(float) * N);
    G->hkey = (float *)malloc(sizeof(float) * N * 8);
    G->prev = (int *)malloc(sizeof(int) * (N + IN_MAX_DIM));
    G->heap = (int *)malloc(sizeof(int) * N * 8);
    G->mark = (uint8_t *)malloc(N);
    G->reach = (uint8_t *)malloc(N);
    for (int k = 0; k < IN_MAX_CORE; ++k) G->path[k] = (int *)malloc(sizeof(int) * N);
    if (!G->sat[0] || !G->sat[1] || !G->sat[2] || !G->sat[3] || !G->dist || !G->cost || !G->hkey || !G->prev || !G->heap || !G->mark || !G->reach || !G->path[0] || !G->path[1])
        fatal("out of memory planning an interior");
    blog_printf(log, "  info: %s grammar, %.0f m2, %d x %d cells, frame turned %d deg, %.0f floors served\n",
             G->grammar == IG_OFFICE ? "office" : "service", L->area, L->nx, L->nz, L->quarter * 90, L->floors);
    L->planned = 1;
    for (int at = 0; at < IN_ATTEMPTS; ++at) {
        G->attempt = at;
        init_streams(G);
        reset_attempt(G);
        if (attempt_layout(G)) {
            L->ok = 1;
            L->attempts = at + 1;
            blog_printf(log, " attempt %d:\n%s", at + 1, G->abuf);
            blog_printf(log, "  pass: %d regions, %d doors, %d fixtures\n", L->nreg, L->ndoor, L->nfix);
            break;
        }
        blog_printf(log, "  reject: attempt %d: %s\n", at + 1, G->why);
    }
    if (!L->ok) {
        /* an open floor plate */
        reset_attempt(G);
        for (int c = 0; c < (int)N; ++c) if (L->occ[c] == IO_USABLE) L->own[c] = 0;
        finish_regions(L);
        L->attempts = IN_ATTEMPTS;
        L->atrium.mode = (uint8_t)(g->groundMode == GF_DOUBLE ? IA_DOUBLE : IA_NONE);
        L->atrium.topY = L->ceilY;
        L->routeStart = G->entryCell;
        blog_printf(log, "  simplify: six attempts failed; open floor plate\n");
    }
    for (int k = 0; k < IN_MAX_CORE; ++k) free(G->path[k]);
    for (int k = 0; k < 4; ++k) free(G->sat[k]);
    free(G->dist); free(G->cost); free(G->hkey); free(G->prev); free(G->heap); free(G->mark); free(G->reach); free(G->cand);
    free(G);
    return 1;
}
