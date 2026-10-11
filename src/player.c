#include "player.h"
#include "city.h"

#define EYE_HEIGHT    1.68f
#define RADIUS        0.32f
#define WALK_SPEED    1.9f
#define SPRINT_SPEED  5.4f
#define FLY_SPEED     30.0f
#define GRAVITY       9.81f
#define JUMP_SPEED    3.9f
#define MOUSE_SENS    0.0022f
#define BASE_FOV      (72.0f * PI_F / 180.0f)

/* ------------------------------------------------------------------------
 * Collision world: block layouts are a pure function of chunk coordinates,
 * so collision just regenerates (and caches) the layouts around the player.
 * ---------------------------------------------------------------------- */

#define LAYOUT_CACHE 16
static BlockLayout g_layouts[LAYOUT_CACHE];
static int g_layoutValid[LAYOUT_CACHE];
static uint32_t g_layoutUse[LAYOUT_CACHE], g_useClock;

static const BlockLayout *layout_get(int cx, int cz) {
    int lru = 0;
    for (int i = 0; i < LAYOUT_CACHE; ++i) {
        if (g_layoutValid[i] && g_layouts[i].cx == cx && g_layouts[i].cz == cz) {
            g_layoutUse[i] = ++g_useClock;
            return &g_layouts[i];
        }
        if (!g_layoutValid[i] || g_layoutUse[i] < g_layoutUse[lru]) lru = i;
    }
    city_layout(cx, cz, &g_layouts[lru]);
    g_layoutValid[lru] = 1;
    g_layoutUse[lru] = ++g_useClock;
    return &g_layouts[lru];
}

static bool in_curb(const CurbPoly *c, double x, double z) {
    double sgn = 0.0;
    for (int i = 0; i < c->n; ++i) {
        const double *a = c->v[i], *b = c->v[(i + 1) % c->n];
        double cr = (b[0] - a[0]) * (z - a[1]) - (b[1] - a[1]) * (x - a[0]);
        if (cr == 0.0) continue;
        if (sgn == 0.0) sgn = cr;
        else if ((cr > 0.0) != (sgn > 0.0)) return false;
    }
    return c->n >= 3;
}

static float ground_height(double x, double z) {
    int cx = (int)floor(x / CHUNK_SIZE), cz = (int)floor(z / CHUNK_SIZE);
    const BlockLayout *L = layout_get(cx, cz);
    for (int i = 0; i < L->ncurbs; ++i)
        if (in_curb(&L->curb[i], x, z)) return SIDEWALK_HEIGHT;
    return 0.0f;
}

/* Push a circle out of an axis-aligned rectangle. */
static void push_out(float *px, float *pz, Rect r) {
    float cx = clampf(*px, r.x0, r.x1), cz = clampf(*pz, r.z0, r.z1);
    float dx = *px - cx, dz = *pz - cz;
    float d2 = dx * dx + dz * dz;
    if (d2 > RADIUS * RADIUS) return;
    if (d2 > 1e-8f) {
        float d = sqrtf(d2), k = (RADIUS - d) / d;
        *px += dx * k; *pz += dz * k;
    } else {
        /* centre inside the rectangle: leave by the shortest way */
        float l = *px - r.x0, rr = r.x1 - *px, b = *pz - r.z0, t = r.z1 - *pz;
        float m = minf(minf(l, rr), minf(b, t));
        if (m == l) *px = r.x0 - RADIUS; else if (m == rr) *px = r.x1 + RADIUS;
        else if (m == b) *pz = r.z0 - RADIUS; else *pz = r.z1 + RADIUS;
    }
}

/* Push out of a rectangle given in a rotated frame (c, s) centred on (ox, oz). */
static void push_out_frame(double *x, double *z, double ox, double oz, float c, float s, Rect r) {
    double dx = *x - ox, dz = *z - oz;
    float lx = (float)(c * dx + s * dz), lz = (float)(-s * dx + c * dz);
    if (lx < r.x0 - 1.0f || lx > r.x1 + 1.0f || lz < r.z0 - 1.0f || lz > r.z1 + 1.0f) return;
    float px = lx, pz = lz;
    push_out(&lx, &lz, r);
    if (lx == px && lz == pz) return;
    *x = ox + c * lx - s * lz;
    *z = oz + s * lx + c * lz;
}

