#pragma once
#include "common.h"
#include "player.h"

/* The lobbies' automatic doors round the walker (LobbyDoor, in the chunks'
 * collision layouts; src/gen/interior_doors.c lays them out). A sliding door
 * opens as the walker comes near it, from either side, and shuts a while
 * after they have gone; a revolving door turns all the while. A locked door
 * does neither: a sliding one shuts, if it was open, and stays shut; a
 * revolving one stops where it is. Its leaves and wings stop the walker
 * where they stand at the time (lobby_doors_collide), and the frame carries
 * the state of the nearest (FrameUBO.doorKey, doorState) for shaders/city.vert
 * to draw them so. */

/* Once a frame, after the player moves: t the shaders' clock (FrameUBO.camPos.w). */
void lobby_doors_update(const Player *p, double t, float dt);
/* Pushes a circle r across at (x, z), feet at world height `feet`, out of the doors' leaves and wings. */
void lobby_doors_collide(double *x, double *z, double feet, float r);
/* The doors nearest the camera, at most DOOR_SLOTS: their keys, and how far
 * each is open (0..1, eased) and a revolving one's angle. Returns how many. */
int  lobby_doors_frame(uint32_t *keys, float (*state)[4]);

/* Locking, for what comes later (nothing locks a door yet but --lockdoors).
 * A door is locked if it is, or if all are. */
void lobby_doors_lock(uint32_t key, bool locked);
void lobby_doors_lock_all(bool locked);
bool lobby_doors_locked(uint32_t key);
