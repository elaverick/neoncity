#ifndef DISTANCES_H
#define DISTANCES_H

/* How far the world is drawn, and the clip planes that follow (metres). The
 * one definition, shared by C (src/core/common.h) and GLSL
 * (shaders/common.glsl): #defines only, in syntax both languages accept.
 *
 * VIEW_DISTANCE: how far the city's chunks are drawn in full. Beyond it, and
 * wherever a chunk hasn't streamed in yet, the city stands as its distant
 * proxies (src/render/proxy_gpu.c), out to TERRAIN_DISTANCE. It also sizes
 * the city's chunk streaming, so it can't simply grow.
 * TERRAIN_DISTANCE: how far the distant proxies reach, and the least the
 * terrain clipmap's coarsest window must (src/render/terrain_gpu.c checks).
 * Beyond the window the planet's patches (src/render/planet_gpu.c) carry the
 * ground to the horizon.
 * MAX_ALTITUDE: how high the camera can go (fly mode): aircraft height.
 * HORIZON_DISTANCE: how far the ground can be seen from there: the horizon
 * over the sphere from MAX_ALTITUDE, sqrt(2 R h), and as far again as the
 * highest land (the Earth's peaks, under 8.8 km) stands up above it: 505 km
 * and 335 km. The far plane follows it.
 * TERRAIN_HAZE: the terrain's haze, clearer than the city's (CITY_HAZE in
 * shaders/common.glsl): its mean free path at street level (m), thinning with
 * height (hazeAmount) through a low layer HAZE_LOW_H metres deep and a high
 * one HAZE_HIGH_H deep, above which the air is clear.
 *
 * The land beside the city takes its lamps (shaders/terrain/land_lights.glsl)
 * out to LAND_LIGHTS_FAR, fading over the last 100 m, from a chunk whose
 * lights reach it, found through a LAND_GRID x LAND_GRID grid of chunk cells
 * round the camera's (src/render/chunks.c: build_land_records), which must
 * reach that far and a cell beyond, whichever way it looks, from anywhere in
 * its cell: (LAND_GRID / 2 - 2) cells of CHUNK_SIZE. */
#define LAND_LIGHTS_FAR   500.0f
#define LAND_GRID         14
#define VIEW_DISTANCE     1900.0f
#define TERRAIN_DISTANCE  20000.0f
#define MAX_ALTITUDE      20000.0f
#define HORIZON_DISTANCE  840000.0f
#define NEAR_PLANE        0.08f
#define FAR_PLANE         (HORIZON_DISTANCE * 1.05f)
#define TERRAIN_HAZE      4000.0f
#define HAZE_LOW_H        110.0f
#define HAZE_HIGH_H       2000.0f

#endif
