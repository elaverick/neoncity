#ifndef BRDF_GLSL
#define BRDF_GLSL
/* The GGX microfacet lobe, shared by the city's lights (shaders/city/lighting.glsl:
 * ggxSpec), the land's wet sheen (shaders/terrain/ground.glsl) and the interior
 * baker's path tracer (shaders/interior/pathtrace.glsl),
 * so a surface baked into an impostor reflects as a live one does: the
 * Trowbridge-Reitz distribution, the height-correlated Smith visibility and
 * Schlick's Fresnel, with alpha = roughness^2 and a2 = alpha^2. */

// The distribution of microfacet normals, at the cosine ndh between the half vector and the normal.
float brdfGgxD(float ndh, float a2) {
    float dd = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / (3.14159265 * dd * dd);
}

// G2 / (4 ndl ndv), height-correlated.
float brdfSmithVis(float ndl, float ndv, float a2) {
    return 0.5 / (ndl * sqrt(ndv * ndv * (1.0 - a2) + a2) + ndv * sqrt(ndl * ndl * (1.0 - a2) + a2) + 1e-5);
}

float brdfSchlick(float f0, float vdh) { return f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0); }
vec3  brdfSchlick(vec3 f0, float vdh)  { return f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0); }

// Split-sum environment BRDF, analytic fit (Karis, "Physically Based Shading on Mobile").
float envBRDF(float f0, float rough, float ndv) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * ndv)) * r.x + r.y;
    vec2 AB = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * AB.x + AB.y;
}

#endif
