#ifndef GRASS_WAKE_GLSL
#define GRASS_WAKE_GLSL

/* The player's wake through the grass (docs/procedural_grass_rendering_implementation.md
 * 9; GR8). FrameUBO.wake is the path the feet took over the last few seconds,
 * newest first (src/grass/grass_wake.c, which says why nothing pops), each
 * point with its age and weight. A blade's root within GRASS_WAKE_RADIUS of
 * the path is pushed away from its nearest point there by (1 - r^2)^2
 * (spec 9.1), times the recovery at the age the path has there: a damped
 * spring released as the feet passed, which swings back a little past upright
 * and settles (spec 9.2), its pace tuned by the blade's own phase so a trodden
 * strip doesn't spring back in step. The segments' pushes are summed and held
 * to the largest one's size, so where the path bends or crosses itself a
 * blade isn't pushed twice. Only a root inside the box the path reaches
 * evaluates it (spec 9.1's "only nearby patches"). The legs reach 0.6-1.2 m
 * down to a root, a bank above the feet 1.4-1.8 m up.
 *
 * Step for step src/grass/grass_wake.c grass_wake_push (grass_test's). */

float grassWakeRecover(float age, float phase) {
    float om = float(GRASS_WAKE_OMEGA) * (0.85 + 0.3 * phase), l = float(GRASS_WAKE_DAMP);
    age = max(age, 0.0);
    return exp(-l * age) * (cos(om * age) + l / om * sin(om * age));
}

float grassWakeWeight(int i) { return frame.wakeW[i >> 2][i & 3]; }

// The push at camera-relative root p: the direction along the ground (x, z) times how far
// (-1 .. 1 of GRASS_WAKE_BEND x the height x the species' interaction).
vec2 grassWakeAt(vec3 p, float phase) {
    int n = int(frame.wakeInfo.x + 0.5);
    vec4 bx = frame.wakeBox;
    if (n < 2 || p.x < bx.x || p.z < bx.y || p.x > bx.z || p.z > bx.w) return vec2(0.0);
    const float R2 = float(GRASS_WAKE_RADIUS * GRASS_WAKE_RADIUS);
    vec2 v = vec2(0.0);
    float most = 0.0;
    for (int i = 0; i + 1 < n; ++i) {
        vec4 a = frame.wake[i], b = frame.wake[i + 1];
        vec2 ab = b.xz - a.xz;
        float s = clamp(dot(p.xz - a.xz, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
        vec2 d = p.xz - (a.xz + ab * s);
        float r2 = dot(d, d) / R2;
        if (r2 >= 1.0) continue;
        float wt = mix(grassWakeWeight(i), grassWakeWeight(i + 1), s);
        float gap = mix(a.y, b.y, s) - p.y;
        float g = (1.0 - smoothstep(0.6, 1.2, gap)) * (1.0 - smoothstep(1.4, 1.8, -gap));
        float amt = (1.0 - r2) * (1.0 - r2) * wt * g * grassWakeRecover(mix(a.w, b.w, s), phase);
        float dl = sqrt(dot(d, d));
        vec2 u = dl > 1e-4 ? d / dl : vec2(-ab.y, ab.x) / (length(ab) + 1e-6);
        v += amt * u;
        most = max(most, abs(amt));
    }
    float lv = length(v);
    return lv > most ? v * (most / lv) : v;
}

// --grass-debug wake, by the push's size s: dark blue untouched, through orange to white where pushed flat.
vec3 grassWakeColour(float s) {
    vec3 c = mix(vec3(0.02, 0.04, 0.25), vec3(0.80, 0.35, 0.05), smoothstep(0.0, 0.5, s));
    return mix(c, vec3(0.95), smoothstep(0.5, 1.0, s));
}

#endif
