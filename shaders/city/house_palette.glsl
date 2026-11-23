// The houses' palettes (src/gen/home_shell.c's finishes pick their entries): included by shaders/city/house.glsl
// (the shells) and shaders/proxy/proxy.frag (the far rows' prisms), so a house keeps its colours as it hands over.
// Needs house_surf.h's HW_* and HRM_* (common.glsl).

// The palettes (linear albedo): the walls' by finish, the roofs', the trim's, doors' and garage doors'.
const vec3 H_BRICK[8] = vec3[](vec3(0.30, 0.11, 0.07), vec3(0.20, 0.08, 0.055), vec3(0.36, 0.15, 0.08), vec3(0.42, 0.33, 0.20),
                               vec3(0.19, 0.11, 0.075), vec3(0.075, 0.068, 0.068), vec3(0.24, 0.24, 0.23), vec3(0.50, 0.44, 0.33));
const vec3 H_RENDER[16] = vec3[](vec3(0.66, 0.64, 0.60), vec3(0.64, 0.57, 0.44), vec3(0.60, 0.54, 0.44), vec3(0.46, 0.46, 0.45),
                                 vec3(0.36, 0.34, 0.31), vec3(0.56, 0.46, 0.31), vec3(0.56, 0.36, 0.13), vec3(0.50, 0.22, 0.12),
                                 vec3(0.64, 0.43, 0.31), vec3(0.62, 0.38, 0.38), vec3(0.40, 0.50, 0.58), vec3(0.42, 0.52, 0.40),
                                 vec3(0.66, 0.56, 0.20), vec3(0.20, 0.45, 0.45), vec3(0.48, 0.40, 0.55), vec3(0.38, 0.33, 0.27));
const vec3 H_SIDING[12] = vec3[](vec3(0.66, 0.66, 0.63), vec3(0.62, 0.57, 0.45), vec3(0.64, 0.58, 0.34), vec3(0.36, 0.42, 0.33),
                                 vec3(0.25, 0.32, 0.42), vec3(0.40, 0.41, 0.41), vec3(0.16, 0.17, 0.18), vec3(0.34, 0.09, 0.07),
                                 vec3(0.42, 0.53, 0.62), vec3(0.50, 0.42, 0.30), vec3(0.10, 0.20, 0.13), vec3(0.24, 0.15, 0.09));
const vec3 H_STONE[4] = vec3[](vec3(0.52, 0.47, 0.37), vec3(0.48, 0.36, 0.24), vec3(0.24, 0.15, 0.10), vec3(0.34, 0.34, 0.33));
const vec3 H_BLOCK[6] = vec3[](vec3(0.36, 0.35, 0.33), vec3(0.24, 0.24, 0.23), vec3(0.56, 0.55, 0.51), vec3(0.58, 0.44, 0.42),
                               vec3(0.42, 0.50, 0.56), vec3(0.55, 0.46, 0.34));
// HM5c2: the retrofits' cast concrete (boxes, rounded bays): pale cream, warm and cool greys, buff, a dark grey, and the
// painted ones the South Asian and Latin American sheets show (a faded pink, mint, ochre)
const vec3 H_CONCRETE[8] = vec3[](vec3(0.60, 0.58, 0.53), vec3(0.52, 0.50, 0.46), vec3(0.47, 0.48, 0.48), vec3(0.60, 0.54, 0.44),
                                  vec3(0.34, 0.34, 0.33), vec3(0.60, 0.42, 0.40), vec3(0.46, 0.56, 0.48), vec3(0.62, 0.49, 0.28));
const vec3 H_CLAY[4] = vec3[](vec3(0.30, 0.10, 0.05), vec3(0.40, 0.17, 0.08), vec3(0.22, 0.11, 0.07), vec3(0.16, 0.09, 0.07));
const vec3 H_SLATE[3] = vec3[](vec3(0.065, 0.075, 0.085), vec3(0.055, 0.055, 0.06), vec3(0.085, 0.075, 0.085));
const vec3 H_CONC[4] = vec3[](vec3(0.18, 0.12, 0.09), vec3(0.18, 0.18, 0.18), vec3(0.09, 0.09, 0.09), vec3(0.25, 0.11, 0.07));
const vec3 H_SHINGLE[6] = vec3[](vec3(0.07, 0.07, 0.07), vec3(0.16, 0.16, 0.15), vec3(0.15, 0.10, 0.07), vec3(0.08, 0.12, 0.09),
                                 vec3(0.25, 0.20, 0.14), vec3(0.15, 0.06, 0.05));
