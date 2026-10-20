#include "city_internal.h"

/* ===========================================================================
 * Mesh emission helpers
 * ========================================================================= */

void mesh_free(Mesh *m) { free(m->v); free(m->lights); free(m->sky); free(m->air); free(m->flame); free(m->plume); memset(m, 0, sizeof(*m)); }

void mesh_begin(Mesh *m, int cx, int cz) {
    m->count = 0;
    m->xf = 0;
    m->xy = 0.0f;
    m->drape = 0;
    m->footing = 0;
    m->grounded = 0;
    m->ox = cx * (double)CHUNK_SIZE; m->oz = cz * (double)CHUNK_SIZE;
    m->cell = city_drape_cell(m->ox + 0.5 * CHUNK_SIZE, m->oz + 0.5 * CHUNK_SIZE);
    m->sink = city_footing_sink(m->ox + 0.5 * CHUNK_SIZE, m->oz + 0.5 * CHUNK_SIZE);
    m->mn[0] = m->mn[1] = m->mn[2] = 1e30f;
    m->mx[0] = m->mx[1] = m->mx[2] = -1e30f;
    free(m->lights);
    m->lights = NULL;
    m->nlights = 0;
}

float mesh_ground_local(const Mesh *m, float x, float z) {
    if (!m->xf) return city_ground_y(m->ox + x, m->oz + z);
    return city_ground_y(m->ox + m->xx + m->xc * x - m->xs * z, m->oz + m->xz + m->xs * x + m->xc * z) - m->xy;
}

void mesh_push(Mesh *m, vec3 p, vec3 n, float u, float v, uint32_t seed, const uint8_t prm[4], uint32_t aux) {
    mesh_push_w(m, p, n, 0, u, v, seed, prm, aux);
}

void mesh_push_w(Mesh *m, vec3 p, vec3 n, int8_t w, float u, float v, uint32_t seed, const uint8_t prm[4], uint32_t aux) {
    if (m->count == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 8192;
        m->v = (Vertex *)realloc(m->v, m->cap * sizeof(Vertex));
        if (!m->v) fatal("out of memory building chunk mesh");
    }
    /* a wall's foot at street level, not a door (doors stand on their step: emit_walls) */
    int foot = m->footing && m->xf && p.y <= SIDEWALK_HEIGHT + 1e-3f && fabsf(n.y) < 0.5f && prm[0] != MAT_DOOR;
    if (m->xf) {
        float x = p.x, z = p.z;
        p.x = m->xx + m->xc * x - m->xs * z;
        p.z = m->xz + m->xs * x + m->xc * z;
        x = n.x; z = n.z;
        n.x = m->xc * x - m->xs * z;
        n.z = m->xs * x + m->xc * z;
        p.y += m->xy;
    }
    if (foot) p.y = minf(p.y, city_ground_y(m->ox + p.x, m->oz + p.z) + (SIDEWALK_HEIGHT - m->sink));
    float base = m->xf ? m->xy : m->grounded ? m->ground : city_ground_y(m->ox + p.x, m->oz + p.z);
    if (m->drape) {
        p.y += base;
        if (n.y != 0.0f) {   /* tilted with the ground (a vertical face stays as it is) */
            float gx, gz;
            city_ground_slope(m->ox + p.x, m->oz + p.z, &gx, &gz);
            n = v3_norm(v3(n.x - gx * n.y, n.y, n.z - gz * n.y));
        }
    }
    Vertex *o = &m->v[m->count++];
    o->pos[0] = p.x; o->pos[1] = p.y; o->pos[2] = p.z;
    o->nrm[0] = (int8_t)(n.x * 127.0f); o->nrm[1] = (int8_t)(n.y * 127.0f);
    o->nrm[2] = (int8_t)(n.z * 127.0f); o->nrm[3] = w;
    o->uv[0] = u; o->uv[1] = v;
    o->seed = seed;
    memcpy(o->params, prm, 4);
    o->aux = aux;
    o->base = base;
    if (p.x < m->mn[0]) m->mn[0] = p.x;
    if (p.y < m->mn[1]) m->mn[1] = p.y;
    if (p.z < m->mn[2]) m->mn[2] = p.z;
    if (p.x > m->mx[0]) m->mx[0] = p.x;
    if (p.y > m->mx[1]) m->mx[1] = p.y;
    if (p.z > m->mx[2]) m->mx[2] = p.z;
}

