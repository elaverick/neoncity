#include "weather.h"
#include "noise.h"
#include "plan.h"
#include "city.h"

void weather_update(Weather *w, double t, float dt, float fixedRain) {
    /* The megacities' rain never stops: left to drift, it stays between a
     * heavy drizzle and a downpour. (The rest of the world is to get weather
     * of its own, dry spells included.) */
    float rain = fixedRain >= 0.0f ? fminf(fixedRain, 1.0f)
               : RAIN_DRIZZLE + (1.0f - RAIN_DRIZZLE) * (float)(0.6 + 0.25 * sin(t / 53.0) + 0.15 * sin(t / 31.0 + 1.7));
    /* Dry at 0; a drizzle leaves the ground damp (0.55), a downpour soaked. */
    float target = rain < RAIN_DRIZZLE ? 0.55f * rain / RAIN_DRIZZLE
                 : 0.55f + 0.45f * (rain - RAIN_DRIZZLE) / (1.0f - RAIN_DRIZZLE);
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
 * warp is lost in the border round it. The discs are found over the sphere
 * and drawn where they lie in the active frame. */
typedef struct Near { double edge; int k; } Near;

void weather_frame(const Weather *w, const double eye[3], FrameUBO *ubo) {
    /* Settlements further than this can't bring cloud within CLOUD_REACH. */
    const double far = CLOUD_REACH + CLOUD_BORDER + CLOUD_FADE + CLOUD_RAGGED;
    static TerrainDir disc[PLAN_MAX_SETTLEMENTS];
    static int known;
    int ns = plan_settlement_count();
    if (!known) {
        for (int k = 0; k < ns; ++k) disc[k] = terrain_frame_dir(plan_frame_settlement(k), plan_settlement(k).disc.x, plan_settlement(k).disc.z);
        known = 1;
    }
    TerrainDir at = terrain_world_dir(eye[0], eye[2]);
    Near nearest[CLOUD_CITIES];
    int n = 0;
    for (int k = 0; k < ns; ++k) {
        double c = at.x * disc[k].x + at.y * disc[k].y + at.z * disc[k].z;
        double e = PLANET_RADIUS * acos(c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c) - plan_settlement(k).radius;
        if (e >= far || (n == CLOUD_CITIES && e >= nearest[n - 1].edge)) continue;
        int i = n < CLOUD_CITIES ? n++ : n - 1;   /* insertion, nearest first */
        for (; i > 0 && nearest[i - 1].edge > e; --i) nearest[i] = nearest[i - 1];
        nearest[i] = (Near){ e, k };
    }
    for (int i = 0; i < n; ++i) {
        double x, z;
        terrain_dir_world(disc[nearest[i].k], &x, &z);
        ubo->cities[i][0] = (float)(x - eye[0]);
        ubo->cities[i][1] = (float)(z - eye[2]);
        ubo->cities[i][2] = (float)plan_settlement(nearest[i].k).radius;
        ubo->cities[i][3] = city_settlement_deck(nearest[i].k);   /* the ground its deck hangs over: its highest */
    }
    double edge = n > 0 ? nearest[0].edge : 1e9;
    /* the ground the deck over the eye hangs over (deckDatum in shaders/common.glsl) */
    float datum = 0.0f, wsum = 0.0f, lowest = 1e9f;
    for (int i = 0; i < n; ++i) {
        float w = DECK_BLEND / ((float)fmax(nearest[i].edge, 0.0) + DECK_BLEND);
        w *= w; w *= w;
        datum += w * ubo->cities[i][3];
        wsum += w;
        lowest = fminf(lowest, ubo->cities[i][3]);
    }
    datum = wsum > 0.0f ? datum / wsum : 0.0f;
    float ragged = CLOUD_RAGGED * (2.0f * glsl_fbm((float)eye[0] / 5000.0, (float)eye[2] / 5000.0, 0xC1A0u) - 1.0f);
    float deck = 1.0f - smoothstepf(CLOUD_BORDER, CLOUD_BORDER + CLOUD_FADE, (float)edge + ragged);
    /* the smog stops at the deck's top over the cities' cloud (hazeAmount):
     * its top over the eye and the lowest of any drawn, in the haze's heights */
    ubo->smog[0] = CLOUD_HIGH + datum - ubo->weather[2];
    ubo->smog[1] = deck;
    ubo->smog[2] = n > 0 ? CLOUD_HIGH + lowest - ubo->weather[2] : 1e9f;
    ubo->smog[3] = 0.0f;
    /* an eye up out of the deck (on a hill, or flying) is under none of it (landSky in shaders/terrain/ground.glsl) */
    float cover = deck * (1.0f - smoothstepf(DECK_OVER + datum, CLOUD_HIGH + datum, (float)eye[1]));
    ubo->sky[0] = 1.0f - smoothstepf(0.0f, CLOUD_BORDER + CLOUD_FADE, (float)edge);
    ubo->sky[1] = (float)n;
    ubo->sky[2] = cover;
    /* how wet the ground is under the cloud, wherever it is (the land's: shaders/terrain/ground.glsl) */
    ubo->sky[3] = w->wet > 0.0f ? w->wet : 0.0f;   /* (-1 before the first update) */
    /* It rains only from the cloud overhead. */
    ubo->weather[0] = w->rain * cover;
    ubo->weather[1] = w->wet * cover;
}