const vec3 H_METAL[6] = vec3[](vec3(0.30, 0.31, 0.32), vec3(0.25, 0.12, 0.06), vec3(0.35, 0.08, 0.06), vec3(0.10, 0.25, 0.18),
                               vec3(0.10, 0.18, 0.35), vec3(0.55, 0.55, 0.53));
const vec3 H_BARREL[3] = vec3[](vec3(0.42, 0.16, 0.08), vec3(0.50, 0.24, 0.10), vec3(0.36, 0.17, 0.10));
const vec3 H_GLAZED[3] = vec3[](vec3(0.06, 0.07, 0.09), vec3(0.24, 0.25, 0.26), vec3(0.04, 0.04, 0.045));
const vec3 H_TRIM[8] = vec3[](vec3(0.70, 0.70, 0.68), vec3(0.62, 0.58, 0.48), vec3(0.03, 0.03, 0.03), vec3(0.04, 0.12, 0.07),
                              vec3(0.30, 0.30, 0.30), vec3(0.12, 0.07, 0.04), vec3(0.04, 0.06, 0.14), vec3(0.35, 0.06, 0.04));
const vec3 H_DOOR[12] = vec3[](vec3(0.40, 0.04, 0.03), vec3(0.06, 0.12, 0.35), vec3(0.03, 0.05, 0.13), vec3(0.04, 0.16, 0.08),
                               vec3(0.025, 0.025, 0.025), vec3(0.68, 0.68, 0.66), vec3(0.62, 0.48, 0.08), vec3(0.28, 0.29, 0.30),
                               vec3(0.30, 0.18, 0.08), vec3(0.10, 0.05, 0.03), vec3(0.05, 0.25, 0.26), vec3(0.20, 0.06, 0.20));
const vec3 H_GARAGE[6] = vec3[](vec3(0.66, 0.66, 0.64), vec3(0.60, 0.55, 0.45), vec3(0.34, 0.35, 0.35), vec3(0.20, 0.13, 0.08),
                                vec3(0.08, 0.16, 0.10), vec3(0.10, 0.10, 0.11));

// The city's air on paint (HM3b, the setting weighed over the references): a painted or rendered surface's colour
// a little dulled and darkened. Brick and stone take theirs from their sets' soot.
vec3 houseSoot(vec3 c) { return mix(vec3(dot(c, vec3(0.2126, 0.7152, 0.0722))), c, 0.82) * 0.92; }
vec3 houseWallColour(uint fin, uint c) {
    if (fin == HW_BRICK)  return H_BRICK[c & 7u];
    if (fin == HW_RENDER) return houseSoot(H_RENDER[c & 15u]);
    if (fin == HW_SIDING) return houseSoot(H_SIDING[c % 12u]);
    if (fin == HW_STONE)  return H_STONE[c & 3u];
    if (fin == HW_CONCRETE) return houseSoot(H_CONCRETE[c & 7u]);
    return houseSoot(H_BLOCK[c % 6u]);
}
vec3 houseRoofColour(uint fin, uint c) {
    if (fin == HRM_CLAY)     return H_CLAY[c & 3u];
    if (fin == HRM_SLATE)    return H_SLATE[c % 3u];
    if (fin == HRM_CONCRETE) return H_CONC[c & 3u];
    if (fin == HRM_SHINGLE)  return H_SHINGLE[c % 6u];
    if (fin == HRM_METAL)    return houseSoot(H_METAL[c % 6u]);
    if (fin == HRM_BARREL)   return H_BARREL[c % 3u];
    if (fin == HRM_GLAZED)   return H_GLAZED[c % 3u];
    if (fin == HRM_FELT)     return c == 0u ? vec3(0.045) : vec3(0.11, 0.11, 0.105);
    return c == 0u ? vec3(0.32, 0.31, 0.29) : vec3(0.22, 0.21, 0.20);   // HRM_SLAB
}