/* ---------------------------------------------------------------------------
 * Draping (Mesh.drape): a vertex is raised by the ground under it, but a
 * triangle is flat between its vertices, so a draped polygon is first cut
 * along a grid of squares (Mesh.cell, city_drape_cell). Each piece then follows the ground's
 * gentle curve to about a millimetre, and polygons that share an edge, cut by
 * the same lines, share the vertices along it.
 * ------------------------------------------------------------------------- */

#define DRAPE_MAXV 64
typedef struct DrapeV { vec3 p; float u, v; } DrapeV;

static int drape_before(const DrapeV *a, const DrapeV *b) {
    return a->p.x != b->p.x ? a->p.x < b->p.x : a->p.z != b->p.z ? a->p.z < b->p.z : a->p.y < b->p.y;
}

/* The part of convex polygon in where sign * (x or z) <= sign * lim. A cut
 * edge is interpolated from its lower end, so both polygons sharing it get
 * the same point, and the point lies on the line exactly. */
static int drape_clip(const DrapeV *in, int n, int axis, float sign, float lim, DrapeV *out) {
    int k = 0;
    for (int i = 0; i < n && k + 2 <= DRAPE_MAXV; ++i) {
        const DrapeV *a = &in[i], *b = &in[(i + 1) % n];
        float da = sign * ((axis ? a->p.z : a->p.x) - lim), db = sign * ((axis ? b->p.z : b->p.x) - lim);
        if (da <= 0.0f) out[k++] = *a;
        if ((da <= 0.0f) != (db <= 0.0f)) {
            if (drape_before(b, a)) { const DrapeV *t = a; a = b; b = t; float d = da; da = db; db = d; }
            float t = da / (da - db);
            DrapeV *o = &out[k++];
            o->p = v3_add(a->p, v3_scale(v3_sub(b->p, a->p), t));
            o->u = a->u + (b->u - a->u) * t;
            o->v = a->v + (b->v - a->v) * t;
            if (axis) o->p.z = lim; else o->p.x = lim;
        }
    }
    return k;
}

/* Convex polygon p (wound to face along nrm) cut along the drape grid and fanned. */
static void emit_draped(Mesh *m, const DrapeV *p, int n, vec3 nrm, Mat mt) {
    const float c = m->cell;
    float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
    for (int i = 0; i < n; ++i) {
        x0 = minf(x0, p[i].p.x); x1 = maxf(x1, p[i].p.x);
        z0 = minf(z0, p[i].p.z); z1 = maxf(z1, p[i].p.z);
    }
    /* a face lying on a grid line is cut once, not on both sides of it */
    int i0 = (int)floorf(x0 / c), i1 = (int)ceilf(x1 / c) - 1, j0 = (int)floorf(z0 / c), j1 = (int)ceilf(z1 / c) - 1;
    if (i1 < i0) i1 = i0;
    if (j1 < j0) j1 = j0;
    DrapeV a[DRAPE_MAXV], col[DRAPE_MAXV], cell[DRAPE_MAXV];
    for (int i = i0; i <= i1; ++i) {
        int nc = drape_clip(p, n, 0, -1.0f, i * c, a);
        nc = drape_clip(a, nc, 0, 1.0f, (i + 1) * c, col);
        if (nc < 3) continue;
        for (int j = j0; j <= j1; ++j) {
            int k = drape_clip(col, nc, 1, -1.0f, j * c, a);
            k = drape_clip(a, k, 1, 1.0f, (j + 1) * c, cell);
            for (int t = 1; t + 1 < k; ++t) {
                const DrapeV *v[3] = { &cell[0], &cell[t], &cell[t + 1] };
                if (v3_len(v3_cross(v3_sub(v[1]->p, v[0]->p), v3_sub(v[2]->p, v[0]->p))) < 1e-6f) continue;   /* a sliver along a cut */
                for (int q = 0; q < 3; ++q) mesh_push(m, v[q]->p, nrm, v[q]->u, v[q]->v, mt.seed, mt.p, mt.aux);
            }
        }
    }
}

