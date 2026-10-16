#include "city_internal.h"

/* ---------------------------------------------------------------------------
 * Lobby plants (src/gen/plant.c). A planter is a stone pot with a top dressing,
 * part of the lobby; the plants are a layer of their own (city_build_plants),
 * drawn without back-face culling because a leaf is a single surface, so the
 * chunk's plant budget can choose their detail. Planters set out together hold
 * one kind of plant, matched in size: a row along the frontage, the pair
 * either side of a lift hall. A row repeats a few specimens, each turned its
 * own way. Each plant is sized to its ceiling and to the open floor round its
 * planter, so it does not grow through a wall, a core or the glass.
 * ------------------------------------------------------------------------- */

/* How far the open lobby reaches round a planter: the distance to the nearest
 * cell that is outside, at the envelope, a column, a core or a room. */
static float planter_clearance(const ILayout *L, const IFixture *f) {
    float cx = 0.5f * (f->x0 + f->x1), cz = 0.5f * (f->z0 + f->z1), best = 2.5f;
    int ci = (int)floorf((cx - L->gx0) / IN_CELL), cj = (int)floorf((cz - L->gz0) / IN_CELL), R = (int)ceilf(best / IN_CELL);
    for (int j = cj - R; j <= cj + R; ++j)
        for (int i = ci - R; i <= ci + R; ++i) {
            float x0 = L->gx0 + (float)i * IN_CELL, z0 = L->gz0 + (float)j * IN_CELL;
            float dx = maxf(maxf(x0 - cx, cx - (x0 + IN_CELL)), 0.0f), dz = maxf(maxf(z0 - cz, cz - (z0 + IN_CELL)), 0.0f);
            float d = sqrtf(dx * dx + dz * dz);
            if (d >= best) continue;
            int blocked = 1;
            if (i >= 0 && j >= 0 && i < L->nx && j < L->nz) {
                int c = j * L->nx + i;
                blocked = L->occ[c] != IO_USABLE || (L->flag[c] & (IF_COLUMN | IF_EDGE)) || L->own[c] == 255 ||
                          interior_boundary(L, f->region, L->own[c]) > IB_OPEN;
            }
            if (blocked) best = d;
        }
    return best;
}

static void record_plant(Mesh *m, const BGraph *g, const ILayout *L, const IFixture *f, Planting *pl) {
    if (pl->n == pl->cap) {
        pl->cap = pl->cap ? pl->cap * 2 : 32;
        pl->s = (PlantSpot *)realloc(pl->s, sizeof(PlantSpot) * (size_t)pl->cap);
        if (!pl->s) fatal("out of memory planting a lobby");
    }
    PlantSpot *sp = &pl->s[pl->n++];
    memset(sp, 0, sizeof(*sp));
    float lx, lz;
    interior_to_local(L, 0.5f * (f->x0 + f->x1), 0.5f * (f->z0 + f->z1), &lx, &lz);
    float yaw = hash_float(hash3i(L->seed, 0x9A77, f->group, f->member)) * 2.0f * PI_F;
    if (m->xf) {
        sp->x = m->xx + m->xc * lx - m->xs * lz;
        sp->z = m->xz + m->xs * lx + m->xc * lz;
        yaw += atan2f(m->xs, m->xc);
    } else { sp->x = lx; sp->z = lz; }
    sp->y = L->floorY + f->y0 + POT_HEIGHT - 0.05f;
    sp->c = cosf(yaw); sp->s = sinf(yaw);
    sp->clear = planter_clearance(L, f);
    sp->group = f->group; sp->member = f->member;
    sp->bseed = g->seed;
    sp->ceil = q_half(L->ceilY - L->floorY);
}

/* Which plant each group of the lobby's planters holds, and how big. Groups
 * whose planters stand within 4.5 m of each other (a frontage planter near a
 * lift hall's, say) are planted as one, so planters that read as a pair match. */
