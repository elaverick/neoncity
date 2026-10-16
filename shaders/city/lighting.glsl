// Part of shaders/city.frag, which includes the parts in order: lighting: ambient, lamps, shopfronts, ray-traced shadow lookups, pane sheen.

/* ======================================================================
 * Lighting: street lamps and lit shopfronts come from the chunk's light
 * table, built from the same plan as the geometry, so light pools sit
 * exactly under the lamp heads and shop spill matches the lit storefronts.
 * ==================================================================== */

vec3 ambientLight(vec3 n, float h) {
    vec3 sky = vec3(0.006, 0.007, 0.013);
    vec3 bounce = vec3(0.013, 0.008, 0.006);
    float up = n.y * 0.5 + 0.5;
    return mix(bounce, sky, up) * (0.8 + 0.8 * exp(-h / 30.0));
}

/* Specular lobes. Procedural materials use a normalised Blinn-Phong lobe
 * (shin, specK) and pass rough < 0. Authored PBR materials pass rough >= 0
 * and get GGX (Trowbridge-Reitz distribution, height-correlated Smith
 * visibility, Schlick Fresnel, alpha = rough^2) with F0 = specK. Lighting here
 * leaves the 1/pi out of Lambert diffuse (diffuse is alb * E), so the GGX
 * lobe carries a factor of pi to stay in the same units. */
float ggxSpec(vec3 n, vec3 L, vec3 V, float alpha, float f0, float ndl) {
    vec3 H = normalize(L + V);
    float ndh = max(dot(n, H), 0.0), ndv = max(dot(n, V), 1e-4), vdh = max(dot(V, H), 0.0);
    float a2 = max(alpha * alpha, 1e-6);
    float D;
    vec3 tv = V - n * dot(n, V);   // the view direction across the surface
    float s = 1.0 + (gStreak - 1.0) * smoothstep(0.2, 0.7, length(tv));
    if (s > 1.001) {
        // Anisotropic GGX, wider along the view direction and narrower across
        // it (same area): rain-slicked streets streak lights toward the viewer.
        vec3 t = normalize(tv), b = cross(n, t);
        float ax = min(alpha * s, 1.0), ay = max(alpha / s, 1e-3);
        vec3 h = vec3(dot(H, t) / ax, dot(H, b) / ay, ndh);
        float q = dot(h, h);
        D = 1.0 / (PI * ax * ay * q * q);
    } else {
        float dd = ndh * ndh * (a2 - 1.0) + 1.0;
        D = a2 / (PI * dd * dd);
    }
    float vis = 0.5 / (ndl * sqrt(ndv * ndv * (1.0 - a2) + a2) + ndv * sqrt(ndl * ndl * (1.0 - a2) + a2) + 1e-5);
    float F = f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0);
    return PI * D * vis * F * ndl;
}

float specLobe(vec3 n, vec3 L, vec3 V, float shin, float specK, float rough, float ndl) {
    if (rough >= 0.0) return ggxSpec(n, L, V, max(rough * rough, 0.002), specK, ndl);
    vec3 H = normalize(L + V);
    return pow(max(dot(n, H), 0.0), shin) * specK * ndl * (shin + 8.0) / 25.0;
}

// Split-sum environment BRDF, analytic fit (Karis, "Physically Based Shading on Mobile").
float envBRDF(float f0, float rough, float ndv) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * ndv)) * r.x + r.y;
    vec2 AB = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * AB.x + AB.y;
}

void addLamp(vec3 p, vec3 n, vec3 V, float shin, float specK, float rough, vec3 lpos, vec3 lcol,
             inout vec3 diff, inout vec3 spec) {
    vec3 l = lpos - p;
    // the beam below the head, and a spill all round it (the glowing bowl, light
    // scattered off the wet street) that carries up the columns and facades
    float att = lampFalloff(l);
    if (att <= 0.0) return;
    vec3 L = normalize(l);
    float ndl = max(dot(n, L), 0.0);
    diff += lcol * att * ndl;
    spec += lcol * att * specLobe(n, L, V, shin, specK, rough, ndl);
}

