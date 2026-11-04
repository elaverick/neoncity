#ifndef GRASS_INSTANCE_GLSL
#define GRASS_INSTANCE_GLSL

/* A blade as the prepare pass writes it and the blade shaders draw it:
 * GRASS_INSTANCE_WORDS (8) words. p is the root, camera-relative and bent
 * over the planet as the terrain is (planetBend), on the ground the terrain
 * draws there. */
struct GrassInstance {
    vec3  p;        // its root (m)
    float height;   // m
    uint  nrm;      // the ground's normal there: x, z as snorm16 (y up, positive)
    uint  dir;      // unorm8 its face's direction (turns, from +x towards +z), unorm8 its stiffness / 2 (its clump's: GR5's wind),
                    // unorm16 its lean's direction (turns)
    uint  shape;    // half x2: its width at the base (m), its lean (rad)
    uint  misc;     // unorm8 bend (share of its height), unorm8 phase, unorm8 tint + 0.5, then the species (2 bits), its bucket (2 bits),
                    // its morph to the next bucket's shape (4 bits, in GRASS_MORPH_STEPS)
};

// The prepare pass writes them, the vertex shader reads them (where a storage
// buffer must be read-only): GRASS_INSTANCE_ACCESS says which, before this is included.
#ifndef GRASS_INSTANCE_ACCESS
#define GRASS_INSTANCE_ACCESS readonly
#endif
layout(std430, set = 0, binding = 5) GRASS_INSTANCE_ACCESS buffer Instances { GrassInstance inst[]; };

struct GrassBladeDraw {
    vec3  p, n;
    float height, width, facing, leanAngle, lean, bend, phase, tint, morph, stiffness;
    int   species, lod;
};

void grassPack(vec3 p, float height, vec3 n, float facing, vec2 leanDir, float width, float lean, float bend, float phase, float tint, float stiffness, int species, int lod, float morph, out GrassInstance g) {
    const float TURN = 1.0 / 6.28318531;
    g.p = p;
    g.height = height;
    g.nrm = packSnorm2x16(n.xz);
    uint faceU = uint(fract(facing * TURN) * 256.0) & 255u, stiffU = uint(clamp(0.5 * stiffness, 0.0, 1.0) * 255.0 + 0.5);
    g.dir = faceU | (stiffU << 8) | (uint(fract(atan(leanDir.y, leanDir.x) * TURN) * 65535.0 + 0.5) << 16);
    g.shape = packHalf2x16(vec2(width, lean));
    uint bendU = uint(clamp(bend, 0.0, 1.0) * 255.0 + 0.5), phaseU = uint(clamp(phase, 0.0, 1.0) * 255.0 + 0.5);
    uint tintU = uint(clamp(tint + 0.5, 0.0, 1.0) * 255.0 + 0.5);
    uint morphU = uint(clamp(morph, 0.0, 1.0) * float(GRASS_MORPH_STEPS) + 0.5);
    g.misc = bendU | (phaseU << 8) | (tintU << 16) | (uint(species & 3) << 24) | (uint(lod & 3) << 26) | (morphU << 28);
}

GrassBladeDraw grassUnpack(GrassInstance g) {
    GrassBladeDraw b;
    b.p = g.p;
    b.height = g.height;
    vec2 nxz = unpackSnorm2x16(g.nrm);
    b.n = vec3(nxz.x, sqrt(max(1.0 - dot(nxz, nxz), 0.0)), nxz.y);
    b.facing = float(g.dir & 255u) * (6.28318531 / 256.0);
    b.stiffness = float((g.dir >> 8) & 255u) * (2.0 / 255.0);
    b.leanAngle = float(g.dir >> 16) * (6.28318531 / 65535.0);
    vec2 s = unpackHalf2x16(g.shape);
    b.width = s.x;
    b.lean = s.y;
    b.bend = float(g.misc & 255u) / 255.0;
    b.phase = float((g.misc >> 8) & 255u) / 255.0;
    b.tint = float((g.misc >> 16) & 255u) / 255.0 - 0.5;
    b.species = int((g.misc >> 24) & 3u);
    b.lod = int((g.misc >> 26) & 3u);
    b.morph = float(g.misc >> 28) / float(GRASS_MORPH_STEPS);
    return b;
}

#endif
