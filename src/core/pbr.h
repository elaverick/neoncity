#ifndef PBR_H
#define PBR_H

/* Authored PBR materials: what C and GLSL share about them. #defines only, in
 * syntax both languages accept (included by src/core/common.h and
 * shaders/common.glsl).
 *
 * A material is defined once, in assets/materials/<name>.json: which images
 * are its maps and the properties surfaces shade it with (the format is in
 * src/render/material_def.h). Its id, PBR_<NAME>, is its place among those
 * files in name order. CMake writes the ids into pbr_materials.h when it
 * configures (CMakeLists.txt, "Materials"), and reconfigures when a file is
 * added or removed, so the files, the C code and the shaders can't disagree.
 * The renderer (src/render/materials.c) loads the maps and uploads the
 * properties; shaders/city/surface.glsl reads them. */
#include "pbr_materials.h"   /* PBR_<NAME>, PBR_MATERIAL_COUNT */

/* A material's maps, bound at material * PBR_MAPS_PER_MATERIAL + map. */
#define PBR_ALBEDO            0   /* base colour, sRGB; alpha the coverage where the surface is cut out */
#define PBR_NORMAL            1   /* tangent-space normal, OpenGL convention (+Y green = image up) */
#define PBR_ORM               2   /* R ambient occlusion, G roughness, B height */
#define PBR_EMISSIVE          3   /* optional: emitted colour, sRGB (black where absent) */
#define PBR_MAPS_PER_MATERIAL 4
#define PBR_TEXTURE_COUNT     (PBR_MATERIAL_COUNT * PBR_MAPS_PER_MATERIAL)

/* A material's flags (MaterialGPU info.x). Without PBR_LOADED its maps are
 * placeholders and surfaces keep their procedural look. */
#define PBR_LOADED            1u   /* its file and all its maps were read */
#define PBR_HAS_EMISSIVE      2u   /* it has an emissive map */

#endif
