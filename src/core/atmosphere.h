#ifndef ATMOSPHERE_H
#define ATMOSPHERE_H

/* The air (day/night cycle DN3; README, "Sky"). The one definition, shared
 * by C (src/render/atmosphere_gpu.c) and GLSL (shaders/atmosphere.glsl,
 * shaders/common.glsl): #defines only, as in distances.h.
 *
 * The Earth's clear air as Hillaire models it ("A Scalable and Production
 * Ready Sky and Atmosphere Rendering Technique", 2020, after Bruneton 2017):
 * Rayleigh scattering thinning over ATMO_RAYLEIGH_H, Mie (aerosol) scattering
 * and absorption over ATMO_MIE_H, ozone absorbing in a layer ATMO_OZONE_W
 * either side of ATMO_OZONE_MID, up to ATMO_HEIGHT above the planet's sphere
 * (PLANET_RADIUS). Coefficients per kilometre, heights in kilometres: the
 * look-up tables work in kilometres, which keep the planet's radius in a
 * float to half a metre.
 *
 * The light: the sun's illuminance above the air, SUN_ILLUMINANCE in the
 * engine's units (light_units.h: 2560 is 128 000 lux; about 100 000 reach the
 * ground at noon), as white; its disc SUN_RADIUS across (rad), the real one.
 * The moon lights the air too, with MOON_AIR of the light it throws on the
 * land (MOON_FULL in shaders/common.glsl, which is NIGHT_LIFT times the real
 * moon's): enough that a moonlit night's sky is a little brighter and bluer
 * than a moonless one's, not the lifted moon's whole light, which would make
 * every full moon a dusk.
 *
 * The smog: the haze of the cities and of the land (CITY_HAZE, TERRAIN_HAZE;
 * hazeAmount in shaders/common.glsl), much thicker than the clear air's
 * aerosols, scatters the daylight with SMOG_ALBEDO and Henyey-Greenstein's
 * SMOG_G, the skylight inside it carried SMOG_MS times for the light it
 * scatters on through itself. */
#define ATMO_HEIGHT        100.0f      /* km */
#define ATMO_RAYLEIGH_R    5.802e-3f   /* scattering, per km: red */
#define ATMO_RAYLEIGH_G    13.558e-3f  /* green */
#define ATMO_RAYLEIGH_B    33.1e-3f    /* blue */
#define ATMO_RAYLEIGH_H    8.0f        /* km */
#define ATMO_MIE_SCAT      3.996e-3f   /* per km */
#define ATMO_MIE_ABS       4.40e-3f    /* per km */
#define ATMO_MIE_H         1.2f        /* km */
#define ATMO_MIE_G         0.8f
#define ATMO_OZONE_R       0.650e-3f   /* absorption, per km */
#define ATMO_OZONE_G       1.881e-3f
#define ATMO_OZONE_B       0.085e-3f
#define ATMO_OZONE_MID     25.0f       /* km */
#define ATMO_OZONE_W       15.0f       /* km */
#define ATMO_GROUND_ALBEDO 0.3f

#define SUN_ILLUMINANCE    2560.0f
#define SUN_RADIUS         0.004675f
#define MOON_AIR           0.25f

#define SMOG_ALBEDO        0.9f
#define SMOG_G             0.6f
#define SMOG_MS            1.6f

/* The look-up tables (src/render/atmosphere_gpu.c): the transmittance to the
 * top of the air from any height along any direction; the light scattered
 * many times (Hillaire's psi_ms) at any height for any sun's height; the sky
 * round the camera, made each frame, its azimuth round the camera's local
 * frame and its rows packed towards the horizon. */
#define ATMO_TRANS_W   256
#define ATMO_TRANS_H   64
#define ATMO_MS_SIZE   32
#define ATMO_SKY_W     192
#define ATMO_SKY_H     108

#endif
