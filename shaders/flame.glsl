// Shared by flame.vert and flame.frag: the flames near the camera (chunks_flames
// in src/render/chunks.c; Flame and FlameSet in src/core/common.h: must match).

struct Flame {
    vec3  pos;      // the nozzle, camera-relative
    float reach;    // how high a gout climbs (m)
    float radius;   // the column's widest (m)
    uint  id;       // its timing (flameGout in common.glsl)
    float pad0, pad1;
};
layout(std430, set = 0, binding = 3) readonly buffer Flames { uint flameCount, flamePad0, flamePad1, flamePad2; Flame flames[]; };

// The box a flame burns in: from just under the nozzle to past the top of a
// gout burning away (flameGout's head reaches 1.45 of the reach), and wider
// than the fireball it leaves.
void flameBox(Flame f, out vec3 lo, out vec3 hi) {
    float w = f.radius * 3.2;   // the burning-away fireball swells to about three times the column
    lo = f.pos - vec3(w, 0.6, w);
    hi = f.pos + vec3(w, f.reach * 1.5 + f.radius, w);
}