Mat mat(uint8_t id, uint32_t seed, uint8_t a, uint8_t b, uint8_t c) {
    Mat m; m.seed = seed; m.p[0] = id; m.p[1] = a; m.p[2] = b; m.p[3] = c; m.aux = 0;
    return m;
}

/* Quad a,b,c,d (in perimeter order). Winding is fixed up so the front face
 * always points along n, regardless of the order the corners were given. */
void emit_quad(Mesh *m, vec3 a, vec3 b, vec3 c, vec3 d, vec3 n, const float uv[4][2], Mat mt) {
    vec3 p[4] = { a, b, c, d };
    int idx[4] = { 0, 1, 2, 3 };
    if (v3_dot(v3_cross(v3_sub(b, a), v3_sub(c, a)), n) < 0) { idx[1] = 3; idx[3] = 1; }
    if (m->drape) {
        DrapeV d[4];
        for (int i = 0; i < 4; ++i) { d[i].p = p[idx[i]]; d[i].u = uv[idx[i]][0]; d[i].v = uv[idx[i]][1]; }
        emit_draped(m, d, 4, n, mt);
        return;
    }
    static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; ++i) {
        int k = idx[tri[i]];
        mesh_push(m, p[k], n, uv[k][0], uv[k][1], mt.seed, mt.p, mt.aux);
    }
}

void emit_tri(Mesh *m, vec3 a, vec3 b, vec3 c, vec3 n, const float uv[3][2], Mat mt) {
    int k[3] = { 0, 1, 2 };
    vec3 p[3] = { a, b, c };
    if (v3_dot(v3_cross(v3_sub(b, a), v3_sub(c, a)), n) < 0) { k[1] = 2; k[2] = 1; }
    if (m->drape) {
        DrapeV d[3];
        for (int i = 0; i < 3; ++i) { d[i].p = p[k[i]]; d[i].u = uv[k[i]][0]; d[i].v = uv[k[i]][1]; }
        emit_draped(m, d, 3, n, mt);
        return;
    }
    for (int i = 0; i < 3; ++i) mesh_push(m, p[k[i]], n, uv[k[i]][0], uv[k[i]][1], mt.seed, mt.p, mt.aux);
}

