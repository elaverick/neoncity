#include "lobby_doors.h"
#include "layouts.h"
#include "interior.h"
#include "doors.h"

/* The doors in the 3 x 3 chunks round the walker (the collision layouts'),
 * with their state, carried over by key as the walker moves. A door that
 * leaves them forgets its state: it is shut (sliding) or turning at its own
 * pace (revolving) when it comes back, as the shaders draw any door not in
 * the frame's list. */

#define NEAR_MAX      (9 * MAX_LOBBY_DOORS)
#define LOCK_MAX      256
#define TRIGGER_ALONG 1.2f    /* a sliding door opens for the walker this far past its opening's edges (m), */
#define TRIGGER_DEPTH 3.0f    /* ... and this far either side of it */
#define OPEN_TIME     0.9f    /* seconds to open */
#define CLOSE_TIME    1.6f    /* ... and to shut */
#define HOLD_TIME     1.5f    /* it stays open this long after the walker has gone */
#define LEAF_HALF     0.025f  /* a leaf's or wing's half thickness, to the walker */

typedef struct DoorState {
    LobbyDoor d;
    float     open, hold;     /* sliding: how far open (0..1, before easing); seconds it stays open */
    float     angle, offset;  /* revolving: its angle; its lag on interior_revolve_angle from being stopped */
} DoorState;

static DoorState g_near[NEAR_MAX], g_next[NEAR_MAX];
static int       g_nnear;
static double    g_eye[3];
static uint32_t  g_locked[LOCK_MAX];
static int       g_nlocked;
static bool      g_lockAll;

bool lobby_doors_locked(uint32_t key) {
    if (g_lockAll) return true;
    for (int i = 0; i < g_nlocked; ++i) if (g_locked[i] == key) return true;
    return false;
}

void lobby_doors_lock(uint32_t key, bool locked) {
    for (int i = 0; i < g_nlocked; ++i)
        if (g_locked[i] == key) {
            if (!locked) g_locked[i] = g_locked[--g_nlocked];
            return;
        }
    if (locked && g_nlocked < LOCK_MAX) g_locked[g_nlocked++] = key;
}

void lobby_doors_lock_all(bool locked) { g_lockAll = locked; }

static float eased(float t) { return t * t * (3.0f - 2.0f * t); }

void lobby_doors_update(const Player *p, double t, float dt) {
    int ccx = (int)floor(p->feet[0] / CHUNK_SIZE), ccz = (int)floor(p->feet[2] / CHUNK_SIZE), n = 0;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) {
            const BlockLayout *L = layouts_get(ccx + dx, ccz + dz);   /* valid until the next: copied at once */
            for (int i = 0; i < L->ndoor && n < NEAR_MAX; ++i) {
                DoorState *s = &g_next[n++];
                memset(s, 0, sizeof(*s));
                s->d = L->door[i];
                s->angle = interior_revolve_angle(s->d.key, t);
                for (int k = 0; k < g_nnear; ++k)
                    if (g_near[k].d.key == s->d.key) { s->open = g_near[k].open; s->hold = g_near[k].hold; s->angle = g_near[k].angle; s->offset = g_near[k].offset; break; }
            }
        }
    memcpy(g_near, g_next, sizeof(DoorState) * (size_t)n);
    g_nnear = n;
    memcpy(g_eye, p->eye, sizeof(g_eye));
    for (int i = 0; i < n; ++i) {
        DoorState *s = &g_near[i];
        bool locked = lobby_doors_locked(s->d.key);
        if (s->d.type == IE_REVOLVE) {
            float free_ = interior_revolve_angle(s->d.key, t);
            if (locked) s->offset = s->angle - free_;     /* stopped: when it goes again, it goes on from here */
            else s->angle = free_ + s->offset;
            continue;
        }
        double ox = p->feet[0] - s->d.c[0], oz = p->feet[2] - s->d.c[1];
        float along = (float)(ox * s->d.a[0] + oz * s->d.a[1]), across = (float)(ox * s->d.n[0] + oz * s->d.n[1]);
        float up = (float)(p->feet[1] - s->d.y);
        bool near = !locked && fabsf(along) < s->d.width * 0.5f + TRIGGER_ALONG && fabsf(across) < TRIGGER_DEPTH && up > -1.0f && up < 2.5f;
        s->hold = near ? HOLD_TIME : maxf(0.0f, s->hold - dt);
        float target = s->hold > 0.0f ? 1.0f : 0.0f;
        s->open = clampf(s->open + clampf(target - s->open, -dt / CLOSE_TIME, dt / OPEN_TIME), 0.0f, 1.0f);
    }
}

