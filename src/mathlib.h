#pragma once
#include <math.h>

#define PI_F 3.14159265358979f

typedef struct { float x, y, z; } vec3;
typedef struct { float m[16]; } mat4;   /* column-major, m[col*4 + row] */

static inline vec3 v3(float x, float y, float z) { vec3 r = { x, y, z }; return r; }
static inline vec3 v3_add(vec3 a, vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline vec3 v3_sub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline vec3 v3_scale(vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline float v3_dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline vec3 v3_cross(vec3 a, vec3 b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float v3_len(vec3 a) { return sqrtf(v3_dot(a, a)); }
static inline vec3 v3_norm(vec3 a) { float l = v3_len(a); return l > 1e-8f ? v3_scale(a, 1.0f / l) : a; }

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float minf(float a, float b) { return a < b ? a : b; }
static inline float maxf(float a, float b) { return a > b ? a : b; }
static inline float smoothstepf(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
/* Frame-rate independent exponential approach: rate is 1/seconds. */
static inline float damp(float cur, float target, float rate, float dt) {
    return lerpf(target, cur, expf(-rate * dt));
}

static inline mat4 mat4_identity(void) {
    mat4 r = { { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 } };
    return r;
}

static inline mat4 mat4_mul(mat4 a, mat4 b) {
    mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

/* Right-handed, Vulkan clip space (Y down), reverse-Z: near -> 1, far -> 0. */
static inline mat4 mat4_perspective_rz(float fovy, float aspect, float zn, float zf) {
    float f = 1.0f / tanf(fovy * 0.5f);
    mat4 r = { { 0 } };
    r.m[0]  = f / aspect;
    r.m[5]  = -f;
    r.m[10] = zn / (zf - zn);
    r.m[11] = -1.0f;
    r.m[14] = zf * zn / (zf - zn);
    return r;
}

/* View rotation only (camera sits at the origin: camera-relative rendering). */
static inline mat4 mat4_view_rot(vec3 right, vec3 up, vec3 fwd) {
    mat4 r = mat4_identity();
    r.m[0] = right.x; r.m[4] = right.y; r.m[8]  = right.z;
    r.m[1] = up.x;    r.m[5] = up.y;    r.m[9]  = up.z;
    r.m[2] = -fwd.x;  r.m[6] = -fwd.y;  r.m[10] = -fwd.z;
    return r;
}

/* Frustum planes (a,b,c,d), inside when a*x+b*y+c*z+d >= 0. */
typedef struct { float p[6][4]; } Frustum;

static inline Frustum frustum_from_matrix(mat4 m) {
    Frustum f;
    float row[4][4];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) row[r][c] = m.m[c * 4 + r];
    for (int i = 0; i < 4; ++i) {
        f.p[0][i] = row[3][i] + row[0][i];  /* left   */
        f.p[1][i] = row[3][i] - row[0][i];  /* right  */
        f.p[2][i] = row[3][i] + row[1][i];  /* top/bottom (Y flipped, both kept) */
        f.p[3][i] = row[3][i] - row[1][i];
        f.p[4][i] = row[2][i];              /* far  (reverse-Z: z >= 0) */
        f.p[5][i] = row[3][i] - row[2][i];  /* near (z <= w) */
    }
    for (int i = 0; i < 6; ++i) {
        float l = sqrtf(f.p[i][0] * f.p[i][0] + f.p[i][1] * f.p[i][1] + f.p[i][2] * f.p[i][2]);
        for (int k = 0; k < 4; ++k) f.p[i][k] /= l;
    }
    return f;
}

static inline int frustum_test_aabb(const Frustum *f, vec3 mn, vec3 mx) {
    for (int i = 0; i < 6; ++i) {
        const float *p = f->p[i];
        vec3 v = v3(p[0] >= 0 ? mx.x : mn.x, p[1] >= 0 ? mx.y : mn.y, p[2] >= 0 ? mx.z : mn.z);
        if (p[0] * v.x + p[1] * v.y + p[2] * v.z + p[3] < 0) return 0;
    }
    return 1;
}