/* Convex polygon lying flat at height y, fan-triangulated, facing up. */
void emit_flat(Mesh *m, const DVec2 *p, int n, float y, const float (*uv)[2], Mat mt) {
    vec3 up = v3(0, 1, 0);
    if (m->drape) {
        DrapeV d[DRAPE_MAXV];
        if (n > DRAPE_MAXV - 4) n = DRAPE_MAXV - 4;   /* room for the cuts */
        double turn = 0.0;   /* twice the signed area seen from above: negative faces up */
        for (int i = 0; i < n; ++i) turn += p[i].x * p[(i + 1) % n].z - p[(i + 1) % n].x * p[i].z;
        for (int i = 0; i < n; ++i) {
            int k = turn > 0.0 ? n - 1 - i : i;
            d[i].p = v3((float)p[k].x, y, (float)p[k].z); d[i].u = uv[k][0]; d[i].v = uv[k][1];
        }
        emit_draped(m, d, n, up, mt);
        return;
    }
    for (int i = 1; i + 1 < n; ++i) {
        int k[3] = { 0, i, i + 1 };
        vec3 a = v3((float)p[0].x, y, (float)p[0].z), b = v3((float)p[i].x, y, (float)p[i].z), c = v3((float)p[i + 1].x, y, (float)p[i + 1].z);
        if (v3_dot(v3_cross(v3_sub(b, a), v3_sub(c, a)), up) < 0) { k[1] = i + 1; k[2] = i; }
        for (int j = 0; j < 3; ++j)
            mesh_push(m, v3((float)p[k[j]].x, y, (float)p[k[j]].z), up, uv[k[j]][0], uv[k[j]][1], mt.seed, mt.p, mt.aux);
    }
}
Xf xf_save(const Mesh *m) { Xf t; t.on = m->xf; t.c = m->xc; t.s = m->xs; t.x = m->xx; t.z = m->xz; t.y = m->xy; return t; }
void xf_restore(Mesh *m, Xf t) { m->xf = t.on; m->xc = t.c; m->xs = t.s; m->xx = t.x; m->xz = t.z; m->xy = t.y; }
void xf_enter(Mesh *m, float ox, float oz, float c, float s) {
    if (!m->xf) { m->xf = 1; m->xc = c; m->xs = s; m->xx = ox; m->xz = oz; m->xy = 0.0f; return; }
    float nc = m->xc * c - m->xs * s, ns = m->xs * c + m->xc * s;
    float nx = m->xx + m->xc * ox - m->xs * oz, nz = m->xz + m->xs * ox + m->xc * oz;
    m->xc = nc; m->xs = ns; m->xx = nx; m->xz = nz;
}

/* Vertical face uv: u = metres from the face's left edge (seen from outside),
 * v = absolute height. Tangent is cross(up, normal), matching the shader. */
void emit_box(Mesh *m, vec3 mn, vec3 mx, int faces, Mat mt, int uvmode) {
    vec3 N[6] = { {-1,0,0}, {1,0,0}, {0,0,-1}, {0,0,1}, {0,1,0}, {0,-1,0} };
    for (int f = 0; f < 6; ++f) {
        if (!(faces & (1 << f))) continue;
        vec3 n = N[f], c[4];
        if (f == 0 || f == 1) {
            float x = f == 0 ? mn.x : mx.x;
            c[0] = v3(x, mn.y, mn.z); c[1] = v3(x, mn.y, mx.z); c[2] = v3(x, mx.y, mx.z); c[3] = v3(x, mx.y, mn.z);
        } else if (f == 2 || f == 3) {
            float z = f == 2 ? mn.z : mx.z;
            c[0] = v3(mn.x, mn.y, z); c[1] = v3(mx.x, mn.y, z); c[2] = v3(mx.x, mx.y, z); c[3] = v3(mn.x, mx.y, z);
        } else {
            float y = f == 4 ? mx.y : mn.y;
            c[0] = v3(mn.x, y, mn.z); c[1] = v3(mx.x, y, mn.z); c[2] = v3(mx.x, y, mx.z); c[3] = v3(mn.x, y, mx.z);
        }
        float uv[4][2];
        if (f < 4) {
            vec3 t = v3_cross(v3(0, 1, 0), n);
            float umin = 1e30f, umax = -1e30f;
            for (int i = 0; i < 4; ++i) { float u = v3_dot(c[i], t); umin = minf(umin, u); umax = maxf(umax, u); }
            for (int i = 0; i < 4; ++i) {
                float u = v3_dot(c[i], t) - umin;
                if (uvmode == UV_UNIT) { uv[i][0] = u / maxf(umax - umin, 1e-4f); uv[i][1] = (c[i].y - mn.y) / maxf(mx.y - mn.y, 1e-4f); }
                else { uv[i][0] = u; uv[i][1] = c[i].y; }
            }
        } else {
            for (int i = 0; i < 4; ++i) {
                if (uvmode == UV_UNIT) { uv[i][0] = (c[i].x - mn.x) / maxf(mx.x - mn.x, 1e-4f); uv[i][1] = (c[i].z - mn.z) / maxf(mx.z - mn.z, 1e-4f); }
                else { uv[i][0] = c[i].x; uv[i][1] = c[i].z; }
            }
        }
        emit_quad(m, c[0], c[1], c[2], c[3], n, uv, mt);
    }
}

