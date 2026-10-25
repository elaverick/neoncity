// Part of shaders/city.frag, which includes the parts in order: street furniture.

/* ======================================================================
 * Street furniture (src/city/props.c): models baked by tools/model_pack.c,
 * in the near-only props layer, lit by their chunk's lamps and shopfronts as
 * the litter is. params y is the model's material (PBR_*), z the model (PM_*
 * in src/city/models.h), w its flags (PF_* in src/city/city.h); the
 * seed is the piece's own (a crossing's, shared by both its kerbs). uv runs
 * over the model's atlas. Its maps are a material's (assets/materials/), but
 * orm.b is the metalness, and the normal map is in the frame the bake used
 * (modelFrame in surface.glsl). Out in the rain a piece turns
 * glossy and beads with drops; a signal head's red hand and green man take
 * turns to light.
 * ==================================================================== */

const uint PM_SIGNAL = 7u;

// A pedestrian signal's phase (seconds into its cycle of SIGNAL_CYCLE): the
// green man for SIGNAL_WALK, flashing for SIGNAL_FLASH, then the red hand.
const float SIGNAL_CYCLE = 45.0, SIGNAL_WALK = 9.0, SIGNAL_FLASH = 5.0;
vec2 signalLamps(uint seed) {
    float t = mod(gTime + hfloat(seed) * SIGNAL_CYCLE, SIGNAL_CYCLE);
    float walk = t < SIGNAL_WALK ? 1.0 : t < SIGNAL_WALK + SIGNAL_FLASH ? step(0.5, fract(t * 1.25)) : 0.0;
    return vec2(t >= SIGNAL_WALK + SIGNAL_FLASH ? 1.0 : 0.0, walk);   // hand, man
}

vec3 shadeProp(vec3 p, vec3 rel, vec3 n, vec2 uv, uint seed, uvec4 prm) {
    vec3 V = -normalize(rel);
    int m = int(prm.y);
    if (!pbrLoaded(m)) {   // its maps are missing: a plain dark painted shape
        return litterLight(p, n, V, vec3(0.05), 0.6, 0.04, vec3(1.0), 1.0);
    }
    vec3 T, B;
    modelFrame(p, uv, n, T, B);
    Surface s = samplePBR(m, uv, T, B, n);
    float metal = s.height, rough = s.rough;
    vec3 base = s.albedo * pbrAlbedoScale(m) * mix(0.8, 1.05, hf(seed, 3u));   // some kept cleaner than others

    // rain on what stands in the open: glossier, and beaded with drops
    float wet = WETNESS * (1.0 - shelterAt(rel));
    rough = mix(rough, rough * 0.45, wet);
    vec3 nn = beads(p, s.n, wet, rough);
    base *= mix(1.0, 0.8, wet * (1.0 - metal));

    vec3 alb = base * (1.0 - metal);
    float f0 = mix(0.04, 0.8, metal);
    vec3 tint = mix(vec3(1.0), clamp(base / max(max(base.r, max(base.g, base.b)), 1e-3), 0.0, 1.0), metal);
    vec3 col = litterLight(p, nn, V, alb, rough, f0, tint, s.ao);

    if (prm.z == PM_SIGNAL && (materials[m].info.x & PBR_HAS_EMISSIVE) != 0u) {
        vec2 e = texture(pbrMaps[m * PBR_MAPS_PER_MATERIAL + PBR_EMISSIVE], uv).rg;
        vec2 on = signalLamps(seed);
        col += vec3(1.0, 0.10, 0.03) * 5.0 * e.r * mix(0.03, 1.0, on.x) + vec3(0.25, 1.0, 0.7) * 4.0 * e.g * mix(0.03, 1.0, on.y);
    }
    return col;
}