// Coloured light thrown onto the sidewalk and street by a lit shopfront. The
// storefront is a large source: for GGX its size widens the lobe (alpha grows
// by its ~1.5 m half-width over twice the distance, as for a sphere light), so
// the sheen spreads like a reflection of the window, not a point highlight.
void addShop(vec3 p, vec3 n, vec3 V, float specK, float rough, vec3 lpos, vec3 col, inout vec3 diff, inout vec3 spec) {
    vec3 l = lpos - p;
    float d = length(l);
    if (d > 12.0) return;
    vec3 L = l / max(d, 1e-3);
    float ndl = max(dot(n, L), 0.0);
    vec3 c = col * shopFalloff(d);
    diff += c * (0.35 + 0.65 * ndl);
    if (rough >= 0.0) spec += c * ggxSpec(n, L, V, min(max(rough * rough, 0.002) + 0.75 / max(d, 0.5), 1.0), specK, ndl);
    else spec += c * specK * 0.6;
}

/* Facade faces are numbered within their section, round the outline. The
 * cell (window bay or 6 m shop bay) that wraps a convex corner is keyed as
 * the next face's first cell, so both windows of a corner room or shop look
 * into the same space. */
ivec2 cornerKey(int face, int next, bool endShare, int idx, int last) {
    return (endShare && idx >= last && last > 0) ? ivec2(next, 0) : ivec2(face, idx);
}

// Storefronts are 6 m bays along each ground-floor face (city_shop_hash in src/city/city.c).
uint shopHash(uint seed, int section, int face, int next, bool endShare, float along, float faceW) {
    ivec2 k = cornerKey(face, next, endShare, int(floor(along / 6.0)), int(floor((faceW - 0.01) / 6.0)));
    return hash3i(WORLD_SEED ^ 0x5B0Au, int(seed), section * 64 + k.x, k.y);
}

/* Ray-traced shadows (common.glsl). shadow_denoise.comp leaves soft shadows
 * for the most important lights of the surface the prepass saw at this pixel;
 * shadowsBegin (in main) reads them and checks they are this surface's. Any
 * other light bright enough to matter, and every light on a surface the
 * prepass didn't draw (glass, veils, a triangle's edge that misses sample 0),
 * gets a hard shadow traced here, to the middle of its emitter. */
#ifdef RT_SHADOWS
layout(set = 0, binding = 5) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 6) uniform usampler2D shadowIdsTex;   // shadow_trace.comp: slot light ids
layout(set = 0, binding = 7) uniform usampler2D shadowVisTex;   // shadow_denoise.comp: slot visibility
uvec4 gSlotIds = uvec4(0u), gSlotVis = uvec4(0u);
bool  gSlotsOurs = false;        // the slots at this pixel were made for this surface
vec3  gGeoN = vec3(0.0, 1.0, 0.0);
uint  gShadowCull = 0xFFu;       // instance mask (src/render/rt.h)
float gFwd, gFwdTol;             // this fragment's forward distance, and how near a slot's must be to be ours
#endif
float gShadowFade = 1.0;         // 1: unshadowed (shadows off, or faded out with distance)

void shadowsBegin() {
#ifdef RT_SHADOWS
    float fwd = dot(vRel, frame.camFwd.xyz);
    float tol = fwidth(fwd) * 1.5 + fwd * 0.002 + 0.01;   // the slots are for sample 0; this fragment is at the pixel's centre
    gFwd = fwd; gFwdTol = tol;
    gGeoN = normalize(vNrm);
    gShadowCull = vParams.x == MAT_LITTER ? 0xFFu & ~RT_MASK_LITTER : 0xFFu;   // litter's own shape moves (city.vert)
    if (!shadowsOn()) return;
    gShadowFade = smoothstep(SHADOW_FADE, SHADOW_FAR, length(vRel));
    ivec2 px = ivec2(gl_FragCoord.xy);
    gSlotVis = texelFetch(shadowVisTex, px, 0);
    gSlotsOurs = (gSlotVis.w & SH_VALID) != 0u && (gSlotVis.w & 0xFFFFu) == vChunk && abs(uintBitsToFloat(gSlotVis.z) - fwd) < tol;
    if (gSlotsOurs) gSlotIds = texelFetch(shadowIdsTex, px, 0);
#endif
}