static bool ring_inside(const double (*v)[2], int n, double x, double z) {
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++)
        if ((v[i][1] > z) != (v[j][1] > z) && x < v[j][0] + (z - v[j][1]) * (v[i][0] - v[j][0]) / (v[i][1] - v[j][1])) in = !in;
    return in;
}

static void ring_closest(const double (*v)[2], int n, double x, double z, double *best, double *qx, double *qz) {
    for (int i = 0, j = n - 1; i < n; j = i++) {
        double ex = v[i][0] - v[j][0], ez = v[i][1] - v[j][1], l2 = ex * ex + ez * ez;
        double t = l2 > 1e-12 ? ((x - v[j][0]) * ex + (z - v[j][1]) * ez) / l2 : 0.0;
        t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
        double cx = v[j][0] + ex * t, cz = v[j][1] + ez * t, d = (x - cx) * (x - cx) + (z - cz) * (z - cz);
        if (d < *best) { *best = d; *qx = cx; *qz = cz; }
    }
}

/* Push a circle out of a solid outline (with an optional courtyard). */
static void push_out_poly(double *x, double *z, const BlockLayout *L, const CollPoly *c) {
    const double (*outer)[2] = (const double (*)[2])L->collv[c->first];
    const double (*hole)[2] = (const double (*)[2])L->collv[c->holeFirst];
    double best = 1e300, qx = *x, qz = *z;
    ring_closest(outer, c->n, *x, *z, &best, &qx, &qz);
    if (c->holeN) ring_closest(hole, c->holeN, *x, *z, &best, &qx, &qz);
    bool inside = ring_inside(outer, c->n, *x, *z) && !(c->holeN && ring_inside(hole, c->holeN, *x, *z));
    double d = sqrt(best);
    if (!inside && d >= RADIUS) return;
    double dx = *x - qx, dz = *z - qz;
    if (d < 1e-6) return;
    double k = inside ? -1.0 / d : 1.0 / d;      /* inside: leave through the nearest wall */
    *x = qx + dx * k * RADIUS;
    *z = qz + dz * k * RADIUS;
}

/* Buildings belong to the chunk holding their centre and overhang by less
 * than a chunk, so the 3x3 chunks around the player cover everything. What
 * stands at street level (walls, piers, lift cores, columns) is solid; the
 * space under arcades, void decks and gateways is open. */
static void collide(double *x, double *z) {
    int ccx = (int)floor(*x / CHUNK_SIZE), ccz = (int)floor(*z / CHUNK_SIZE);
    const Rect pole = { -0.2f, -0.2f, 0.2f, 0.2f };
    for (int iter = 0; iter < 3; ++iter) {
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                const BlockLayout *L = layout_get(ccx + dx, ccz + dz);
                for (int i = 0; i < L->ncoll; ++i) push_out_poly(x, z, L, &L->coll[i]);
                for (int i = 0; i < L->nlamps; ++i) push_out_frame(x, z, L->lamp[i].x, L->lamp[i].z, 1.0f, 0.0f, pole);
            }
    }
}

/* ------------------------------------------------------------------------ */

void player_init(Player *p) {
    memset(p, 0, sizeof(*p));
    /* Start on a sidewalk in the central regional centre, looking along the street. */
    city_spawn(&p->feet[0], &p->feet[2], &p->yaw);
    p->feet[1] = SIDEWALK_HEIGHT;
    p->pitch = 0.04f;
    p->onGround = true;
    p->fovY = BASE_FOV;
}