Face face_local(float width) {
    Face f; f.o = v3(0, 0, 0); f.t = v3(1, 0, 0); f.n = v3(0, 0, 1); f.width = width;
    return f;
}

static vec3 face_point(const Face *f, float u, float y, float out) {
    return v3_add(v3_add(f->o, v3_scale(f->t, u)), v3_add(v3(0, y, 0), v3_scale(f->n, out)));
}

void push_light(SkyLight **arr, uint32_t *n, uint32_t *cap, const SkyLight *L) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *arr = (SkyLight *)realloc(*arr, *cap * sizeof(SkyLight));
        if (!*arr) fatal("out of memory building chunk lights");
    }
    (*arr)[(*n)++] = *L;
}

/* A neon sign or video screen lights the rain mist (an air light) and, if it
 * can reach the cloud layer, the clouds (a sky light; src/core/common.h): its
 * centre and facing in chunk-local space (through the current emission
 * transform, as mesh_push), and its power, from the sign's average emitted
 * radiance (shadeNeon / shadeScreen) and its area. */
void sign_lights(Mesh *m, vec3 c, vec3 n, float area, Mat mt) {
    int screen = mt.p[0] == MAT_SCREEN;
    float power = area * (screen ? 0.7f : 1.0f);
    float reach = sky_light_range(power);
    if (m->xf) {
        float x = c.x, z = c.z;
        c.x = m->xx + m->xc * x - m->xs * z;
        c.z = m->xz + m->xs * x + m->xc * z;
        x = n.x; z = n.z;
        n.x = m->xc * x - m->xs * z;
        n.z = m->xs * x + m->xc * z;
        c.y += m->xy;
    }
    SkyLight L;
    L.pos[0] = c.x; L.pos[1] = c.y; L.pos[2] = c.z;
    L.power = power;
    L.nx = n.x; L.nz = n.z;
    L.seed = mt.seed;
    L.kind = screen ? SKY_KIND_SCREEN : (uint32_t)((mt.p[1] & 7) | (((mt.p[1] >> 6) & 3) << 3));
    L.kind |= AIR_SIGN << 8;
    push_light(&m->air, &m->nair, &m->capair, &L);
    if (c.y + reach >= CLOUD_LOW && c.y - reach <= CLOUD_HIGH) push_light(&m->sky, &m->nsky, &m->capsky, &L);
}

/* A flare's fire, its nozzle at c (through the emission transform, as
 * mesh_push): the volume the flame pass marches, a light for the rain mist and,
 * if it can reach them, the clouds, all with timing id (shaders/common.glsl:
 * flameGout). Its light in the chunks' light tables is light_tiles.c's. */
void push_flame(Mesh *m, vec3 c, float reach, float radius, uint32_t id) {
    if (m->xf) {
        float x = c.x, z = c.z;
        c.x = m->xx + m->xc * x - m->xs * z;
        c.z = m->xz + m->xs * x + m->xc * z;
        c.y += m->xy;
    }
    if (m->nflame == m->capflame) {
        m->capflame = m->capflame ? m->capflame * 2 : 4;
        m->flame = (Flame *)realloc(m->flame, m->capflame * sizeof(Flame));
        if (!m->flame) fatal("out of memory building chunk flames");
    }
    Flame *f = &m->flame[m->nflame++];
    memset(f, 0, sizeof(*f));
    f->pos[0] = c.x; f->pos[1] = c.y; f->pos[2] = c.z;
    f->reach = reach; f->radius = radius; f->id = id & 63u;
    /* at the nozzle: clouds.comp's flameAt raises it with the fire (nx carries the reach) */
    SkyLight L = { { c.x, c.y, c.z }, 260.0f, reach, 0.0f, id & 63u, AIR_FLAME << 8 };
    push_light(&m->air, &m->nair, &m->capair, &L);
    L.power = 900.0f;
    float r = sky_light_range(L.power);
    if (L.pos[1] + r >= CLOUD_LOW && L.pos[1] - r <= CLOUD_HIGH) push_light(&m->sky, &m->nsky, &m->capsky, &L);
}

