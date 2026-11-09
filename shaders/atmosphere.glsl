#ifndef ATMOSPHERE_GLSL
#define ATMOSPHERE_GLSL

/* The clear air (DN3; src/core/atmosphere.h): its make-up with height, the
 * phase functions, the look-up tables' parameterisations (Hillaire 2020,
 * Bruneton 2017) and the distances along a ray to the ground and to the top
 * of the air. Lengths in kilometres, from the planet's centre (r) or above its
 * sphere (h). Pure functions: the tables are passed in by those that read
 * them (atmo_*.comp, sky.frag). */

#include "atmosphere.h"

const float AIR_PI = 3.14159265;
const float AIR_BOTTOM = PLANET_RADIUS * 1e-3;            // km
const float AIR_TOP = AIR_BOTTOM + ATMO_HEIGHT;
const vec3  AIR_RAYLEIGH = vec3(ATMO_RAYLEIGH_R, ATMO_RAYLEIGH_G, ATMO_RAYLEIGH_B);
const vec3  AIR_OZONE = vec3(ATMO_OZONE_R, ATMO_OZONE_G, ATMO_OZONE_B);
const float AIR_H = sqrt(ATMO_HEIGHT * (2.0 * AIR_BOTTOM + ATMO_HEIGHT));   // sqrt(top^2 - bottom^2): a ray grazing the ground, to the top

// The air at h km: its Rayleigh and Mie scattering, and all it takes out of a beam (per km).
struct AirAt { vec3 scatR; float scatM; vec3 ext; };
AirAt airAt(float h) {
    h = max(h, 0.0);
    float dR = exp(-h / ATMO_RAYLEIGH_H), dM = exp(-h / ATMO_MIE_H);
    float dO = max(0.0, 1.0 - abs(h - ATMO_OZONE_MID) / ATMO_OZONE_W);
    AirAt a;
    a.scatR = AIR_RAYLEIGH * dR;
    a.scatM = ATMO_MIE_SCAT * dM;
    a.ext = a.scatR + (ATMO_MIE_SCAT + ATMO_MIE_ABS) * dM + AIR_OZONE * dO;
    return a;
}

// c: the cosine between the way the light goes on and the way it came.
float rayleighPhase(float c) { return 3.0 / (16.0 * AIR_PI) * (1.0 + c * c); }
float hgPhase(float c, float g) {
    float d = 1.0 + g * g - 2.0 * g * c;
    return (1.0 - g * g) / (4.0 * AIR_PI * d * sqrt(d));
}
float miePhase(float c) {   // Cornette-Shanks, as Hillaire's
    const float g = ATMO_MIE_G;
    float k = 3.0 / (8.0 * AIR_PI) * (1.0 - g * g) / (2.0 + g * g);
    float d = 1.0 + g * g - 2.0 * g * c;
    return k * (1.0 + c * c) / (d * sqrt(d));
}

/* Along a ray from radius r whose cosine to the local up is mu, h = r -
 * AIR_BOTTOM above the ground (passed separately: r's float loses it): how far
 * to the ground (-1: it misses), and to the top of the air. Written to keep
 * their precision a few metres above a sphere 6371 km across. */
float airToGround(float r, float mu, float h) {
    if (mu >= 0.0) return -1.0;
    float c = h * (r + AIR_BOTTOM);            // r^2 - R^2
    float disc = r * r * mu * mu - c;
    if (disc < 0.0) return -1.0;
    return c / (-r * mu + sqrt(disc));         // the nearer root, without the cancellation
}
float airToTop(float r, float mu) {
    float disc = r * r * mu * mu + (AIR_TOP - r) * (AIR_TOP + r);
    return max(-r * mu + sqrt(max(disc, 0.0)), 0.0);
}

// A texture coordinate for a unit-range value whose ends are the first and last texels' centres, and back.
float unitToUV(float x, float n) { return 0.5 / n + x * (1.0 - 1.0 / n); }
float uvToUnit(float u, float n) { return (u - 0.5 / n) / (1.0 - 1.0 / n); }