#ifdef RT_SHADOWS
// 1 if nothing stands between this fragment (p, chunk-local) and the middle of the light's emitter.
float traceShadow(vec3 p, vec3 lp, uint w) {
    vec3 e = emitterCentre(lp, w, p) + gC.origin.xyz;
    vec3 nn = dot(gGeoN, e - vRel) >= 0.0 ? gGeoN : -gGeoN;
    vec3 o = vRel + nn * (0.01 + length(vRel) * 2e-4);
    vec3 toE = e - o;
    float len = length(toE);
    rayQueryEXT q;
    rayQueryInitializeEXT(q, tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, gShadowCull, o, 0.0, toE / len, max(len - 0.02, 0.0));
    while (rayQueryProceedEXT(q)) {}
    return rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}
#endif

// How much of light record w (at lp, chunk-local) reaches this fragment.
float shadowVis(vec3 p, vec3 n, vec3 V, vec3 lp, uint w) {
#ifdef RT_SHADOWS
    if (gShadowFade >= 1.0 || !castsShadow(w)) return 1.0;
    float v = -1.0;
    if (gSlotsOurs) {
        uint id = lightId(gC.info.xy, lp, w);
        for (int k = 0; k < SHADOW_SLOTS; ++k)
            if (((gSlotIds[k >> 1] >> (16u * uint(k & 1))) & 0xFFFFu) == id) v = float((gSlotVis[k >> 2] >> (8u * uint(k & 3))) & 255u) / 255.0;
        if (v < 0.0 && (gSlotVis.w & SH_OVERFLOW) == 0u) return 1.0;   // too faint here to show a shadow
    }
    if (v < 0.0) v = lightImportance(p, n, V, lp, w) < LIGHT_NEGLIGIBLE ? 1.0 : traceShadow(p, lp, w);
    return mix(v, 1.0, gShadowFade);
#else
    return 1.0;
#endif
}

/* How much of a lobby's ceiling panels' light reaches this fragment: the mean
 * of its ceiling slots (shadow_trace.comp: traceCeiling). A fragment the
 * prepass didn't see at sample 0 (the floor at a desk's edge) borrows a
 * neighbouring pixel's slots if they are its surface's, else is unshadowed. */
float ceilingVis() {
#ifdef RT_SHADOWS
    if (gShadowFade >= 1.0) return 1.0;
    uvec4 ids = gSlotIds, vis = gSlotVis;
    bool ours = gSlotsOurs;
    const ivec2 NB[4] = ivec2[](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1));
    for (int i = 0; i < 4 && !ours; ++i) {
        ivec2 q = ivec2(gl_FragCoord.xy) + NB[i];
        vis = texelFetch(shadowVisTex, q, 0);
        ours = (vis.w & SH_VALID) != 0u && (vis.w & 0xFFFFu) == vChunk && abs(uintBitsToFloat(vis.z) - gFwd) < gFwdTol * 2.0;
        if (ours) ids = texelFetch(shadowIdsTex, q, 0);
    }
    if (!ours) return 1.0;
    float sum = 0.0, cnt = 0.0;
    for (int k = 0; k < SHADOW_SLOTS; ++k) {
        uint id = (ids[k >> 1] >> (16u * uint(k & 1))) & 0xFFFFu;
        if (id < CEIL_ID || id >= CEIL_ID + uint(CEIL_SLOTS)) continue;
        sum += float((vis[k >> 2] >> (8u * uint(k & 3))) & 255u) / 255.0;
        cnt += 1.0;
    }
    return cnt > 0.0 ? mix(sum / cnt, 1.0, gShadowFade) : 1.0;
#else
    return 1.0;
#endif
}

// rough < 0: procedural Blinn-Phong (shin, specK); rough >= 0: GGX with F0 = specK.
void cityLights(vec3 p, vec3 n, vec3 V, float shin, float specK, float rough, inout vec3 diff, inout vec3 spec) {
    int base = gC.info.z;
    if (base < 0 || p.y > 32.0) return;
    vec4 hdr = uintBitsToFloat(lightRec[base]);
    ivec2 t = clamp(ivec2(floor((p.xz - hdr.xy) * hdr.zw)), ivec2(0), ivec2(3));
    int ti = t.y * 4 + t.x;
    uint tile = lightRec[base + 1 + (ti >> 2)][ti & 3];
    int first = base + int(tile >> 16), count = int(tile & 0xFFFFu);
    for (int i = 0; i < count; ++i) {
        uvec4 r = lightRec[first + i];
        vec3 lp = uintBitsToFloat(r.xyz);
        uint type = r.w & 3u;
        vec3 l = lp - p;
        if (dot(l, l) > (type < 2u ? 34.0 * 34.0 : 12.0 * 12.0)) continue;   // out of its reach (addLamp, addShop)
        float vis = shadowVis(p, n, V, lp, r.w);
        if (vis <= 0.0) continue;
        if (type < 2u) addLamp(p, n, V, shin, specK, rough, lp, lampColor(int(type)) * lampState(r.w >> 2, gTime) * vis, diff, spec);
        else addShop(p, n, V, specK, rough, lp, (type == 2u ? shopColor(r.w) : LOBBY_SPILL) * vis, diff, spec);
    }
}

