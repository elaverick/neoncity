#include "weather.h"
#include "noise.h"
#include "plan.h"

void weather_update(Weather *w, double t, float dt, float fixedRain) {
    float rain = fixedRain >= 0.0f ? fixedRain
               : (float)(0.6 + 0.25 * sin(t / 53.0) + 0.15 * sin(t / 31.0 + 1.7));
    float target = 0.55f + 0.45f * rain;
    if (w->wet < 0.0f) w->wet = target;   /* first frame: it has already been raining for a while */
    float tau = target > w->wet ? 15.0f : 120.0f;
    w->wet += (target - w->wet) * (1.0f - expf(-dt / tau));
    w->rain = rain;
}

/* fbm in shaders/common.glsl. */
static float glsl_fbm(double x, double z, uint32_t salt) {
    return 0.5f * noise_value2(salt, x, z) + 0.3f * noise_value2(salt + 17u, x * 2.03, z * 2.03)
         + 0.2f * noise_value2(salt + 31u, x * 4.11, z * 4.11);
}

/* The C mirror of the cloud field in shaders/common.glsl (cityEdge, cloudCover),
 * at the camera. Each settlement's footprint is taken as its plain disc: the
 * warp is lost in the border round it. */
typedef struct Near { double edge; int k; } Near;

void weather_frame(const Weather *w, const double eye[3], FrameUBO *ubo) {
    /* Settlements further than this can't bring cloud within CLOUD_REACH. */
    const double far = CLOUD_REACH + CLOUD_BORDER + CLOUD_FADE + CLOUD_RAGGED;
    Near nearest[CLOUD_CITIES];
    int n = 0;
    for (int k = 0, ns = plan_settlement_count(); k < ns; ++k) {
        PlanSettlement s = plan_settlement(k);
        double e = hypot(s.centre.x - eye[0], s.centre.z - eye[2]) - s.radius;
        if (e >= far || (n == CLOUD_CITIES && e >= nearest[n - 1].edge)) continue;
        int i = n < CLOUD_CITIES ? n++ : n - 1;   /* insertion, nearest first */
        for (; i > 0 && nearest[i - 1].edge > e; --i) nearest[i] = nearest[i - 1];
        nearest[i] = (Near){ e, k };
    }
    for (int i = 0; i < n; ++i) {
        PlanSettlement s = plan_settlement(nearest[i].k);
        ubo->cities[i][0] = (float)(s.centre.x - eye[0]);
        ubo->cities[i][1] = (float)(s.centre.z - eye[2]);
        ubo->cities[i][2] = (float)s.radius;
        ubo->cities[i][3] = 0.0f;
    }
    double edge = n > 0 ? nearest[0].edge : 1e9;
    float ragged = CLOUD_RAGGED * (2.0f * glsl_fbm((float)eye[0] / 5000.0, (float)eye[2] / 5000.0, 0xC1A0u) - 1.0f);
    float cover = 1.0f - smoothstepf(CLOUD_BORDER, CLOUD_BORDER + CLOUD_FADE, (float)edge + ragged);
    ubo->sky[0] = 1.0f - smoothstepf(0.0f, CLOUD_BORDER + CLOUD_FADE, (float)edge);
    ubo->sky[1] = (float)n;
    ubo->sky[2] = cover;
    ubo->sky[3] = 0.0f;
    /* It rains only from the cloud overhead. */
    ubo->weather[0] = w->rain * cover;
    ubo->weather[1] = w->wet * cover;
}
