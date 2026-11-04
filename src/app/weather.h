#pragma once
#include "renderer.h"

/* The weather. Rain falls steadily on the megacities, and its intensity
 * (0 dry, RAIN_DRIZZLE a drizzle, 1 a downpour: src/core/clouds.h) drifts
 * between drizzle and downpour over minutes without stopping. The streets wet quickly as
 * the rain strengthens and dry slowly as it eases. All of it falls from the
 * clouds the cities gather (src/core/clouds.h), so out in the countryside,
 * under a clear sky, it is dry. */
typedef struct Weather { float rain, wet; } Weather;

/* Advances the rain to time t (s). fixedRain >= 0 pins its intensity. */
void weather_update(Weather *w, double t, float dt, float fixedRain);

/* The frame's weather, for a camera at world position eye: the rain and
 * wetness where the camera is, ubo->sky, and the settlements nearest it whose
 * clouds are drawn (ubo->cities). */
void weather_frame(const Weather *w, const double eye[3], FrameUBO *ubo);
