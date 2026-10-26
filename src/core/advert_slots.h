#ifndef ADVERT_SLOTS_H
#define ADVERT_SLOTS_H

/* Authored adverts on the video screens: what C and GLSL share about them.
 * #defines only, in syntax both languages accept (included by
 * src/render/advert_def.h and shaders/common.glsl).
 *
 * An advert is assets/adverts/<name>.json, naming a sheet of its frames
 * (src/render/advert_def.h; tools/advert_bake.py makes both from a video).
 * CMake counts the files when it configures (CMakeLists.txt, "Adverts") into
 * adverts_list.h. The renderer (src/render/adverts.c) loads every advert's
 * frames into a texture of its own; shaders/adverts.glsl picks what each screen
 * shows, and shadeAdvert (shaders/city/signs.glsl) draws it. */
#include "adverts_list.h"   /* ADVERT_COUNT, ADVERT_TABLE_SIZE (at least 1) */

/* A screen changes what it shows every slot, through a burst of static: the
 * first ADVERT_STATIC of the slot (screenAdvert, advertPhase in common.glsl).
 * An advert plays once from the static's end and holds its last frame, so it
 * may last up to (1 - ADVERT_STATIC) slots (material_test checks). */
#define ADVERT_SLOT_SECONDS 9.0
#define ADVERT_STATIC       0.025

/* A slot shows an authored advert or one of the four procedural scenes
 * (shadeScreen). Each scene weighs 1 against an advert's "weight". */
#define ADVERT_PLACEHOLDER_WEIGHT 4.0

/* A screen shows only the adverts of its own shape: whose frames' aspect ratio
 * is within ADVERT_ASPECT_FIT of its own (advertFits in adverts.glsl). The
 * picture fills it edge to edge, its LEDs stretched by no more than that; a
 * screen no advert fits shows only the procedural scenes. */
#define ADVERT_ASPECT_FIT 0.05

#endif
