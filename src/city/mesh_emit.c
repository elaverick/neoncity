#include "city_internal.h"

/* ===========================================================================
 * Mesh emission helpers
 * ========================================================================= */

void mesh_free(Mesh *m) { free(m->v); free(m->lights); free(m->sky); free(m->air); memset(m, 0, sizeof(*m)); }

void mesh_push(Mesh *m, vec3 p, vec3 n, float u, float v, uint32_t seed, const uint8_t prm[4], uint32_t aux) {
    if (m->count == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 8192;
        m->v = (Vertex *)realloc(m->v, m->cap * sizeof(Vertex));
        if (!m->v) fatal("out of memory building chunk mesh");
    }
    if (m->xf) {
        float x = p.x, z = p.z;
        p.x = m->xx + m->xc * x - m->xs * z;
        p.z = m->xz + m->xs * x + m->xc * z;
        x = n.x; z = n.z;
        n.x = m->xc * x - m->xs * z;
        n.z = m->xs * x + m->xc * z;
    }
    Vertex *o = &m->v[m->count++];
    o->pos[0] = p.x; o->pos[1] = p.y; o->pos[2] = p.z;
    o->nrm[0] = (int8_t)(n.x * 127.0f); o->nrm[1] = (int8_t)(n.y * 127.0f);
    o->nrm[2] = (int8_t)(n.z * 127.0f); o->nrm[3] = 0;
    o->uv[0] = u; o->uv[1] = v;
    o->seed = seed;
    memcpy(o->params, prm, 4);
    o->aux = aux;
    if (p.x < m->mn[0]) m->mn[0] = p.x;
    if (p.y < m->mn[1]) m->mn[1] = p.y;
    if (p.z < m->mn[2]) m->mn[2] = p.z;
    if (p.x > m->mx[0]) m->mx[0] = p.x;
    if (p.y > m->mx[1]) m->mx[1] = p.y;
    if (p.z > m->mx[2]) m->mx[2] = p.z;
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
    for (int i = 0; i < 3; ++i) mesh_push(m, p[k[i]], n, uv[k[i]][0], uv[k[i]][1], mt.seed, mt.p, mt.aux);
}

/* Convex polygon lying flat at height y, fan-triangulated, facing up. */
void emit_flat(Mesh *m, const DVec2 *p, int n, float y, const float (*uv)[2], Mat mt) {
    vec3 up = v3(0, 1, 0);
    for (int i = 1; i + 1 < n; ++i) {
        int k[3] = { 0, i, i + 1 };
        vec3 a = v3((float)p[0].x, y, (float)p[0].z), b = v3((float)p[i].x, y, (float)p[i].z), c = v3((float)p[i + 1].x, y, (float)p[i + 1].z);
        if (v3_dot(v3_cross(v3_sub(b, a), v3_sub(c, a)), up) < 0) { k[1] = i + 1; k[2] = i; }
        for (int j = 0; j < 3; ++j)
            mesh_push(m, v3((float)p[k[j]].x, y, (float)p[k[j]].z), up, uv[k[j]][0], uv[k[j]][1], mt.seed, mt.p, mt.aux);
    }
}
Xf xf_save(const Mesh *m) { Xf t; t.on = m->xf; t.c = m->xc; t.s = m->xs; t.x = m->xx; t.z = m->xz; return t; }
void xf_restore(Mesh *m, Xf t) { m->xf = t.on; m->xc = t.c; m->xs = t.s; m->xx = t.x; m->xz = t.z; }
void xf_enter(Mesh *m, float ox, float oz, float c, float s) {
    if (!m->xf) { m->xf = 1; m->xc = c; m->xs = s; m->xx = ox; m->xz = oz; return; }
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
