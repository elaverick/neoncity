#pragma once
#include "common.h"
#include "mathlib.h"

typedef struct PlayerInput {
    float moveX, moveZ;      /* strafe (-1..1), forward (-1..1) */
    float lookDX, lookDY;    /* mouse delta in pixels */
    bool  sprint, jump, down;
    bool  toggleFly;
} PlayerInput;

typedef struct Player {
    double feet[3];          /* world position of the feet */
    float  vel[3];
    float  yaw, pitch;
    bool   onGround, fly;

    /* camera feel state */
    float  eyeOffset, eyeOffsetVel;  /* spring that absorbs curb steps and landings */
    float  bobPhase, bobWeight, sprintBlend;
    float  strafeRoll, fovKick, time;
    uint32_t footsteps;              /* increments on every footfall (hook for audio later) */

    /* outputs */
    double eye[3];
    vec3   fwd, right, up;
    float  fovY;
} Player;

void player_init(Player *p);
void player_update(Player *p, const PlayerInput *in, float dt);
