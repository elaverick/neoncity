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

/* Auto exposure (src/render/exposure_gpu.c; shaders/exposure_meter.comp and
 * exposure_adapt.comp; README, "Light and exposure"). The meter: a histogram
 * of the scene's luminance in the engine's units, every other pixel each way,
 * weighted to the middle of the screen, in METER_BINS bins: bin 0 everything
 * below 2^METER_LOG_LO (black), the rest evenly in log2 up to 2^METER_LOG_HI
 * (a sunlit wall is about 2^9). Its average is taken over the bins between
 * the METER_PCT_LO and METER_PCT_HI fractions of the weight: the darkest half
 * of the screen (a night sky, an unlit wall) doesn't drag the exposure up, nor
 * a few lamp heads it down. The metered EV100 is the photographer's
 * log2(L x 100 / 12.5), L the average in cd/m2. */
#define METER_BINS    128
#define METER_LOG_LO  (-16.0f)
#define METER_LOG_HI  12.0f
#define METER_PCT_LO  0.50f
#define METER_PCT_HI  0.95f

/* The key: the exposure the eye settles on for a metered EV (Krawczyk et al.
 * 2005 in spirit: a dark scene is shown darker than mid-grey). Through the
 * night it follows the scene by KEY_NIGHT_SLOPE of a stop a stop, from
 * EV100_REF at KEY_NIGHT_EV, what the city's streets meter at night (the
 * spawn in rain 3.4, the street under the towers 2.8, a facade 1.7): so the
 * night stays where it was tuned, the moonlit land (0.4-0.7) shows only some
 * 0.8 of a stop brighter. Above KEY_DUSK_EV it follows a stop a stop, as a
 * camera would by day (sunlit land, metering about 15, shown at 14.5). The
 * result is held within EXPOSURE_MIN and EXPOSURE_MAX: EXPOSURE_MIN is the
 * floor that keeps a cave dark however little light it has (one meters -4
 * to -11), a stop over the night's. The eye adapts at ADAPT_UP a second
 * (e-foldings) to a brighter scene, ADAPT_DOWN to a darker one. */
#define KEY_NIGHT_EV    2.75f
#define KEY_NIGHT_SLOPE 0.35f
#define KEY_DUSK_EV     5.0f
#define EXPOSURE_MIN    2.7f
#define EXPOSURE_MAX    16.0f
#define ADAPT_UP      3.0f
#define ADAPT_DOWN    1.0f

#endif
