#pragma once
#include "bake.h"

/* The kit of parts office floors are built from (kit.c): the scene's primitives, the materials
 * and fittings, and the furniture and services. scene.c's test modules and floor_scene.c's
 * generated ones share it. */

/* The kit's lights are photometric (docs/interior-impostors.md, section 16). A bake's radiance is
 * BAKE_NIT of a candela a square metre. */
#define BAKE_NIT 0.01f
#define PI_F 3.14159265f

typedef struct Kit {
    int   state;              /* LIGHTS_* */
    float cx, cz, cr;         /* the late shift's cluster: a circle on the floor */
    int   trofferOn, trofferOff, stripOn, coveOn, orbOff;
    int floor, ceiling, concrete, duct, tray, pipe, wood, frame, pedestal, mesh, plastic, screenOn, screenOff;
    int pendantOn, pendantUp, pendantOff, lampOn, lampOff, wire, mullion, coreWall, door, board, glass, band, rug, marble, orb, pot, leaf, bark, sofa;
} Kit;

/* Primitives */
void  kelvin_unit(float k, float c[3]);
int   mesh_add(Scene *s, const float *v, uint32_t nv, const uint32_t *ix, uint32_t ni);
int   mesh_add_attr(Scene *s, const float *v, const float *attr, uint32_t nv, const uint32_t *ix, uint32_t ni);
int   mat_add(Scene *s, float r, float g, float b, float er, float eg, float eb);
void  mat_surface(Scene *s, int id, float rough, float metal, float coat, float coatRough);
int   mat_light(Scene *s, float lumens, float area, float kelvin, float share);
void  light_add(Scene *s, float x, float y, float z, float r, float g, float b);
void  inst_add(Scene *s, int mesh, int mat, uint32_t flags, float cx, float cy, float cz, float sx, float sy, float sz, float yaw);
void  box(Scene *s, int mat, uint32_t flags, float x0, float y0, float z0, float x1, float y1, float z1);
void  box_local(Scene *s, int mat, uint32_t flags, float ox, float oz, float yaw, float lx, float ly, float lz, float sx, float sy, float sz, float spin);
void  cube_mesh(Scene *s);
int   orb_mesh(Scene *s);

/* The kit */
Kit   kit_make(Scene *s, int state, float W, float D);
void  slabs(Scene *s, const Kit *k, float x0, float x1);
void  column(Scene *s, const Kit *k, float x, float z);
void  chair(Scene *s, const Kit *k, float x, float z, float yaw);
void  far_glass(Scene *s, const Kit *k, float x0, float x1, float z);
void  plant(Scene *s, const Kit *k, float x, float z, int species, float height, uint32_t seed);
void  sofa(Scene *s, Kit *k, float x, float z, float yaw, const char *assets);

/* The test modules' fittings, whose light depends on the kit's state and a draw */
bool  cluster(const Kit *k, float x, float z);
bool  pendant_lit(const Kit *k, float x, float z, float r);
bool  lamp_lit(const Kit *k, float x, float z, float r);
bool  screen_lit(const Kit *k, float x, float z, float r);
bool  strip_lit(const Kit *k, float x, float z, float r);
void  troffers(Scene *s, const Kit *k, float x0, float x1, float zEnd);
void  services(Scene *s, const Kit *k, float x0, float x1, const float *beamZ, int nbeam, const float *ductX, int nduct, const float *trayZ, int ntray, float zEnd);
void  pendant(Scene *s, const Kit *k, float x, float z, bool on);
void  desk(Scene *s, const Kit *k, float x, float zBack, float dir, uint32_t *rng);
void  desk_rows(Scene *s, const Kit *k, float zc, const float *xs, const int *n, int runs, uint32_t *rng);
void  core_wall(Scene *s, const Kit *k, float x0, float x1, float z, uint32_t *rng);
void  lounge(Scene *s, Kit *k, float x, float z, const char *assets, uint32_t seed);
void  meeting_room(Scene *s, const Kit *k, float xa, float xb, float za, float zb, uint32_t *rng);

/* Common values of a module (scene.c). */
void  scene_defaults(Scene *s, const char *name, int type, float W, float D);

/* A module from the floor generator's catalogue (floor_gen.h), or false if name isn't one
 * (floor_scene.c). state < 0: the state the catalogue gives it. */
bool  floor_scene_build(Scene *s, const char *name, int state, const char *assets, bool *found);
