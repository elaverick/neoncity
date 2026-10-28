#include "frames.h"
#include "plan.h"
#include <stdio.h>

/* See frames.h. */

static double dot(TerrainDir a, TerrainDir b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

void frames_init(Frames *f, int settlement) {
    memset(f, 0, sizeof(*f));
    f->n = plan_settlement_count();
    for (int k = 0; k < f->n; ++k) {
        PlanSettlement s = plan_settlement(k);
        f->disc[k] = terrain_frame_dir(plan_frame_settlement(k), s.disc.x, s.disc.z);
        f->reach[k] = plan_settlement_reach(k);
    }
    f->active = plan_frame_settlement(settlement);
    terrain_frame_activate(f->active);
}

/* How far direction p lies over the sphere beyond the disc settlement k's
 * footprint lies in (m; < 0 inside it). */
static double beyond(const Frames *f, int k, TerrainDir p) {
    double c = dot(p, f->disc[k]);
    return PLANET_RADIUS * acos(c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c) - f->reach[k];
}

/* The player moved from frame `from` into `to`: the same place, heading and
 * velocity on the sphere, through the local frames there. */
static void carry(Player *p, const TerrainFrame *from, const TerrainFrame *to) {
    TerrainDir ax0, up0, az0, ax1, up1, az1;
    TerrainDir d = terrain_frame_dir(from, p->feet[0], p->feet[2]);
    terrain_frame_axes(from, p->feet[0], p->feet[2], &ax0, &up0, &az0);
    double x, z;
    terrain_dir_frame(to, d, &x, &z);
    terrain_frame_axes(to, x, z, &ax1, &up1, &az1);
    /* the heading: forward is (sin yaw, -cos yaw) on the local x and z */
    double sy = sin(p->yaw), cy = cos(p->yaw);
    TerrainDir h = { sy * ax0.x - cy * az0.x, sy * ax0.y - cy * az0.y, sy * ax0.z - cy * az0.z };
    float yaw = (float)atan2(dot(h, ax1), -dot(h, az1)), turn = yaw - p->yaw;
    p->yaw = yaw;
    TerrainDir v = { p->vel[0] * ax0.x + p->vel[2] * az0.x, p->vel[0] * ax0.y + p->vel[2] * az0.y, p->vel[0] * ax0.z + p->vel[2] * az0.z };
    p->vel[0] = (float)dot(v, ax1);
    p->vel[2] = (float)dot(v, az1);
    /* the camera's outputs turned as the heading was: its axes, and the eye about the feet */
    float c = cosf(turn), s = sinf(turn);
    vec3 *axes[3] = { &p->fwd, &p->right, &p->up };
    for (int k = 0; k < 3; ++k) {
        vec3 a = *axes[k];
        *axes[k] = v3(a.x * c - a.z * s, a.y, a.x * s + a.z * c);
    }
    double ex = p->eye[0] - p->feet[0], ez = p->eye[2] - p->feet[2];
    p->eye[0] = x + ex * c - ez * s;
    p->eye[2] = z + ex * s + ez * c;
    p->feet[0] = x;
    p->feet[2] = z;
}

/* A travel frame about where the player stands in frame `from`: that place
 * keeps its (x, z) and the axes there are from's, so nothing moves. */
static const TerrainFrame *make_travel(Frames *f, const Player *p, const TerrainFrame *from) {
    TerrainFrame *t = &f->travel[f->next];
    if (t->roads) plan_frame_free(t);   /* TRAVEL_RING frames ago: nothing works in it any more */
    f->next = (f->next + 1) % TRAVEL_RING;
    TerrainChart ch;
    TerrainDir up;
    ch.o = terrain_frame_dir(from, p->feet[0], p->feet[2]);
    terrain_frame_axes(from, p->feet[0], p->feet[2], &ch.ex, &up, &ch.ez);
    plan_frame_travel(t, &ch, p->feet[0], p->feet[2], from->lineage);
    return t;
}

int frames_update(Frames *f, Player *p) {
    const TerrainFrame *cur = f->active, *next = cur;
    double x = p->feet[0], z = p->feet[2];
    TerrainDir d = terrain_frame_dir(cur, x, z);
    int k = 0;
    double dk = 1e300;
    for (int q = 0; q < f->n; ++q) {
        double b = beyond(f, q, d);
        if (b < dk) { dk = b; k = q; }
    }
    if (cur->settlement >= 0) {
        double own = beyond(f, cur->settlement, d);
        if (k != cur->settlement && dk < PLAN_FRAME_NEAR && dk < own - FRAME_SWITCH_MARGIN) next = plan_frame_settlement(k);
        else if (own >= PLAN_FRAME_NEAR + FRAME_STAY) next = NULL;   /* out in the land */
    } else if (dk < PLAN_FRAME_NEAR) {
        next = plan_frame_settlement(k);
    } else if (hypot(x - cur->x0, z - cur->z0) > TRAVEL_RECENTRE) {
        next = NULL;
    }
    if (next == cur) return 0;
    if (!next) next = make_travel(f, p, cur);   /* the player keeps its place, heading and velocity */
    else carry(p, cur, next);
    f->active = next;
    f->switches++;
    terrain_frame_activate(next);
    return 1;
}

void frames_describe(const Frames *f, char *buf, size_t n) {
    if (f->active->settlement >= 0) snprintf(buf, n, "%s's frame", plan_settlement_name(f->active->settlement));
    else snprintf(buf, n, "a travel frame");
    size_t used = strlen(buf);
    snprintf(buf + used, n - used, ", %d switches", f->switches);
}

void frames_shutdown(Frames *f) {
    for (int k = 0; k < TRAVEL_RING; ++k)
        if (f->travel[k].roads) plan_frame_free(&f->travel[k]);
}
