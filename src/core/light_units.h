#ifndef LIGHT_UNITS_H
#define LIGHT_UNITS_H

/* The engine's light in physical units. The one definition, shared by C
 * (src/app/main.c: the exposure) and GLSL (shaders/common.glsl): #defines
 * only, as in distances.h. README, "Light and exposure", has the table.
 *
 * The anchor: one unit of the engine's irradiance (what the lighting sums,
 * colour times falloff) is LIGHT_LUX lux. It is measured on the street
 * lamps, the light the city is built round: under a sodium head the street
 * gets 0.91 (lampFalloff 1.75 at 7.1 m, times lampColor's luminance 0.52),
 * which is a lit main road's 45 lux or so under the lamp. The lighting
 * leaves Lambert's 1/pi out (diffuse is albedo times irradiance), so one unit
 * of radiance (what the scene target holds, before exposure) is LIGHT_NIT
 * candelas a square metre.
 *
 * Exposure follows the photographic convention (Lagarde and de Rousiers,
 * "Moving Frostbite to PBR", 2014): at EV100 a luminance of 1.2 x 2^EV100
 * cd/m2 is taken to 1 before the tone curve. EV100_REF is the exposure that
 * takes one unit of radiance to 1: the one the night was tuned at, and the
 * --ev default.
 *
 * The night's lights are not all physical against that anchor (README): the
 * moon and the starlight are about NIGHT_LIFT times the real ones, as the
 * night's look was tuned; the lobbies and the impostors' offices are about
 * 1/17 of the real ones (their kit's bake_scale). */
#define LIGHT_LUX     50.0f
#define LIGHT_NIT     15.9155f      /* LIGHT_LUX / pi */
#define EV100_REF     3.7293f       /* log2(LIGHT_NIT / 1.2) */
#define NIGHT_LIFT    70.0f

#endif
