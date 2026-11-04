/* The player's wake through the grass: the trail (grass_wake.h). */
#include "grass_wake.h"
#include <math.h>
#include <string.h>

static float sat(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
static float ease(float a, float b, float x) { float t = sat((x - a) / (b - a)); return t * t * (3.0f - 2.0f * t); }
static double dist3(const double a[3], const double b[3]) {
    double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrt(dx * dx + dy * dy + dz * dz);
}

void grass_wake_reset(GrassWake *w) {
    memset(w, 0, sizeof(*w));
}

void grass_wake_step(GrassWake *w, const double feet[3], bool onFoot, double now) {
    if (onFoot && w->count > 0 && dist3(feet, w->p[w->head]) > GRASS_WAKE_JUMP) grass_wake_reset(w);
    /* the expired: weight 0 by now (grass_wake_frame), so they go unseen */
    while (w->count > 0) {
        int oldest = (w->head - (w->count - 1) + GRASS_WAKE_LAID) % GRASS_WAKE_LAID;
        if (now - w->t[oldest] < GRASS_WAKE_LIFE) break;
        w->count--;
    }
    if (onFoot && (w->count == 0 || dist3(feet, w->p[w->head]) >= GRASS_WAKE_STEP)) {
        w->head = (w->head + 1) % GRASS_WAKE_LAID;
        memcpy(w->p[w->head], feet, sizeof(w->p[0]));
        w->t[w->head] = now;
        if (w->count < GRASS_WAKE_LAID) w->count++;
    }
    w->live = onFoot;
    memcpy(w->feet, feet, sizeof(w->feet));
}

static void put(GrassWakeFrame *f, const double p[3], const double cam[3], float age, float weight) {
    float *o = f->p[f->n];
    o[0] = (float)(p[0] - cam[0]); o[1] = (float)(p[1] - cam[1]); o[2] = (float)(p[2] - cam[2]); o[3] = age;
    f->w[f->n] = weight;
    f->n++;
}

void grass_wake_frame(const GrassWake *w, const double cam[3], double now, GrassWakeFrame *f) {
    memset(f, 0, sizeof(*f));
    f->box[0] = f->box[1] = 1e30f; f->box[2] = f->box[3] = -1e30f;
    if (w->live) put(f, w->feet, cam, 0.0f, 1.0f);
    /* a laid point's rank: how many points newer, and on by the way to the next */
    float frac = w->count > 0 ? sat((float)(dist3(w->feet, w->p[w->head]) / GRASS_WAKE_STEP)) : 0.0f;
    for (int j = 0; j < w->count; ++j) {
        int slot = (w->head - j + GRASS_WAKE_LAID) % GRASS_WAKE_LAID;
        float age = (float)(now - w->t[slot]);
        float weight = (1.0f - ease((float)GRASS_WAKE_LAID - 5.0f, (float)GRASS_WAKE_LAID - 1.0f, (float)j + frac))
                     * (1.0f - ease((float)GRASS_WAKE_LIFE - 1.5f, (float)GRASS_WAKE_LIFE, age));
        put(f, w->p[slot], cam, age, weight);
    }
    /* the faded tail: one weightless point ends the path, the rest are nothing */
    while (f->n >= 2 && f->w[f->n - 1] <= 0.0f && f->w[f->n - 2] <= 0.0f) f->n--;
    if (f->n == 1 && f->w[0] <= 0.0f) f->n = 0;
    const float R = (float)GRASS_WAKE_RADIUS;   /* every point's, weighted or not: a segment carries its neighbour's weight */
    for (int i = 0; i < f->n; ++i) {
        const float *o = f->p[i];
        if (o[0] - R < f->box[0]) f->box[0] = o[0] - R;
        if (o[2] - R < f->box[1]) f->box[1] = o[2] - R;
        if (o[0] + R > f->box[2]) f->box[2] = o[0] + R;
        if (o[2] + R > f->box[3]) f->box[3] = o[2] + R;
    }
}

float grass_wake_recover(float age, float phase) {
    float om = (float)GRASS_WAKE_OMEGA * (0.85f + 0.3f * phase), l = (float)GRASS_WAKE_DAMP;
    age = fmaxf(age, 0.0f);
    return expf(-l * age) * (cosf(om * age) + l / om * sinf(om * age));
}

/* wake.glsl grassWakeAt */
void grass_wake_push(const GrassWakeFrame *f, const float p[3], float phase, float out[2]) {
    out[0] = out[1] = 0.0f;
    if (f->n < 2 || p[0] < f->box[0] || p[2] < f->box[1] || p[0] > f->box[2] || p[2] > f->box[3]) return;
    const float R2 = (float)(GRASS_WAKE_RADIUS * GRASS_WAKE_RADIUS);
    float vx = 0.0f, vz = 0.0f, most = 0.0f;
    for (int i = 0; i + 1 < f->n; ++i) {
        const float *a = f->p[i], *b = f->p[i + 1];
        float abx = b[0] - a[0], abz = b[2] - a[2];
        float s = sat(((p[0] - a[0]) * abx + (p[2] - a[2]) * abz) / fmaxf(abx * abx + abz * abz, 1e-6f));
        float dx = p[0] - (a[0] + abx * s), dz = p[2] - (a[2] + abz * s);
        float r2 = (dx * dx + dz * dz) / R2;
        if (r2 >= 1.0f) continue;
        float wt = f->w[i] + (f->w[i + 1] - f->w[i]) * s;
        float gap = a[1] + (b[1] - a[1]) * s - p[1];   /* the feet over the root: legs reach so far, a bank above them so high */
        float g = (1.0f - ease(0.6f, 1.2f, gap)) * (1.0f - ease(1.4f, 1.8f, -gap));
        float amt = (1.0f - r2) * (1.0f - r2) * wt * g * grass_wake_recover(a[3] + (b[3] - a[3]) * s, phase);
        float dl = sqrtf(dx * dx + dz * dz);
        float ux, uz;
        if (dl > 1e-4f) { ux = dx / dl; uz = dz / dl; }
        else { float al = sqrtf(abx * abx + abz * abz) + 1e-6f; ux = -abz / al; uz = abx / al; }   /* on the path: square to it */
        vx += amt * ux; vz += amt * uz;
        most = fmaxf(most, fabsf(amt));
    }
    float lv = sqrtf(vx * vx + vz * vz);
    float k = lv > most ? most / lv : 1.0f;
    out[0] = vx * k; out[1] = vz * k;
}
