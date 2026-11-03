#include "interior_internal.h"
#include "doors.h"

/* ===========================================================================
 * Automatic doors (interior.h): which the entrance gets, where their parts
 * stand and move, and the check that nothing stands in their way.
 * ========================================================================= */

float interior_door_reach(int type, int leaves, float width) {
    float w2 = width * 0.5f;
    if (type == IE_REVOLVE) return w2 + 0.06f;                       /* the drum, its flanking glass and posts */
    return (leaves == 4 ? 1.5f * w2 : 2.0f * w2) + 0.07f + 0.1f;     /* the parked leaves, and the header's end past them */
}

void interior_leaf(int leaves, int outer, float width, int k, float t, float *s0, float *s1, float *track) {
    float w2 = width * 0.5f, a0, a1, travel;
    int half = leaves == 4 ? 2 : 1, side = k < half ? -1 : 1, fast = leaves == 4 && (k % 2) == 1;
    if (leaves != 4)  { a0 = 0.0f;               a1 = w2 + 0.05f;         travel = w2 + 0.02f; }
    else if (!fast)   { a0 = w2 * 0.5f - 0.02f;  a1 = w2 + 0.05f;         travel = w2 * 0.5f + 0.02f; }
    else              { a0 = 0.0f;               a1 = w2 * 0.5f + 0.02f;  travel = w2 + 0.02f; }
    a0 += travel * t; a1 += travel * t;
    if (side < 0) { *s0 = -a1; *s1 = -a0; } else { *s0 = a0; *s1 = a1; }
    float tr = (outer ? IE_TRACK : IE_INNER_TRACK) + (fast ? IE_TRACK_GAP : 0.0f);
    *track = outer ? tr : -tr;
}

float interior_wing_angle(int leaves, int k) {
    return leaves == 3 ? PI_F * 0.5f + (float)k * (2.0f * PI_F / 3.0f) : PI_F * 0.25f + (float)k * (PI_F * 0.5f);
}

/* The phase is the key's low 16 bits as a fraction of a turn (shaders/city.vert: doorAngle). */
float interior_revolve_angle(uint32_t key, double t) {
    double a = (double)DOOR_REVOLVE_RATE * t + (double)(key & 0xFFFFu) / 65536.0 * 2.0 * PI_F;
    return (float)fmod(a, 2.0 * PI_F);
}

int interior_auto_doors(const BGraph *g, const ILayout *L, IAutoDoor *out, int max) {
    int n = 0;
    if (!L->planned || L->entryDoor == IE_NONE) return 0;
    const BLoop *ol = &g->sec[L->section].foot.outer;
    for (int d = 0; d < g->ndoor && n < max; ++d) {
        const BDoor *dr = &g->door[d];
        if (dr->section != L->section || dr->type != DOOR_PRIMARY) continue;
        int k1 = (dr->face + 1) % ol->n;
        float ex = ol->x[k1] - ol->x[dr->face], ez = ol->z[k1] - ol->z[dr->face], len = sqrtf(ex * ex + ez * ez);
        if (len < 1e-3f) continue;
        ex /= len; ez /= len;
        IAutoDoor *o = &out[n++];
        memset(o, 0, sizeof(*o));
        o->key = hash_combine(L->seed, 0xD0040001u);
        o->type = L->entryDoor; o->leaves = L->entryLeaves; o->outer = 1;
        o->ax = ex; o->az = ez; o->nx = ez; o->nz = -ex;   /* the outward normal is (-ez, ex) */
        o->width = dr->width; o->height = dr->height; o->y0 = L->floorY;
        o->cx = ol->x[dr->face] + ex * dr->u; o->cz = ol->z[dr->face] + ez * dr->u;
        if (o->type == IE_REVOLVE) {
            o->radius = dr->width * 0.5f;
            o->cx += o->nx * (o->radius + IE_DRUM_GAP); o->cz += o->nz * (o->radius + IE_DRUM_GAP);
        }
        break;
    }
    for (int d = 0; d < L->ndoor && n < max; ++d) {
        const IDoor *dr = &L->door[d];
        if (dr->kind != ID_INNER || dr->axis != 0) continue;
        IAutoDoor *o = &out[n++];
        memset(o, 0, sizeof(*o));
        o->key = hash_combine(L->seed, 0xD0040002u);
        o->type = IE_SLIDE; o->outer = 0;
        o->width = (float)dr->len * CELL;
        o->leaves = L->innerLeaves;
        o->height = 2.6f;                                  /* the screen's transom (emit_partitions) */
        o->y0 = L->floorY;
        interior_to_local(L, L->gx0 + ((float)dr->at + (float)dr->len * 0.5f) * CELL, L->gz0 + (float)dr->line * CELL, &o->cx, &o->cz);
        interior_to_local(L, 1.0f, 0.0f, &o->ax, &o->az);
        interior_to_local(L, 0.0f, 1.0f, &o->nx, &o->nz);   /* the vestibule lies at lower z than the lobby */
    }
    return n;
}

