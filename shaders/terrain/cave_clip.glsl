#ifndef CAVE_CLIP_GLSL
#define CAVE_CLIP_GLSL

/* The ground the terrain draws this frame, for cave.frag to cut the caves'
 * meshes off at where they come out of it at the mouths: the clipmap's
 * samples (terrain.vert's binding, here the caves' set's 4) and its levels
 * (terrain/cave_hole.glsl, CaveHoles.caveLevel), each vertex blended into its
 * parent as terrain.vert blends it, interpolated across the triangle the point
 * lies in, as grass/surface.glsl does (its heights only). */

layout(std430, set = 0, binding = 4) readonly buffer CaveSamples { uint caveSamples[]; };

float caveSampleWord(int i) { return uintBitsToFloat(caveSamples[i]); }
int caveSampleSlot(int level, ivec2 s) {
    const int N = TERRAIN_CLIP_N;
    return (((level * N) + (s.y & (N - 1))) * N + (s.x & (N - 1))) * 7;
}

// Vertex g of drawn level l's height, as terrain.vert makes it.
float caveDrawnVertex(int l, ivec2 g) {
    CaveLevel L = caveLevel[l];
    int base = caveSampleSlot(L.d.x, L.c.xy + g);
    float h = caveSampleWord(base), hc = caveSampleWord(base + 1);
    precise vec2 mxz = L.a.zw + (L.b.xy + vec2(g) * L.b.z);
    const float END = float((TERRAIN_CLIP_W - 1) / 2 - 1);
    float d = max(abs(mxz.x), abs(mxz.y)) / L.b.z;
    float morph = clamp((d - (END - float(TERRAIN_CLIP_MORPH))) / float(TERRAIN_CLIP_MORPH), 0.0, 1.0);
    morph = max(morph, L.b.w);
    return h + morph * (hc - h);
}

// The drawn ground's height (world Y) at camera-relative world (x, z): the
// finest drawn level whose window holds it. False outside every window.
bool caveDrawnHeight(vec2 rel, out float h) {
    h = 0.0;
    for (int l = caveHoleInfo.y; l < TERRAIN_CLIP_LEVELS; ++l) {
        CaveLevel L = caveLevel[l];
        vec2 q = (rel - L.a.xy - L.b.xy) / L.b.z;   // samples from the window's corner
        if (q.x < 0.0 || q.y < 0.0 || q.x > float(TERRAIN_CLIP_W - 1) || q.y > float(TERRAIN_CLIP_W - 1)) continue;
        ivec2 c = min(ivec2(floor(q)), ivec2(TERRAIN_CLIP_W - 2));
        vec2 f = q - vec2(c);
        float h0 = caveDrawnVertex(l, c), h2 = caveDrawnVertex(l, c + 1);
        if (f.x >= f.y) h = (1.0 - f.x) * h0 + f.y * h2 + (f.x - f.y) * caveDrawnVertex(l, c + ivec2(1, 0));
        else            h = (1.0 - f.y) * h0 + f.x * h2 + (f.y - f.x) * caveDrawnVertex(l, c + ivec2(0, 1));
        return true;
    }
    return false;
}

#endif
