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
 * so that from out in the countryside a city's clouds stand on the skyline.
 * DECK_DAYLIGHT: how much of the open sky's daylight gets through the deck to
 * the street under its middle (day/night cycle: a storm sky's, at noon some
 * 250 lux); its share of the moon's and the sky's light likewise.
 * The deck hangs over each settlement's own ground (the highest of it across
 * its footprint: city_settlement_deck; FrameUBO.cities[i].w): its heights
 * (CLOUD_LOW..CLOUD_HIGH in common.h, DECK_OVER, and clouds.comp's) count
 * from there, so a city on raised ground, or climbing a hillside, has its deck
 * at least as high over its streets as a flat one has. Between two settlements it runs from one's ground to the
 * other's: each weighs (DECK_BLEND / (DECK_BLEND + how far beyond its
 * footprint))^4 (deckDatum in shaders/common.glsl, weather.c at the eye).
 * DECK_OVER: ground, or an eye, from DECK_OVER up through CLOUD_HIGH stands
 * out of the deck: under none of it, sunlit and dry.
 * RAIN_DRIZZLE: the rain intensity (0 dry .. 1 downpour) of a drizzle. Below
 * it the drops, the ripples and the wetness fade out to dry at 0. */
#define CLOUD_CITIES   8
#define CLOUD_BORDER   3000.0f
#define CLOUD_FADE     2500.0f
#define CLOUD_RAGGED   1500.0f
#define CLOUD_REACH    30000.0f
#define RAIN_DRIZZLE   0.1f
#define DECK_DAYLIGHT  0.0025f
#define DECK_OVER      330.0f
#define DECK_BLEND     2000.0f

#endif