/* The transmittance table (Bruneton's parameterisation): u how far along the
 * ray to the top between its least and most for that height, v the height's
 * share of AIR_H. Rays that meet the ground aren't told apart: the caller
 * takes the planet's shadow (airToGround). */
vec2 transUV(float r, float mu) {
    float rho = sqrt(max((r - AIR_BOTTOM) * (r + AIR_BOTTOM), 0.0));
    float d = airToTop(r, mu), dmin = AIR_TOP - r, dmax = rho + AIR_H;
    return vec2(unitToUV((d - dmin) / max(dmax - dmin, 1e-4), float(ATMO_TRANS_W)), unitToUV(rho / AIR_H, float(ATMO_TRANS_H)));
}
void transParams(vec2 uv, out float r, out float mu) {
    float xm = uvToUnit(uv.x, float(ATMO_TRANS_W)), xr = uvToUnit(uv.y, float(ATMO_TRANS_H));
    float rho = AIR_H * xr;
    r = sqrt(rho * rho + AIR_BOTTOM * AIR_BOTTOM);
    float dmin = AIR_TOP - r, dmax = rho + AIR_H, d = dmin + xm * (dmax - dmin);
    mu = d <= 0.0 ? 1.0 : clamp((AIR_H * AIR_H - rho * rho - d * d) / (2.0 * r * d), -1.0, 1.0);
}
vec3 airTransmittance(sampler2D lut, float r, float mu) { return textureLod(lut, transUV(r, mu), 0.0).rgb; }

// The light reaching a point at (r, h) from a source whose cosine to its up is mu: none in the planet's shadow.
vec3 airLitBy(sampler2D lut, float r, float h, float mu) {
    return airToGround(r, mu, h) >= 0.0 ? vec3(0.0) : airTransmittance(lut, r, mu);
}

// The multiple-scattering table: u the sun's cosine to the up, v the height's share of the air.
vec2 msUV(float h, float muSun) {
    return vec2(unitToUV(clamp(muSun * 0.5 + 0.5, 0.0, 1.0), float(ATMO_MS_SIZE)), unitToUV(clamp(h / ATMO_HEIGHT, 0.0, 1.0), float(ATMO_MS_SIZE)));
}

/* The sky-view table round an eye at radius r (height h): u the azimuth round
 * the local up (atan(z, x) over a turn: sampled with repeat), v the zenith
 * angle, its rows packed towards the horizon on either side (v = 0.5), as
 * Hillaire's. */
void skyRows(float r, float h, out float zHorizon, out float beta) {
    float cosBeta = sqrt(max(h * (r + AIR_BOTTOM), 0.0)) / r;   // the horizon dips below the level by beta's complement
    beta = acos(clamp(cosBeta, 0.0, 1.0));
    zHorizon = AIR_PI - beta;
}
vec2 skyUV(float r, float h, vec3 dir) {
    float zH, beta;
    skyRows(r, h, zH, beta);
    float z = acos(clamp(dir.y, -1.0, 1.0)), v;
    if (z < zH) v = 0.5 * (1.0 - sqrt(max(1.0 - z / zH, 0.0)));
    else        v = 0.5 + 0.5 * sqrt(clamp((z - zH) / beta, 0.0, 1.0));
    return vec2(atan(dir.z, dir.x) / (2.0 * AIR_PI), v);
}
vec3 skyDir(float r, float h, vec2 uv) {
    float zH, beta;
    skyRows(r, h, zH, beta);
    float z;
    if (uv.y < 0.5) { float c = 1.0 - 2.0 * uv.y; z = zH * (1.0 - c * c); }
    else            { float c = 2.0 * uv.y - 1.0; z = zH + beta * c * c; }
    float a = uv.x * 2.0 * AIR_PI;
    return vec3(sin(z) * cos(a), cos(z), sin(z) * sin(a));
}

#endif
