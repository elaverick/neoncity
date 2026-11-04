#ifndef FREEWAY_LIGHT_GLSL
#define FREEWAY_LIGHT_GLSL

/* The freeway's lamps, as the land lights by them (terrain/roads.glsl: the
 * carriageways and the land beside them) and the grass's blades do
 * (grass/blade.frag), with no bindings of its own: sodium lamps down the
 * median every FREEWAY_LAMP_SPACING, FREEWAY_LAMP_H over it; no heads are
 * drawn. Positions are a road's: off across from the centre line, s along it
 * (terrain/road_lookup.glsl RoadHit). */

const float FREEWAY_LAMP_SPACING = 60.0;   // m
const vec3  ROAD_SODIUM = vec3(1.0, 0.55, 0.22);

// The lamps, each a sphere this wide. Their highlights on wet ground are kept
// at least this rough (alpha), so a puddle shows a lamp's glow, not the image
// of a lamp that isn't there.
const float FREEWAY_LAMP_H = 12.0, FREEWAY_LAMP_R = 0.35, FREEWAY_GLINT_ALPHA = 0.08;

// How far beyond a road's paved edge it is shaded (m): a trunk road's verge,
// and beside a freeway its lamps' light on the land, faded out from
// FREEWAY_LIGHT_FADE. road_gpu.c files each segment in the cells within
// ROAD_CELL_REACH of it, to cover the farther.
const float FREEWAY_REACH = 14.0, TRUNK_REACH = 3.0, FREEWAY_LIGHT_FADE = 6.0;

// The light a lamp's pool gives (ROAD_SODIUM x this) square on.
const float FREEWAY_POOL = 1.4;

// How far along the road from the nearest lamp s is (m, -30 .. 30).
float freewayLampDs(float s) { return s - FREEWAY_LAMP_SPACING * floor(s / FREEWAY_LAMP_SPACING + 0.5); }

// The lamps' pools at ds from the nearest lamp and a across the road from its
// centre (0 .. 1): a pool round each lamp, longer along the road; seen from
// afar (pxs, the pixel's length along the road) a pixel spans several lamps,
// and takes their mean.
float freewayPool(float ds, float a, float pxs) {
    float across = exp(-a * a / (2.0 * 11.0 * 11.0));
    float pool = exp(-ds * ds / (2.0 * 14.0 * 14.0)) * across;
    float mean = 2.5066 * 14.0 / FREEWAY_LAMP_SPACING * across;
    return mix(pool, mean, clamp(pxs / (0.5 * FREEWAY_LAMP_SPACING), 0.0, 1.0));
}

// How much of the pools' light reaches the land a across from the centre line
// of a freeway hw wide (half its paved width).
float freewayBeside(float a, float hw) { return 1.0 - smoothstep(hw + FREEWAY_LIGHT_FADE, hw + FREEWAY_REACH, a); }

// From a point off across the road and ds from its nearest lamp, to lamp j
// along the road from that one (j = 0 the nearest): along and across are the
// world's x, z directions s and off grow in.
vec3 freewayLampVec(vec2 along, vec2 across, float ds, float off, int j) {
    vec2 q = along * (float(j) * FREEWAY_LAMP_SPACING - ds) - across * off;
    return vec3(q.x, FREEWAY_LAMP_H, q.y);
}

#endif
