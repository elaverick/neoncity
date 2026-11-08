#ifndef LAND_LIGHTS_GLSL
#define LAND_LIGHTS_GLSL

/* The city's lamps on the land beside it (M11b): which chunk's light table
 * (common.glsl: lightRec) lights the land at a point. Each frame the chunks
 * whose tables are resident round the camera are in frame.landCells, a
 * LAND_GRID x LAND_GRID grid of chunk cells whose corner is frame.landGrid.xy
 * (src/render/chunks.c: build_land_records), each cell holding its chunk's
 * record in chunks[] (-1: none). The terrain's prepass (terrain_pre.frag)
 * writes the chunk this finds into the G-buffer, so the shadow passes
 * (shadow_trace.comp, shadow_denoise.comp) serve the land as they do the
 * city's surfaces, and terrain.frag finds the same one for its light. Reads
 * only the frame's uniforms. */

// The chunk record whose light table lights the land at camera-relative x, z
// (as the chunks lie: flat, not bent over the planet), -1 for none: of the
// cell it lies in and the three beside it on its side (lamps reach a third of
// a cell, flares not much more than half), the first whose lights reach it
// (frame.landRects: the box they do), its own cell's first.
int landChunk(vec2 xz) {
    vec2 g = (xz - frame.landGrid.xy) / CHUNK_SIZE;
    ivec2 c = ivec2(floor(g)), s = ivec2(fract(g.x) < 0.5 ? -1 : 1, fract(g.y) < 0.5 ? -1 : 1);
    for (int k = 0; k < 4; ++k) {
        ivec2 q = c + ivec2(k & 1, k >> 1) * s;
        if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, ivec2(LAND_GRID)))) continue;
        int i = q.y * LAND_GRID + q.x;
        vec4 b = frame.landRects[i];   // (none: x0 > x1)
        if (all(greaterThanEqual(xz, b.xy)) && all(lessThanEqual(xz, b.zw))) return frame.landCells[i >> 2][i & 3];
    }
    return -1;
}

#endif
