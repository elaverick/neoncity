#ifndef CLOUDS_H
#define CLOUDS_H

/* Where the rain clouds gather (metres). The one definition, shared by C
 * (src/app/weather.c) and GLSL (shaders/common.glsl): #defines only, as in
 * distances.h.
 *
 * The cities draw the weather to them as mountains do. Over each settlement's
 * footprint, and a border CLOUD_BORDER wide round it, the deck is whole.
 * Beyond, it breaks up over CLOUD_FADE into the clear sky over the
 * countryside, where the moon and the stars light the land. Noise moves that
 * line in and out by up to CLOUD_RAGGED, so the deck's edge isn't a circle.
 * The city's light lights the underside of the deck only over the city
 * itself: towards the border's outer line it fades, so the border is the
 * darkest ground of all.
 * CLOUD_CITIES: how many of the settlements nearest the camera bring their
 * clouds into the frame (FrameUBO.cities).
 * CLOUD_REACH: how far across the ground the deck is marched (clouds.comp),
 * so that from out in the countryside a city's clouds stand on the skyline. */
#define CLOUD_CITIES   8
#define CLOUD_BORDER   3000.0f
#define CLOUD_FADE     2500.0f
#define CLOUD_RAGGED   1500.0f
#define CLOUD_REACH    30000.0f

#endif
