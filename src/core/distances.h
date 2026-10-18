#ifndef DISTANCES_H
#define DISTANCES_H

/* How far the world is drawn, and the clip planes that follow (metres). The
 * one definition, shared by C (src/core/common.h) and GLSL
 * (shaders/common.glsl): #defines only, in syntax both languages accept.
 *
 * VIEW_DISTANCE: how far the city is drawn. Long, but deliberately finite:
 * its haze reaches 100% exactly here so chunks streaming in at the edge are
 * never visible popping. It also sizes the city's chunk streaming, so it
 * can't simply grow.
 * TERRAIN_DISTANCE: how far the terrain is drawn, under a lighter haze of its
 * own beyond the city (shaders/terrain/terrain.frag) that is total here. It
 * must lie inside the coarsest clipmap level's window (src/render/terrain_gpu.c
 * checks). The far plane follows it. */
#define VIEW_DISTANCE     1900.0f
#define TERRAIN_DISTANCE  15000.0f
#define NEAR_PLANE        0.08f
#define FAR_PLANE         (TERRAIN_DISTANCE * 1.05f)

#endif
