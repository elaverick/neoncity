#ifndef DOORS_SHARED_H
#define DOORS_SHARED_H

/* Lobbies' automatic doors (src/gen/interior_doors.c). The one definition,
 * shared by C (src/app/doors.c, the frame's uniforms) and GLSL
 * (shaders/city.vert): #defines only, as in clouds.h.
 *
 * DOOR_SLOTS: how many doors near the camera the frame carries the state of
 * (FrameUBO.doorKey, doorState); a door's leaves look its key up there, and
 * one not there stands shut, or turns at its own pace if it revolves.
 * DOOR_REVOLVE_RATE: a revolving door's turn (radians a second, about 4 rpm),
 * from DOOR_PHASE of its key at time 0. */
#define DOOR_SLOTS        16
#define DOOR_REVOLVE_RATE 0.42f

#endif
