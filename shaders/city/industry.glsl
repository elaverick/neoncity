// Part of shaders/city.frag, which includes the parts in order: industrial plants and substations.

/* ======================================================================
 * Industrial plants and substations (src/city/mesh_industry.c). params y
 * surface (IND_* in src/core/common.h), z paint (low 4 bits; PA_* in
 * src/gen/building.h) | pattern << 4 (1 hoops, 2 cladding ribs, 4 heavy
 * grime). Paint, gratings and glazing are lit like the facades
 * (wallLighting); the fences' wire mesh is a veil over one MSAA sample (veil,
 * arcology.glsl). A stack's plume and a flare's fire are volumes of their own
 * (shaders/smoke.frag, shaders/flame.frag); IND_STEAM is no longer emitted.
 * ==================================================================== */

const uint IND_PAINT = 0u, IND_GRATING = 1u, IND_STEAM = 2u, IND_GLOW = 3u, IND_MESH = 4u;

vec3 industryPaint(uint k) {
    const vec3 P[16] = vec3[16](
        vec3(0.150, 0.150, 0.142),   // pale grey
        vec3(0.180, 0.155, 0.112),   // cream
        vec3(0.190, 0.078, 0.024),   // orange
        vec3(0.140, 0.032, 0.024),   // red
        vec3(0.080, 0.150, 0.122),   // mint
        vec3(0.200, 0.150, 0.024),   // yellow
        vec3(0.017, 0.017, 0.018),   // soot black
        vec3(0.210, 0.210, 0.200),   // white
        vec3(0.068, 0.070, 0.073),   // galvanised steel
        vec3(0.072, 0.038, 0.022),   // rust
        vec3(0.118, 0.082, 0.034),   // ochre
        vec3(0.024, 0.048, 0.032),   // dark green
        vec3(0.088, 0.093, 0.098),   // cladding
        vec3(0.034, 0.035, 0.039),   // dark steel
        vec3(0.092, 0.062, 0.028),   // brass and glazed ceramic
        vec3(0.044, 0.058, 0.078));  // blue-grey
    return P[k & 15u];
}

vec3 shadeIndustry(vec3 p, vec3 rel, vec3 n, vec2 uv, uint seed, uvec4 prm) {
    vec3 V = -normalize(rel);
    uint surf = prm.y;
    float y = p.y - gBase;
    if (surf == IND_GLOW) {
        // glazing lit from within: amber saw-tooth roofs, the refinery's green windows
        vec3 c = (prm.z & 15u) == 11u ? vec3(0.35, 1.0, 0.45) : vec3(1.0, 0.6, 0.22);
        float mull = max(step(0.93, fract(uv.x / 1.2)), step(0.9, fract(uv.y / 1.0)));
        return mix(c * (1.6 + 0.8 * hf(seed, uint(floor(uv.x / 1.2)))), vec3(0.01), mull);
    }
    if (surf == IND_MESH) {
        // chain-link: a diamond lattice of galvanised wire, the gaps let the view through
        veil();
        vec2 q = vec2(uv.x + uv.y, uv.x - uv.y) / 0.06;
        vec2 f = abs(fract(q) - 0.5);
        float aa = max(fwidth(q.x), fwidth(q.y));
        float wire = 1.0 - smoothstep(0.36 - aa, 0.36 + aa, min(f.x, f.y) + 0.14);
        wire = max(wire, smoothstep(0.35, 1.0, aa));   // far off the lattice blurs to grey
        if (wire < 0.25) discard;
        vec3 spec;
        vec3 diff = wallLighting(p, n, V, 30.0, 0.15, y, spec);
        return industryPaint(prm.z & 15u) * diff + spec;
    }

    vec3 alb = industryPaint(prm.z & 15u);
    uint pat = (prm.z >> 4) & 15u;
    // weathering: blotchy grime, streaks run down from seams, dirt splashed up from the ground
    float g = fbm(vec2(uv.x * 0.5, y * 0.18), seed & 0xFFFFu);
    float streak = vnoise(vec2(uv.x * 3.0, y * 0.05), (seed >> 16) | 1u);
    float heavy = (pat & 4u) != 0u ? 1.0 : 0.0;
    alb *= mix(0.7, 1.15, g) * (1.0 - (0.25 + 0.3 * heavy) * smoothstep(0.55, 0.9, streak));
    alb = mix(alb, vec3(0.025, 0.022, 0.02), (0.35 + 0.3 * heavy) * (1.0 - smoothstep(0.0, 2.5, y)));
    if ((pat & 1u) != 0u) {   // stiffening hoops round a shell
        float fh = fract(y / 2.4), aa = fwidth(y / 2.4) + 0.002;
        alb *= 1.0 - 0.45 * band(fh, 0.0, 0.05, aa);
    }
    if ((pat & 2u) != 0u) {   // profiled cladding
        float r = fract(uv.x / 0.25), aa = fwidth(uv.x / 0.25) + 0.002;
        alb *= 0.82 + 0.18 * band(r, 0.15, 0.55, aa);
    }
    float shin = 30.0, specK = 0.18;
    if (surf == IND_GRATING) {   // open steel grating: dark between its bars
        vec2 q = uv / vec2(0.035, 0.1);
        vec2 aa = fwidth(q) + 0.002;
        float bars = max(band(fract(q.x), 0.0, 0.35, aa.x), band(fract(q.y), 0.0, 0.2, aa.y));
        bars = mix(bars, 0.45, smoothstep(0.3, 1.0, max(aa.x, aa.y)));
        alb *= mix(0.15, 1.0, bars);
        specK = 0.1;
    }
    vec3 spec;
    vec3 diff = wallLighting(p, n, V, shin, specK, y, spec);
    return alb * diff + spec;
}
