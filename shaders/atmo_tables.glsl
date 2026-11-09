#ifndef ATMO_TABLES_GLSL
#define ATMO_TABLES_GLSL

/* The air's look-up tables' set (src/render/atmosphere_gpu.c), shared by the
 * passes that fill them: atmo_transmittance.comp, atmo_multiscatter.comp,
 * atmo_skyview.comp, atmo_eye.comp. Binding 0 is the frame (common.glsl). */

#include "common.glsl"

layout(set = 0, binding = 1) uniform sampler2D transLut;
layout(set = 0, binding = 2) uniform sampler2D msLut;
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D transOut;
layout(set = 0, binding = 4, rgba16f) uniform writeonly image2D msOut;
layout(set = 0, binding = 5, rgba16f) uniform writeonly image2D skyOut;
layout(set = 0, binding = 6) uniform sampler2D skyLut;
layout(set = 0, binding = 7, std430) buffer AirEye { vec4 airEye[8]; };   // AIR_* in common.glsl, read back for the frame

#endif