static void plan_planting(Planting *pl, int first, const ILayout *L) {
    static const int ROW[] = { PS_KENTIA, PS_KENTIA, PS_BENJAMINA, PS_BENJAMINA, PS_LYRATA, PS_LYRATA, PS_DRACAENA, PS_SCHEFFLERA, PS_SCHEFFLERA, PS_ZZ };
    static const int PAIR[] = { PS_LYRATA, PS_LYRATA, PS_LYRATA, PS_KENTIA, PS_KENTIA, PS_BENJAMINA, PS_BENJAMINA, PS_DRACAENA, PS_DRACAENA, PS_SCHEFFLERA, PS_ZZ };
    static const int SINGLE[] = { PS_ZZ, PS_ZZ, PS_ZZ, PS_DRACAENA, PS_DRACAENA, PS_SCHEFFLERA, PS_SCHEFFLERA, PS_LYRATA };
    static const int NARROW[] = { -1, PS_DRACAENA, PS_LYRATA };
    int root[4] = { 0, 1, 2, 3 }, kind[4] = { -1, -1, -1, -1 };
    float height[4] = { 0 }, width[4] = { 0 }, clear[4] = { 9.0f, 9.0f, 9.0f, 9.0f };
    for (int i = first; i < pl->n; ++i)
        for (int j = i + 1; j < pl->n; ++j) {
            int a = root[pl->s[i].group & 3], b = root[pl->s[j].group & 3];
            float dx = pl->s[i].x - pl->s[j].x, dz = pl->s[i].z - pl->s[j].z;
            if (a == b || dx * dx + dz * dz > 4.5f * 4.5f) continue;
            int lo = a < b ? a : b, hi = a < b ? b : a;
            for (int g = 0; g < 4; ++g) if (root[g] == hi) root[g] = lo;
        }
    for (int i = first; i < pl->n; ++i) { int g = root[pl->s[i].group & 3]; clear[g] = minf(clear[g], pl->s[i].clear); }
    /* room below the ceiling, or below a gallery's soffit */
    float room = (L->ceilY - L->floorY) - POT_HEIGHT - 0.3f;
    if (L->atrium.mode == IA_GALLERY && L->atrium.x1 > L->atrium.x0) room = minf(room, L->atrium.galleryY - 0.35f - L->floorY - POT_HEIGHT - 0.3f);
    for (int grp = IP_ROW; grp <= IP_SINGLE; ++grp) {
        if (root[grp] != grp || clear[grp] > 8.0f) continue;
        uint32_t h = hash_combine(L->seed ^ 0x504C414Eu, (uint32_t)grp);
        const int *tab = grp == IP_ROW ? ROW : grp == IP_PAIR ? PAIR : SINGLE;
        int n = grp == IP_ROW ? (int)ARRAY_LEN(ROW) : grp == IP_PAIR ? (int)ARRAY_LEN(PAIR) : (int)ARRAY_LEN(SINGLE);
        int sp = tab[h % (uint32_t)n];
        if (grp == IP_PAIR && sp == kind[IP_ROW]) sp = tab[hash_u32(h) % (uint32_t)n];   /* the lift halls usually differ from the frontage */
        /* the ceiling caps the height, the open floor round the planters the spread */
        float H = 0.0f, W = 0.0f;
        for (int t = 0; t < 3; ++t) {
            int cand = t == 0 ? sp : NARROW[t];
            const PlantInfo *pi = plant_info(cand);
            float ht = minf(pi->refHeight * (0.88f + 0.24f * hash_float(hash_combine(h, 7u))), room);
            float wt = clear[grp] / maxf(pi->spread * ht * 0.5f, 0.05f);
            if (t == 0 || wt > W) { sp = cand; H = ht; W = wt; }
            if (W >= 0.6f) break;
        }
        kind[grp] = sp; height[grp] = H; width[grp] = clampf(W, 0.45f, 1.15f);
    }
    for (int i = first; i < pl->n; ++i) {
        PlantSpot *s = &pl->s[i];
        int own = s->group & 3, grp = root[own];
        if (kind[grp] < 0 || height[grp] < 0.5f) { s->tier = -1; continue; }
        uint32_t seed = hash3i(L->seed, 0x5EED, own, own == IP_ROW ? s->member % 3 : s->member);
        plant_defaults(&s->pp, kind[grp], seed);
        s->pp.height = height[grp];
        s->pp.widthScale = width[grp];
        s->pp.floorDepth = POT_HEIGHT - 0.05f;
        s->tier = PQ_MEDIUM;
    }
}

