#pragma once
#include <stdbool.h>
#include "grass_rules.h"

/* The player's wake through the grass (GR8; docs/procedural_grass_rendering_implementation.md 9).
 *
 * The trail: where the feet have been over the last GRASS_WAKE_LIFE seconds,
 * a point laid every GRASS_WAKE_STEP metres walked, in a ring, each with the
 * time it was laid; the feet themselves are its head while the player is on
 * foot (not flying). Each frame the trail goes to the GPU camera-relative,
 * newest first, with each point's age and weight (FrameUBO.wake, wakeW,
 * wakeBox, wakeInfo), and shaders/grass/wake.glsl bends every blade near the
 * path away from it, recovering by the age the path has there.
 *
 * Nothing pops. A point is laid where the feet are, so the path it splits
 * was already through it at age 0; a point's weight falls to 0 over its last
 * ranks in the ring (counted on from where the feet are since the newest was
 * laid, so the rank moves smoothly) and over its last 1.5 s of life, so it is
 * gone before it is overwritten or dropped. Feet more than GRASS_WAKE_JUMP from
 * the newest point (a teleport, a frame change, landing from flight) start a
 * new trail rather than join the old one across the gap.
 *
 * Pure: no clock of its own (the caller passes the time, real seconds, not
 * the shaders' --time); grass_wake_push is wake.glsl's grassWakeAt, step for
 * step, for grass_test. */

#define GRASS_WAKE_LAID (GRASS_WAKE_POINTS - 1)

typedef struct GrassWake {
    double p[GRASS_WAKE_LAID][3];   /* laid points, world */
    double t[GRASS_WAKE_LAID];      /* when each was laid (s) */
    int    head, count;             /* the newest laid point's slot; how many are held */
    bool   live;                    /* on foot: the feet are the trail's head */
    double feet[3];                 /* the feet at the last step */
} GrassWake;

/* The trail as the GPU takes it (FrameUBO's wake fields). */
typedef struct GrassWakeFrame {
    float p[GRASS_WAKE_POINTS][4];  /* camera-relative x, y, z of the path's points, newest first; w the age (s) */
    float w[GRASS_WAKE_POINTS];     /* each point's weight (0 .. 1) */
    float box[4];                   /* camera-relative min x, min z, max x, max z the path can reach (padded by the radius) */
    int   n;                        /* how many points */
} GrassWakeFrame;

void  grass_wake_reset(GrassWake *w);
/* A frame: the feet at world feet at time now (s); onFoot false while flying (the trail stays and fades). */
void  grass_wake_step(GrassWake *w, const double feet[3], bool onFoot, double now);
/* The trail for the frame, camera-relative to cam. */
void  grass_wake_frame(const GrassWake *w, const double cam[3], double now, GrassWakeFrame *f);
/* A blade's recovery a s after the feet passed (1 at once, towards 0), its own phase (0..1) tuning its spring. */
float grass_wake_recover(float age, float phase);
/* The wake's push at camera-relative root p: out the direction it bends, along the ground (x, z), times how far
 * (-1 .. 1 of GRASS_WAKE_BEND x the height x the species' interaction; under 0 swinging back past upright). */
void  grass_wake_push(const GrassWakeFrame *f, const float p[3], float phase, float out[2]);
