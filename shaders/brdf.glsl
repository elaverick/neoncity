#ifndef BRDF_GLSL
#define BRDF_GLSL
/* The GGX microfacet lobe, shared by the city's lights (shaders/city/lighting.glsl:
 * ggxSpec) and the interior baker's path tracer (shaders/interior/pathtrace.glsl),
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

#endif