/* ---------------------------------------------------------------------------
 * Planning
 * ------------------------------------------------------------------------- */

/* The entrance's door, from its own hash so it is the same in every attempt:
 * revolving now and then where the opening suits a drum, else sliding,
 * bi-parting if the opening is narrow and there is room beside it on the
 * face, telescopic if not; with no room for either, the doorway stays open.
 * u: the opening's middle along a face len long. */
void choose_entry_door(Gen *G, int ed, float u, float len, float w) {
    ILayout *L = G->L;
    L->entryDoor = IE_NONE;
    L->entryLeaves = 0;
    if (G->grammar != IG_OFFICE || ed < 0) return;
    float h = G->g->door[ed].height, room = minf(u, len - u) - 0.15f;   /* to the face's nearer end, less a jamb */
    G->entryRoom = room;
    int wings = w > 2.8f ? 3 : 4;
    int drum = w >= 2.2f && w <= 3.6f && h <= 3.0f && interior_door_reach(IE_REVOLVE, wings, w) <= room;
    float roll = hash_float(hash_combine(L->seed, 0xD00E5u));
    if (drum && roll < IE_REVOLVE_SHARE)                                          { L->entryDoor = IE_REVOLVE; L->entryLeaves = (uint8_t)wings; }
    else if (w <= IE_TELESCOPIC && interior_door_reach(IE_SLIDE, 2, w) <= room)   { L->entryDoor = IE_SLIDE; L->entryLeaves = 2; }
    else if (interior_door_reach(IE_SLIDE, 4, w) <= room)                         { L->entryDoor = IE_SLIDE; L->entryLeaves = 4; }
    else if (drum)                                                                { L->entryDoor = IE_REVOLVE; L->entryLeaves = (uint8_t)wings; }
    G->entryDoor = L->entryDoor; G->entryLeaves = L->entryLeaves;
    if (L->entryDoor == IE_NONE) blog_printf(G->log, "  simplify: no room beside the entrance for automatic doors; it stays an open doorway\n");
    else blog_printf(G->log, "  info: entrance: %s\n", L->entryDoor == IE_REVOLVE ? (L->entryLeaves == 3 ? "revolving door, three wings" : "revolving door, four wings")
                                                     : L->entryLeaves == 4 ? "telescopic sliding doors" : "bi-parting sliding doors");
}

/* How far a vestibule reaches either side of the entrance's middle: a metre
 * past the opening, or past its door's parts by IE_SCREEN_GAP to its side
 * screens. (Its inner door's needs place_vestibule meets after.) */
float vestibule_half(const ILayout *L, float w) {
    float half = w * 0.5f + 1.0f;
    if (L->entryDoor != IE_NONE) half = maxf(half, interior_door_reach(L->entryDoor, L->entryLeaves, w) + IE_SCREEN_GAP);
    return half;
}

/* Is the frame point (along, in) from the entrance within what its door keeps
 * clear (IF_ENTRY), beyond the first 3 m inside? Margins cover a whole cell
 * (0.25 m) and a little more, so a cell outside never touches the door. */
