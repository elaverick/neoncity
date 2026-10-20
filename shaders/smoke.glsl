// Shared by smoke.vert and smoke.frag: the stacks' plumes near the camera (chunks_plumes
// in src/render/chunks.c; Plume and PlumeSet in src/core/common.h: must match).

struct Plume {
    vec3  pos;      // the stack's mouth, camera-relative
    float radius;   // the mouth's (m)
    float rise;     // how high it climbs before it has thinned away (m)
    uint  seed;
    uint  kind;     // PLUME_SMOKE, PLUME_STEAM
    float pad;
};
layout(std430, set = 0, binding = 6) readonly buffer Plumes { uint plumeCount, plumePad0, plumePad1, plumePad2; Plume plumes[]; };

const uint PLUME_SMOKE = 0u, PLUME_STEAM = 1u;

/* The flow a plume rides. The gas leaves the mouth at plumeExit and slows as
 * its buoyancy is spent, w(h) = exit / (1 + h / plumeSlow), so a parcel of it
 * reaches height h at age plumeAge(h). Meanwhile the wind carries it downwind
 * (GUST_WIND) at PLUME_BREEZE, and up to PLUME_GUST faster in a gust (gustAt),
 * which bends the plume over. */
const float PLUME_BREEZE = 1.5, PLUME_GUST = 3.5;   // m/s
float plumeExit(Plume p) { return p.kind == PLUME_SMOKE ? 8.0 + p.radius : 5.0 + 0.5 * p.radius; }   // hot flue gas leaves faster
float plumeSlow(Plume p) { return 30.0 + 8.0 * p.radius; }
float plumeAge(Plume p, float h) { return (h + h * h / (2.0 * plumeSlow(p))) / plumeExit(p); }
float plumeRiseSpeed(Plume p, float h) { return plumeExit(p) / (1.0 + h / plumeSlow(p)); }
// How wide it has spread at height h, d downwind of the mouth: entraining air as it goes.
float plumeRadius(Plume p, float h, float d) { return p.radius * 1.3 + 0.16 * h + 0.12 * d; }

// The box a plume drifts in: from just under the mouth to the top of its rise,
// and downwind as far as the strongest gust can carry it, with room for the
// turbulence to throw it about.
void plumeBox(Plume p, out vec3 lo, out vec3 hi) {
    float dmax = plumeAge(p, p.rise) * (PLUME_BREEZE + PLUME_GUST);
    float r = plumeRadius(p, p.rise, dmax) * 1.4;
    vec2 d = GUST_WIND * dmax;
    lo = p.pos + vec3(min(d.x, 0.0) - r, -1.0, min(d.y, 0.0) - r);
    hi = p.pos + vec3(max(d.x, 0.0) + r, p.rise + r * 0.6, max(d.y, 0.0) + r);
}
