// Part of shaders/city.frag, which includes the parts in order: the vendors' kit.

/* ======================================================================
 * The vendors' kit (src/gen/pmodel.h modules, drawn instanced by
 * src/render/kit_gpu.c through shaders/kit.vert), lit as the street furniture
 * is. params y is the part's material (PMAT_*), z the piece's variation (the
 * instance's part), w its flags; aux the piece of the module; the seed the
 * instance's. uv runs in metres along the surface. kit.vert passes the baked
 * occlusion and wear (128 flat, up on convex edges, down in concave corners)
 * in vLitter.
 *
 * NB2's stand-in look: a flat colour, metalness and roughness a material,
 * darkened by the occlusion, the edges worn bright and the corners grimed by
 * the wear. NB3 replaces it with authored sets (docs/noodle-bar-target.md).
 * ==================================================================== */

const uint PMAT_RUST = 0u, PMAT_PLATE = 1u, PMAT_STAINLESS = 2u, PMAT_COPPER = 3u, PMAT_CAST_IRON = 4u, PMAT_PAINTED = 5u,
           PMAT_TILE = 6u, PMAT_RUBBER = 7u, PMAT_ENAMEL = 8u, PMAT_GLASS = 9u, PMAT_BRASS = 10u, PMAT_TIMBER = 11u,
           PMAT_CHALK = 12u, PMAT_CERAMIC = 13u, PMAT_BAMBOO = 14u, PMAT_PAPER = 15u, PMAT_PLASTIC = 16u, PMAT_GALV = 17u,
           PMAT_STEEL = 18u, PMAT_DIAL = 19u, PMAT_RED = 20u, PMAT_BROTH = 21u, PMAT_NOODLE = 22u, PMAT_CURRY = 23u,
           PMAT_RICE = 24u, PMAT_OIL = 25u, PMAT_GINGER = 26u;   // src/gen/pmodel.h

// rgb: base colour (linear), a: metalness; then the roughness
const vec4 KIT_BASE[27] = vec4[27](
    vec4(0.20, 0.085, 0.045, 0.1), vec4(0.10, 0.12, 0.11, 0.0), vec4(0.56, 0.56, 0.55, 1.0), vec4(0.62, 0.34, 0.22, 1.0),
    vec4(0.035, 0.034, 0.033, 0.5), vec4(0.17, 0.05, 0.035, 0.0), vec4(0.68, 0.68, 0.64, 0.0), vec4(0.018, 0.018, 0.018, 0.0),
    vec4(0.72, 0.71, 0.67, 0.0), vec4(0.04, 0.05, 0.045, 0.0), vec4(0.74, 0.56, 0.28, 1.0), vec4(0.22, 0.13, 0.07, 0.0),
    vec4(0.03, 0.035, 0.03, 0.0), vec4(0.62, 0.60, 0.54, 0.0), vec4(0.42, 0.30, 0.15, 0.0), vec4(0.58, 0.53, 0.42, 0.0),
    vec4(0.02, 0.02, 0.02, 0.0), vec4(0.48, 0.49, 0.49, 1.0), vec4(0.11, 0.11, 0.11, 0.8), vec4(0.74, 0.72, 0.64, 0.0),
    vec4(0.33, 0.03, 0.02, 0.0), vec4(0.33, 0.18, 0.07, 0.0), vec4(0.62, 0.52, 0.28, 0.0), vec4(0.42, 0.23, 0.04, 0.0),
    vec4(0.78, 0.76, 0.70, 0.0), vec4(0.33, 0.04, 0.01, 0.0), vec4(0.72, 0.43, 0.38, 0.0));
const float KIT_ROUGH[27] = float[27](0.82, 0.6, 0.32, 0.4, 0.75, 0.55, 0.2, 0.8, 0.25, 0.05, 0.35, 0.7, 0.9, 0.3, 0.6, 0.9,
                                      0.4, 0.5, 0.5, 0.4, 0.45, 0.08, 0.5, 0.35, 0.6, 0.08, 0.4);

vec3 shadeKit(vec3 p, vec3 rel, vec3 n, vec2 uv, uint seed, uvec4 prm, float occ, float wear) {
    vec3 V = -normalize(rel);
    uint m = min(prm.y, 26u);
    vec4 b = KIT_BASE[m];
    vec3 base = b.rgb * mix(0.85, 1.12, hf(seed, prm.z + 7u));
    float metal = b.a, rough = KIT_ROUGH[m];
    // the baked wear: paint and rust chipped to bare steel on the edges, grime in the corners
    float edge = smoothstep(0.58, 0.9, wear), corner = smoothstep(0.45, 0.15, wear);
    if (m == PMAT_PAINTED || m == PMAT_RUST || m == PMAT_PLATE || m == PMAT_RED) {
        base = mix(base, vec3(0.3, 0.29, 0.28), edge * 0.7);
        metal = mix(metal, 0.9, edge * 0.7);
        rough = mix(rough, 0.45, edge * 0.7);
    } else {
        base *= 1.0 + 0.25 * edge;
    }
    base *= 1.0 - 0.45 * corner;
    rough = mix(rough, 0.9, corner * 0.5);
    float ao = mix(0.2, 1.0, occ);

    float wet = WETNESS * (1.0 - shelterAt(rel));
    rough = mix(rough, rough * 0.45, wet);
    base *= mix(1.0, 0.8, wet * (1.0 - metal));
    vec3 alb = base * (1.0 - metal);
    float f0 = mix(m == PMAT_GLASS ? 0.06 : 0.04, 0.85, metal);
    vec3 tint = mix(vec3(1.0), clamp(base / max(max(base.r, max(base.g, base.b)), 1e-3), 0.0, 1.0), metal);
    return litterLight(p, n, V, alb, rough, f0, tint, ao);
}