int entry_door_keep(const ILayout *L, float w, float along, float in) {
    /* the first 3 m, as wide as a vestibule round the door at its narrowest (telescopic), so a core keeps out of its way */
    if (L->entryDoor != IE_NONE && in < 3.0f &&
        along < interior_door_reach(L->entryDoor, L->entryDoor == IE_SLIDE ? 4 : L->entryLeaves, w) + IE_SCREEN_GAP + 0.25f) return 1;
    if (L->entryDoor == IE_SLIDE) {
        float depth = IE_TRACK + (L->entryLeaves == 4 ? IE_TRACK_GAP : 0.0f) + 0.11f;
        return in < depth + 0.4f && along < interior_door_reach(IE_SLIDE, L->entryLeaves, w) + 0.4f;
    }
    if (L->entryDoor == IE_REVOLVE) {
        float r = w * 0.5f;
        return in < 2.0f * r + IE_DRUM_GAP + 1.6f && along < interior_door_reach(IE_REVOLVE, L->entryLeaves, w) + 0.4f;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Validation: what a door sweeps, against what stands
 * ------------------------------------------------------------------------- */

/* What door d's parts sweep, in its own terms: s along a, q along n from c. */
static void door_swept(const IAutoDoor *d, float *s0, float *s1, float *q0, float *q1) {
    float reach = interior_door_reach(d->type, d->leaves, d->width);
    *s0 = -reach; *s1 = reach;
    if (d->type == IE_REVOLVE) { *q0 = -(d->radius + IE_DRUM_GAP) + 0.05f; *q1 = d->radius + 0.06f; return; }
    float t0 = (d->outer ? IE_TRACK : IE_INNER_TRACK) - 0.06f, t1 = (d->outer ? IE_TRACK : IE_INNER_TRACK) + (d->leaves == 4 ? IE_TRACK_GAP : 0.0f) + 0.11f;
    if (d->outer) { *q0 = t0; *q1 = t1; } else { *q0 = -t1; *q1 = -t0; }
}

typedef struct DoorFrame { float cx, cz, ax, az, nx, nz, s0, s1, q0, q1; } DoorFrame;

/* Does the frame rectangle overlap what the door sweeps? */
static int sweeps(const DoorFrame *f, float x0, float z0, float x1, float z1) {
    float smin = 1e30f, smax = -1e30f, qmin = 1e30f, qmax = -1e30f;
    for (int c = 0; c < 4; ++c) {
        float x = (c & 1) ? x1 : x0, z = (c & 2) ? z1 : z0, dx = x - f->cx, dz = z - f->cz;
        float s = dx * f->ax + dz * f->az, q = dx * f->nx + dz * f->nz;
        smin = minf(smin, s); smax = maxf(smax, s); qmin = minf(qmin, q); qmax = maxf(qmax, q);
    }
    const float e = 0.005f;
    return smax > f->s0 + e && smin < f->s1 - e && qmax > f->q0 + e && qmin < f->q1 - e;
}

int doors_clear(Gen *G) {
    ILayout *L = G->L;
    IAutoDoor dd[IE_MAX_DOORS];
    int nd = interior_auto_doors(G->g, L, dd, IE_MAX_DOORS);
    if (!nd) return 1;
    IWall *w = (IWall *)malloc(sizeof(IWall) * 4096);
    if (!w) fatal("out of memory validating an interior");
    int nw = interior_walls(L, w, 4096), ok = 1;
    for (int k = 0; k < nd && ok; ++k) {
        const IAutoDoor *d = &dd[k];
        const char *what = d->outer ? "the entrance's door" : "the vestibule's inner door";
        DoorFrame f;
        interior_to_frame(L, d->cx, d->cz, &f.cx, &f.cz);
        interior_to_frame(L, d->ax, d->az, &f.ax, &f.az);
        interior_to_frame(L, d->nx, d->nz, &f.nx, &f.nz);
        door_swept(d, &f.s0, &f.s1, &f.q0, &f.q1);
        /* inside the envelope, its corners and the middle of each side */
        for (int p = 0; p < 9 && ok; ++p) {
            float s = p % 3 == 0 ? f.s0 : p % 3 == 1 ? 0.5f * (f.s0 + f.s1) : f.s1, q = p / 3 == 0 ? f.q0 : p / 3 == 1 ? 0.5f * (f.q0 + f.q1) : f.q1;
            if (!bfoot_contains(&L->foot, f.cx + f.ax * s + f.nx * q, f.cz + f.az * s + f.nz * q)) ok = fail(G, "%s reaches out of the building", what);
        }
        for (int i = 0; i < nw && ok; ++i) {
            const IWall *r = &w[i];
            if (r->door == 2) continue;   /* gates: their speed gates are fixtures */
            float t = r->type == IB_WALL ? 0.07f : 0.04f, x0, z0, x1, z1;
            if (fabsf(r->z1 - r->z0) < 1e-4f) { x0 = r->x0; x1 = r->x1; z0 = r->z0 - t; z1 = r->z0 + t; }
            else                              { x0 = r->x0 - t; x1 = r->x0 + t; z0 = r->z0; z1 = r->z1; }
            if (sweeps(&f, minf(x0, x1), minf(z0, z1), maxf(x0, x1), maxf(z0, z1))) ok = fail(G, "%s passes through a partition", what);
        }
        for (int j = 0; j < L->nz && ok; ++j)
            for (int i = 0; i < L->nx && ok; ++i) {
                int c = IDX(L, i, j), o = L->own[c];
                int solid = L->occ[c] == IO_USABLE && o != NONE && L->reg[o].kind == IK_SOLID;
                if ((solid || (L->flag[c] & IF_COLUMN)) && sweeps(&f, L->gx0 + i * CELL, L->gz0 + j * CELL, L->gx0 + (i + 1) * CELL, L->gz0 + (j + 1) * CELL))
                    ok = fail(G, "%s passes through a %s", what, solid ? "core" : "column");
            }
        for (int x = 0; x < L->nfix && ok; ++x) {
            const IFixture *fx = &L->fix[x];
            if (fx->y1 > 0.02f && sweeps(&f, fx->x0, fx->z0, fx->x1, fx->z1)) ok = fail(G, "%s passes through furniture", what);
        }
    }
    free(w);
    return ok;
}