/* Push a circle out of the segment a-b, `half` thick. */
static void push_out_segment(double *x, double *z, double ax, double az, double bx, double bz, float half) {
    double ex = bx - ax, ez = bz - az, l2 = ex * ex + ez * ez;
    double t = l2 > 0.0 ? ((*x - ax) * ex + (*z - az) * ez) / l2 : 0.0;
    t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
    double qx = ax + t * ex, qz = az + t * ez, dx = *x - qx, dz = *z - qz, d = sqrt(dx * dx + dz * dz);
    if (d >= half || d < 1e-9) return;
    *x = qx + dx * half / d;
    *z = qz + dz * half / d;
}

void lobby_doors_collide(double *x, double *z, double feet, float r) {
    for (int i = 0; i < g_nnear; ++i) {
        const DoorState *s = &g_near[i];
        const LobbyDoor *d = &s->d;
        double ox = *x - d->c[0], oz = *z - d->c[1];
        float reach = interior_door_reach(d->type, d->leaves, d->width) + 1.0f;
        if (ox * ox + oz * oz > (double)reach * reach || feet < d->y - 1.0 || feet > d->y + d->height) continue;
        if (d->type == IE_REVOLVE) {
            float c = cosf(s->angle), sn = sinf(s->angle);
            for (int k = 0; k < d->leaves; ++k) {
                float p = interior_wing_angle(d->leaves, k);
                float wx = -cosf(p) * d->n[0] + sinf(p) * d->a[0], wz = -cosf(p) * d->n[1] + sinf(p) * d->a[1];
                float rx = wx * c - wz * sn, rz = wx * sn + wz * c;   /* as shaders/city.vert turns them */
                push_out_segment(x, z, d->c[0] + rx * 0.06, d->c[1] + rz * 0.06, d->c[0] + rx * (d->radius - 0.005), d->c[1] + rz * (d->radius - 0.005), r + LEAF_HALF);
            }
            continue;
        }
        float e = eased(s->open);
        for (int k = 0; k < d->leaves; ++k) {
            float s0, s1, tr;
            interior_leaf(d->leaves, d->outer, d->width, k, e, &s0, &s1, &tr);
            double bx = d->c[0] + d->n[0] * tr, bz = d->c[1] + d->n[1] * tr;
            push_out_segment(x, z, bx + d->a[0] * s0, bz + d->a[1] * s0, bx + d->a[0] * s1, bz + d->a[1] * s1, r + LEAF_HALF);
        }
    }
}

int lobby_doors_frame(uint32_t *keys, float (*state)[4]) {
    int idx[DOOR_SLOTS], n = 0;
    double dist[DOOR_SLOTS];
    for (int i = 0; i < g_nnear; ++i) {
        double dx = g_near[i].d.c[0] - g_eye[0], dz = g_near[i].d.c[1] - g_eye[2], d2 = dx * dx + dz * dz;
        if (n == DOOR_SLOTS && d2 >= dist[n - 1]) continue;
        int j = n < DOOR_SLOTS ? n++ : n - 1;   /* insertion, nearest first */
        while (j > 0 && dist[j - 1] > d2) { dist[j] = dist[j - 1]; idx[j] = idx[j - 1]; --j; }
        dist[j] = d2; idx[j] = i;
    }
    for (int k = 0; k < n; ++k) {
        const DoorState *s = &g_near[idx[k]];
        keys[k] = s->d.key;
        state[k][0] = eased(s->open);
        state[k][1] = s->angle;
        state[k][2] = state[k][3] = 0.0f;
    }
    return n;
}