/* A stack's plume, its mouth at c (through the emission transform, as
 * mesh_push): the volume the smoke pass marches (shaders/smoke.frag). */
void push_plume(Mesh *m, vec3 c, float radius, float rise, uint32_t seed, uint32_t kind) {
    if (m->xf) {
        float x = c.x, z = c.z;
        c.x = m->xx + m->xc * x - m->xs * z;
        c.z = m->xz + m->xs * x + m->xc * z;
        c.y += m->xy;
    }
    if (m->nplume == m->capplume) {
        m->capplume = m->capplume ? m->capplume * 2 : 4;
        m->plume = (Plume *)realloc(m->plume, m->capplume * sizeof(Plume));
        if (!m->plume) fatal("out of memory building chunk plumes");
    }
    Plume *p = &m->plume[m->nplume++];
    memset(p, 0, sizeof(*p));
    p->pos[0] = c.x; p->pos[1] = c.y; p->pos[2] = c.z;
    p->radius = radius; p->rise = rise; p->seed = seed; p->kind = kind;
}

/* Flat quad parallel to a face: u in [u0,u1], y in [y0,y1], offset `out`. uv normalised 0..1. */
void emit_face_panel(Mesh *m, const Face *f, float u0, float u1, float y0, float y1, float out, Mat mt) {
    float uv[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
    emit_quad(m, face_point(f, u0, y0, out), face_point(f, u1, y0, out),
              face_point(f, u1, y1, out), face_point(f, u0, y1, out), f->n, uv, mt);
    if (mt.p[0] == MAT_NEON || mt.p[0] == MAT_SCREEN)
        sign_lights(m, face_point(f, 0.5f * (u0 + u1), 0.5f * (y0 + y1), out), f->n, fabsf((u1 - u0) * (y1 - y0)), mt);
}

uint8_t q_half(float metres) { float v = metres * 2.0f + 0.5f; return (uint8_t)(v > 255 ? 255 : v); }

void emit_emissive_box(Mesh *m, vec3 c, float half, uint8_t color, uint8_t mode, uint8_t phase) {
    emit_box(m, v3(c.x - half, c.y - half, c.z - half), v3(c.x + half, c.y + half, c.z + half),
             F_NOBOT | F_BOT, mat(MAT_EMISSIVE, 0, color, mode, phase), UV_UNIT);
}

/* Square-section beam between two points (raked struts). */
void emit_beam(Mesh *m, vec3 a, vec3 b, float size, Mat mt) {
    vec3 ax = v3_norm(v3_sub(b, a));
    vec3 h = v3_cross(ax, v3(0, 1, 0));
    h = v3_len(h) < 1e-3f ? v3(1, 0, 0) : v3_norm(h);
    vec3 v = v3_norm(v3_cross(h, ax));
    float r = size * 0.5f;
    vec3 o[4] = { v3_add(v3_scale(h, r), v3_scale(v, r)), v3_add(v3_scale(h, -r), v3_scale(v, r)),
                  v3_add(v3_scale(h, -r), v3_scale(v, -r)), v3_add(v3_scale(h, r), v3_scale(v, -r)) };
    float uv[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
    for (int i = 0; i < 4; ++i) {
        int j = (i + 1) & 3;
        emit_quad(m, v3_add(a, o[i]), v3_add(a, o[j]), v3_add(b, o[j]), v3_add(b, o[i]), v3_norm(v3_add(o[i], o[j])), uv, mt);
    }
}
