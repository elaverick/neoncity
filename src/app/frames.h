#pragma once
#include "player.h"
#include "plan.h"

/* The active frame (src/terrain/terrain.h, TerrainFrame): what world X/Z
 * means for the camera, the chunks, the clipmap and everything else drawn.
 *
 * In or near a settlement, within PLAN_FRAME_NEAR over the sphere of the
 * disc its footprint lies in (plan_settlement_reach), it is that
 * settlement's own frame, where its city is laid out (the nearest's, where
 * two are that near; FRAME_SWITCH_MARGIN keeps the camera in the one it is
 * in until the other is nearer by that much, and it stays in it to
 * FRAME_STAY further out). Out in the land it is a
 * travel frame: the chart about the place the camera was when it left the
 * last frame, its axes there the last frame's and that place keeping its
 * (x, z), so the camera neither moves nor turns as it changes; a new one
 * once the camera strays TRAVEL_RECENTRE from its origin, so the chart's
 * distortion round the camera (what terrain_bend leaves out) stays small.
 *
 * On a change the player is taken into the new frame through the sphere:
 * its feet, its heading and its velocity, the same place and the same way on
 * the planet. The caller tells the renderer (renderer_set_frame).
 *
 * `ahead` is the frame the camera may change into next that throws away what
 * was made in this one (a settlement's of another lineage): the nearest
 * settlement's, while the camera is within FRAME_AHEAD of where it would
 * change into it, else NULL. The renderer makes ready in it beforehand
 * (renderer_frame_ahead). */
#define FRAME_SWITCH_MARGIN 2000.0    /* m */
#define FRAME_AHEAD         20000.0   /* m: 1 s ahead at fly mode's fastest at 20 km up, several minutes at street level */
#define FRAME_STAY          5000.0    /* a settlement's frame is kept this far past PLAN_FRAME_NEAR of it (m) */
#define TRAVEL_RECENTRE     20000.0   /* m */
#define TRAVEL_RING         8         /* travel frames kept alive: the oldest is freed when a new one is made */

typedef struct Frames {
    const TerrainFrame *active;
    const TerrainFrame *ahead;      /* see above */
    int                 n;          /* settlements */
    TerrainDir          disc[PLAN_MAX_SETTLEMENTS];    /* each one's disc's centre */
    double              reach[PLAN_MAX_SETTLEMENTS];   /* and how far from it its footprint can reach (m) */
    TerrainFrame        travel[TRAVEL_RING];
    int                 next;       /* the ring's slot for the next travel frame */
    int                 switches;   /* so far */
} Frames;

/* Starts in settlement k's frame (the world's for 0) and activates it. */
void frames_init(Frames *f, int settlement);
/* After the player has moved: the frame it should be in. On a change, takes
 * the player into it, activates it and returns 1. Sets `ahead` either way. */
int  frames_update(Frames *f, Player *p);
/* A line of text: the frame (its settlement's name, or travel) and the switches so far. */
void frames_describe(const Frames *f, char *buf, size_t n);
/* Frees the travel frames' roads (after every worker has stopped). */
void frames_shutdown(Frames *f);
