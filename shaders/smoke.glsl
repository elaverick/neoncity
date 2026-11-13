// Shared by smoke.vert and smoke.frag: the stacks' plumes near the camera (chunks_plumes
// in src/render/chunks.c; Plume and PlumeSet in src/core/common.h: must match).

struct Plume {
    vec3  pos;      // the stack's mouth, camera-relative
    float radius;   // the mouth's (m)
    float rise;     // how high it climbs before it has thinned away (m); a stall's: past its last knot
    uint  seed;
    uint  kind;     // PLUME_*
    float strength; // a stall's: how thick, 0..1
    vec2  dir;      // a stall's: the way its knots run out (world xz)
    uint  nknot;    // a stall's knots (1: a free wisp)
    uint  hours;    // a stall's: its vendor's hours code (SF4: common.glsl vendorOpen; none while shut)
    vec4  knot[8];  // a stall's path: out along dir, up, from the mouth; the ceiling over the knot (0: none)
    vec4  light[3]; // a stall's light (linear): over the mouth, under the hood, past the fascia
};
layout(std430, set = 0, binding = 6) readonly buffer Plumes { uint plumeCount, plumePad0, plumePad1, plumePad2; Plume plumes[]; };

const uint PLUME_SMOKE = 0u, PLUME_STEAM = 1u, PLUME_VENT = 2u;   // a stack's smoke or steam; a manhole's wisp
const uint PLUME_STALL = 3u, PLUME_WOK = 4u;   // a noodle bar's steam, its wok's smoke (NB7: src/gen/noodle_bar.h nb_steam)
bool plumeStall(Plume p) { return p.kind >= PLUME_STALL; }

/* The flow a plume rides. The gas leaves the mouth at plumeExit and slows as
 * its buoyancy is spent, w(h) = exit / (1 + h / plumeSlow), so a parcel of it
 * reaches height h at age plumeAge(h). Meanwhile the wind carries it downwind
 * (GUST_WIND) at PLUME_BREEZE, and up to PLUME_GUST faster in a gust (gustAt),
 * which bends the plume over. A manhole's steam only seeps out, and down in
 * the street the buildings keep most of the wind off it (plumeWind). */
const float PLUME_BREEZE = 1.5, PLUME_GUST = 3.5;   // m/s
float plumeExit(Plume p) { return p.kind == PLUME_SMOKE ? 8.0 + p.radius : p.kind == PLUME_STEAM ? 5.0 + 0.5 * p.radius : 0.5; }   // hot flue gas leaves faster
float plumeSlow(Plume p) { return p.kind == PLUME_VENT ? 6.0 : 30.0 + 8.0 * p.radius; }
float plumeWind(Plume p) { return p.kind == PLUME_VENT ? 0.08 : 1.0; }
float plumeAge(Plume p, float h) { return (h + h * h / (2.0 * plumeSlow(p))) / plumeExit(p); }
float plumeRiseSpeed(Plume p, float h) { return plumeExit(p) / (1.0 + h / plumeSlow(p)); }
// How wide it has spread at height h, d downwind of the mouth: entraining air as it goes.
float plumeRadius(Plume p, float h, float d) { return p.radius * 1.3 + 0.16 * h + 0.12 * d; }

// The box a plume drifts in: from just under the mouth to the top of its rise,
// and downwind as far as the strongest gust can carry it before it is gone
// (smoke.frag: it thins away over its path's length, rise), with room for the
// turbulence to throw it about.
/* A stall's steam (steam.vert): it leaves the pot at STALL_EXIT and runs along its knots at STALL_FLOW, then
 * climbs free past the last; down in the street the buildings and the hood keep most of the wind off it. */
const float STALL_EXIT = 1.2, STALL_FLOW = 0.55, STALL_WIND = 0.15;   // m/s; the wind's share
float stallRadius(Plume p, float s) { return p.radius * 1.2 + 0.07 * s; }   // a distance s along its path

void plumeBox(Plume p, out vec3 lo, out vec3 hi) {
    float dmax = min(plumeAge(p, p.rise) * (PLUME_BREEZE + PLUME_GUST) * plumeWind(p), p.rise * 1.1);
    float r = plumeRadius(p, p.rise, dmax) * 1.4;
    vec2 d = GUST_WIND * dmax;
    lo = p.pos + vec3(min(d.x, 0.0) - r, -1.0, min(d.y, 0.0) - r);
    hi = p.pos + vec3(max(d.x, 0.0) + r, p.rise + r * 0.6, max(d.y, 0.0) + r);
}