// Soft coloured light from the sea of signs, strongest near street level.
vec3 neonBounce(vec3 w, float h) {
    float m = vnoise(w.xz / 23.0, 0xB0B0u);
    float k = vnoise(w.xz / 17.0 + 7.3, 0xB0B1u);
    vec3 c = mix(vec3(1.0, 0.1, 0.5), vec3(0.1, 0.8, 1.0), smoothstep(0.3, 0.7, m));
    return c * smoothstep(0.35, 0.8, k) * exp(-h / 14.0) * 0.035;
}

/* Pane sheen: a soft, blurred reflection that gives glass its presence where
 * screen-space reflections can't reach (glass seen near head-on reflects the
 * street behind the camera, which isn't on screen). A view-dependent gradient
 * (the dark street below, a glowing band of the lit facades across the way,
 * the sodium-lit overcast above) plus soft blooms where the street lamps
 * nearby would reflect. Strongest head-on, it fades out as the
 * reflected ray turns toward the screen and SSR takes over (ssr.comp), so the
 * two never double up. Added on top of the pane: it shows on dark windows and
 * is drowned out by lit rooms, as real night glass is. */
vec3 paneSheen(vec3 p, vec3 n, vec3 V) {
    vec3 R = reflect(-V, n);
    float reach = 1.0 - smoothstep(-0.3, 0.1, dot(R, frame.camFwd.xyz));   // SSR's own cut-off, inverted
    if (reach <= 0.0) return vec3(0.0);
    float F = glassFresnel(n, V);

    // the gradient: street, facades across, overcast (blurred: low frequencies only)
    float az = atan(R.z, R.x);
    float across = smoothstep(-0.35, -0.02, R.y) * (1.0 - smoothstep(0.05, 0.45, R.y));
    float rhythm = 0.55 + 0.45 * vnoise(vec2(az * 9.0 + gWorld.x * 0.03, 0.0), 0x5EE1u);
    vec3 c = mix(vec3(0.010, 0.010, 0.013), CLOUD_DECK * 0.8, smoothstep(0.0, 0.5, R.y))
           + vec3(0.11, 0.085, 0.06) * across * rhythm
           + neonBounce(gWorld, p.y) * 4.0 * across;

    // soft blooms of the street lamps nearby, where they'd reflect
    int base = gC.info.z;
    if (base >= 0 && p.y < 40.0) {
        vec4 hdr = uintBitsToFloat(lightRec[base]);
        ivec2 t = clamp(ivec2(floor((p.xz - hdr.xy) * hdr.zw)), ivec2(0), ivec2(3));
        int ti = t.y * 4 + t.x;
        uint tile = lightRec[base + 1 + (ti >> 2)][ti & 3];
        int first = base + int(tile >> 16), count = int(tile & 0xFFFFu);
        for (int i = 0; i < count; ++i) {
            uvec4 r = lightRec[first + i];
            vec3 l = uintBitsToFloat(r.xyz) - p;
            float d = length(l);
            if (dot(l, n) <= 0.0) continue;   // behind the pane
            // Shop and lobby lights stand in for the glow spilling out of the
            // glass, not for anything you could see: they'd reflect as balls
            // of light floating at chest height.
            uint type = r.w & 3u;
            if (type >= 2u) continue;
            vec3 lc = lampColor(int(type)) * lampState(r.w >> 2, gTime);
            float k = max(dot(R, l / d), 0.0);
            float bloom = pow(k, 40.0) * 3.0 + pow(k, 6.0) * 0.25;   // a soft core in a wide halo
            c += lc * bloom * 30.0 / (d * d + 30.0);
        }
    }
    return c * F * reach * SHEEN;
}
