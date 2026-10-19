#ifndef TERRAIN_PLANET_H
#define TERRAIN_PLANET_H

/* The planet's size (see terrain.h, "The planet"), shared by C
 * (src/terrain/terrain.h) and GLSL (the shaders that bend the ground away
 * from the camera, and cut the planet's patches): #defines only, in syntax both accept. */
#define PLANET_RADIUS 6371000.0   /* metres, to the sphere world Y = 0 lies on */
/* The clipmap's window, less this, is cut out of the planet's patches
 * (terrain_planet_select, shaders/terrain/planet.*), m. */
#define TERRAIN_PLANET_HOLE_M 1024.0

#endif