void player_update(Player *p, const PlayerInput *in, float dt) {
    if (dt > 0.05f) dt = 0.05f;
    p->time += dt;
    if (in->toggleFly) { p->fly = !p->fly; p->vel[0] = p->vel[1] = p->vel[2] = 0; }

    /* --- look --- */
    p->yaw += in->lookDX * MOUSE_SENS;
    p->pitch -= in->lookDY * MOUSE_SENS;
    p->pitch = clampf(p->pitch, -1.53f, 1.53f);
    if (p->yaw > PI_F) p->yaw -= 2 * PI_F;
    if (p->yaw < -PI_F) p->yaw += 2 * PI_F;

    float sy = sinf(p->yaw), cy = cosf(p->yaw);
    vec3 flatFwd = v3(sy, 0, -cy), flatRight = v3(cy, 0, sy);
    vec3 look = v3(cosf(p->pitch) * sy, sinf(p->pitch), -cosf(p->pitch) * cy);

    float mx = in->moveX, mz = in->moveZ;
    float ml = sqrtf(mx * mx + mz * mz);
    if (ml > 1.0f) { mx /= ml; mz /= ml; }

    if (p->fly) {
        float sp = FLY_SPEED * (in->sprint ? 5.0f : 1.0f);
        vec3 t = v3_add(v3_scale(look, mz * sp), v3_scale(flatRight, mx * sp));
        if (in->jump) t.y += sp;
        if (in->down) t.y -= sp;
        for (int i = 0; i < 3; ++i) {
            float target = i == 0 ? t.x : i == 1 ? t.y : t.z;
            p->vel[i] = damp(p->vel[i], target, 6.0f, dt);
        }
        for (int i = 0; i < 3; ++i) p->feet[i] += p->vel[i] * dt;
        float gh = ground_height(p->feet[0], p->feet[2]) + 1.0f;
        if (p->feet[1] < gh) p->feet[1] = gh;
        p->bobWeight = damp(p->bobWeight, 0, 8.0f, dt);
    } else {
        /* --- walking --- */
        bool moving = ml > 0.01f;
        p->sprintBlend = damp(p->sprintBlend, (in->sprint && moving && mz > 0.2f) ? 1.0f : 0.0f, 4.0f, dt);
        float speed = lerpf(WALK_SPEED, SPRINT_SPEED, p->sprintBlend);
        vec3 target = v3_add(v3_scale(flatFwd, mz * speed), v3_scale(flatRight, mx * speed));
        /* Humans accelerate quickly and stop a little faster; very little air control. */
        float rate = p->onGround ? (moving ? 8.0f : 10.0f) : 1.2f;
        p->vel[0] = damp(p->vel[0], target.x, rate, dt);
        p->vel[2] = damp(p->vel[2], target.z, rate, dt);

        if (p->onGround && in->jump) { p->vel[1] = JUMP_SPEED; p->onGround = false; }
        p->vel[1] -= GRAVITY * dt;

        double nx = p->feet[0] + p->vel[0] * dt, nz = p->feet[2] + p->vel[2] * dt;
        collide(&nx, &nz);
        /* Recover velocity actually achieved so sliding along walls feels right. */
        p->vel[0] = (float)((nx - p->feet[0]) / dt);
        p->vel[2] = (float)((nz - p->feet[2]) / dt);
        p->feet[0] = nx; p->feet[2] = nz;

        float prevY = (float)p->feet[1];
        p->feet[1] += p->vel[1] * dt;
        float gh = ground_height(p->feet[0], p->feet[2]);
        bool wasGround = p->onGround;
        if (p->feet[1] <= gh) {
            /* Landing: dip the head proportionally to the impact speed. */
            if (!wasGround && p->vel[1] < -1.0f) p->eyeOffsetVel += p->vel[1] * 0.35f;
            /* Stepping up a curb: snap the feet, let the eye spring catch up. */
            if (wasGround && gh > prevY) p->eyeOffset -= gh - prevY;
            p->feet[1] = gh;
            p->vel[1] = 0;
            p->onGround = true;
        } else if (wasGround && p->vel[1] <= 0 && (float)p->feet[1] - gh < 0.35f) {
            /* Stepping down off a curb: stay glued to the ground, eye eases down. */
            p->eyeOffset += (float)p->feet[1] - gh;
            p->feet[1] = gh;
            p->vel[1] = 0;
            p->onGround = true;
        } else {
            p->onGround = false;
        }

        /* --- head bob --- */
        float hs = sqrtf(p->vel[0] * p->vel[0] + p->vel[2] * p->vel[2]);
        float stride = lerpf(0.78f, 1.35f, p->sprintBlend);              /* metres per step */
        float prevPhase = p->bobPhase;
        if (p->onGround) p->bobPhase += hs * dt / stride * PI_F;        /* one step per half cycle */
        if (floorf(prevPhase / PI_F) != floorf(p->bobPhase / PI_F)) p->footsteps++;
        float targetW = p->onGround ? clampf(hs / WALK_SPEED, 0.0f, 1.0f) : 0.0f;
        p->bobWeight = damp(p->bobWeight, targetW, 6.0f, dt);
    }

    /* Eye spring (critically damped) returns curb/landing offsets to rest. */
    {
        const float w = 16.0f;
        float a = -w * w * p->eyeOffset - 2.0f * w * p->eyeOffsetVel;
        p->eyeOffsetVel += a * dt;
        p->eyeOffset += p->eyeOffsetVel * dt;
        p->eyeOffset = clampf(p->eyeOffset, -0.5f, 0.5f);
    }

    /* Bob shape: |sin| gives a firm footfall at the bottom and a rounded top
     * (head is highest mid-stance); lateral sway and roll run at half the step
     * rate, toward the supporting foot. Amplitudes grow with sprinting. */
    float phi = p->bobPhase;
    float wgt = p->bobWeight;
    float s = p->sprintBlend;
    float vert   = (2.0f * fabsf(sinf(phi)) - 1.0f) * lerpf(0.013f, 0.028f, s) * wgt;
    float side   = sinf(phi) * lerpf(0.009f, 0.016f, s) * wgt;
    float rollB  = sinf(phi) * lerpf(0.003f, 0.006f, s) * wgt;
    float nod    = (fabsf(sinf(phi)) - 0.5f) * lerpf(0.0025f, 0.005f, s) * wgt;
    /* Idle breathing keeps the view alive when standing still. */
    float idle = 1.0f - wgt;
    vert += sinf(p->time * 1.35f) * 0.006f * idle;
    nod  += sinf(p->time * 1.35f + 0.6f) * 0.0025f * idle;

    /* Lean slightly into strafes; widen FOV a touch at sprint speed. */
    float lateral = p->vel[0] * flatRight.x + p->vel[2] * flatRight.z;
    p->strafeRoll = damp(p->strafeRoll, p->fly ? 0.0f : -lateral / SPRINT_SPEED * 0.035f, 7.0f, dt);
    p->fovKick = damp(p->fovKick, p->sprintBlend * (6.0f * PI_F / 180.0f), 5.0f, dt);
    p->fovY = BASE_FOV + p->fovKick;

    float pitch = clampf(p->pitch + nod, -1.55f, 1.55f);
    vec3 fwd = v3(cosf(pitch) * sy, sinf(pitch), -cosf(pitch) * cy);
    vec3 right = v3_norm(v3_cross(fwd, v3(0, 1, 0)));
    vec3 up = v3_cross(right, fwd);
    float roll = rollB + p->strafeRoll;
    float cr = cosf(roll), sr = sinf(roll);
    p->fwd = fwd;
    p->right = v3_add(v3_scale(right, cr), v3_scale(up, sr));
    p->up = v3_add(v3_scale(up, cr), v3_scale(right, -sr));

    float eyeH = p->fly ? 0.0f : EYE_HEIGHT;
    p->eye[0] = p->feet[0] + flatRight.x * side;
    p->eye[1] = p->feet[1] + eyeH + vert + p->eyeOffset;
    p->eye[2] = p->feet[2] + flatRight.z * side;
}