static void emit_plant(Mesh *m, const PlantMesh *pm, const PlantSpot *sp) {
    for (uint32_t i = 0; i < pm->count; ++i) {
        const PlantVertex *v = &pm->v[i];
        float x = v->p[0], z = v->p[2];
        vec3 p = v3(sp->x + sp->c * x - sp->s * z, sp->y + v->p[1], sp->z + sp->s * x + sp->c * z);
        vec3 n = v3(sp->c * v->n[0] - sp->s * v->n[2], v->n[1], sp->s * v->n[0] + sp->c * v->n[2]);
        uint8_t prm[4] = { MAT_INTERIOR, (uint8_t)(v->part == PP_LEAF ? IS_PLANT : IS_BARK), v->kind, sp->ceil };
        uint32_t aux = (uint32_t)v->tint | ((uint32_t)v->organ << 8) | ((uint32_t)sp->pp.species << 24);
        mesh_push(m, p, n, v->uv[0], v->uv[1], sp->bseed, prm, aux);
        m->v[m->count - 1].nrm[3] = (int8_t)(v->ao >> 1);   /* the crown's openness (city.vert passes it on) */
    }
}

/* Plants grown in this chunk, by their parameters: a row's repeated specimens are grown once. */
typedef struct Grown { PlantParams pp; PlantMesh mesh; int ok; } Grown;

static const Grown *grow(Grown **cache, int *n, int *cap, const PlantParams *pp) {
    for (int i = 0; i < *n; ++i) if (!memcmp(&(*cache)[i].pp, pp, sizeof(*pp))) return &(*cache)[i];
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *cache = (Grown *)realloc(*cache, sizeof(Grown) * (size_t)*cap);
        if (!*cache) fatal("out of memory growing plants");
    }
    Grown *gr = &(*cache)[(*n)++];
    gr->pp = *pp;
    gr->ok = plant_build(pp, &gr->mesh);
    return gr;
}

/* Grows the chunk's plants within its vertex budget: medium detail if it
 * fits, then the frontage rows at low detail, then all of them, then each
 * held to an equal share; failing that, plants are left out, the rows' first. */
void grow_plants(Mesh *m, Planting *pl) {
    if (!pl->n) return;
    int avail = PLANT_BUDGET - (int)m->count, n = 0, cap = 0;
    Grown *cache = NULL;
    int planted = 0;
    for (int i = 0; i < pl->n; ++i) planted += pl->s[i].tier >= 0;
    for (int round = 0; round < 4; ++round) {
        if (round == 1) for (int i = 0; i < pl->n; ++i) if (pl->s[i].group == IP_ROW && pl->s[i].tier > PQ_LOW) pl->s[i].tier = PQ_LOW;
        if (round == 2) for (int i = 0; i < pl->n; ++i) if (pl->s[i].tier > PQ_LOW) pl->s[i].tier = PQ_LOW;
        if (round == 3) for (int i = 0; i < pl->n; ++i) pl->s[i].pp.maxTris = avail * 19 / 20 / 3 / (planted ? planted : 1);
        int total = 0;
        for (int i = 0; i < pl->n; ++i) {
            if (pl->s[i].tier < 0) continue;
            pl->s[i].pp.quality = pl->s[i].tier;
            const Grown *gr = grow(&cache, &n, &cap, &pl->s[i].pp);
            total += gr->ok ? (int)gr->mesh.count : 0;
        }
        if (total <= avail) break;
    }
    for (int pass = 0; pass < 2; ++pass)   /* pairs and singles, then the rows */
        for (int i = 0; i < pl->n; ++i) {
            PlantSpot *sp = &pl->s[i];
            if (sp->tier < 0 || (sp->group == IP_ROW) != (pass == 1)) continue;
            const Grown *gr = grow(&cache, &n, &cap, &sp->pp);
            if (!gr->ok || (int)gr->mesh.count > avail) continue;
            emit_plant(m, &gr->mesh, sp);
            avail -= (int)gr->mesh.count;
        }
    for (int i = 0; i < n; ++i) plant_mesh_free(&cache[i].mesh);
    free(cache);
}

/* The lobby's planters, and what grows in them. */
void collect_plants(Mesh *m, const BGraph *g, const ILayout *L, Planting *pl) {
    int first = pl->n;
    for (int f = 0; f < L->nfix; ++f) if (L->fix[f].type == IX_PLANTER) record_plant(m, g, L, &L->fix[f], pl);
    plan_planting(pl, first, L);
}
