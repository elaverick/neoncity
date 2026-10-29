/* The interior baker's path tracer (tools/interior_bake; docs/interior-impostors.md,
 * sections 9 and 15): radiance from the module's own lights with full global
 * illumination, for the capture (capture.comp) and the perspective truth
 * (truth.comp, which the tests render). Included after bake_scene.glsl.
 *
 * - Surfaces: Lambert diffuse plus GGX specular in the metallic-roughness
 *   model, under an optional clearcoat (the polished concrete). The GGX lobe
 *   is brdf.glsl's, the one the city shades with. Specular directions are
 *   drawn from the visible normals (Dupuy and Benyoub 2023), and a lobe is
 *   chosen by its share of the reflected light, so the pdf is the mixture's.
 * - Thin glass (partitions, BF_GLASS): Fresnel picks reflection or straight
 *   transmission, which loses P_glass.y. Glass is a delta surface, so the light
 *   it passes is never weighed against next-event estimation.
 * - Leaves (BF_OPEN) are cut to shape in their geometry (src/gen/plant.c), so
 *   they need no alpha test: a leaf is a two-sided opaque surface.
 * - Lights are the emissive triangles (emitters, built by gpu.c): one is
 *   chosen by its power from an alias table, a point uniformly on it, and,
 *   in a periodic module, one of the three copies of it round the point.
 *   Next-event estimation is weighed against BSDF sampling by the power
 *   heuristic. Emission is one-sided, on a triangle's front.
 * - Russian roulette after the second bounce, and a clamp on what any one
 *   indirect sample brings (P_trace3.x).
 * - Every sample has its own random stream, from the texel, the layer and the
 *   sample's index, so the samples a bake draws don't depend on how its
 *   passes are split (their sums may differ in the last bits).
 *
 * Units: radiance as the materials' emission gives it; IM4 makes them
 * photometric and calibrates them against the engine. */
#include "brdf.glsl"

#define PT_PI 3.14159265
#define PT_NO_EMITTER 0xFFFFFFFFu

// An emissive triangle: p0.xyz a corner, p0.w its area; e1, e2 its edges from p0 (front: cross(e1, e2)),
// e1.w the alias table's probability and info.y its alias; e2.w the chance it is chosen (power / total); info.x its material.
struct Emitter { vec4 p0; vec4 e1; vec4 e2; uvec4 info; };
layout(std430, set = 0, binding = 9) readonly buffer Emitters { Emitter emitters[]; };

/* ----------------------------------------------------------- randomness --- */

uint ptHash(uint x) {   // lowbias32 (Wellons)
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

// A stream for one sample of one thing (a texel's layer, a pixel) in this bake.
uint ptSeed(uint thing, uint sub, uint sample_) {
    return ptHash(thing ^ ptHash(sub ^ ptHash(sample_ ^ ptHash(uint(P_trace2.w)))));
}

float ptRand(inout uint s) {   // PCG-RXS-M-XS
    s = s * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    w = (w >> 22u) ^ w;
    return float(w >> 8) * (1.0 / 16777216.0);
}

/* --------------------------------------------------------------- surfaces --- */

float ptLum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// An orthonormal basis round n (Duff et al. 2017).
void ptBasis(vec3 n, out vec3 t, out vec3 b) {
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z), c = n.x * n.y * a;
    t = vec3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = vec3(c, s + n.y * n.y * a, -n.y);
}

struct Bsdf {
    vec3  diff, f0, kD;   // the diffuse colour, the specular F0, and what reaches the diffuse past the specular and the coat
    float a2, alpha, kS;  // the base's GGX; kS what reaches it past the coat
    float coat, coatA2, coatAlpha;
    bool  spec;           // the base has a specular lobe
    float pD, pS;         // the chances of sampling the diffuse and the base's specular (the coat: the rest)
};

float ptG1(float ndv, float a2) { return 2.0 * ndv / (ndv + sqrt(a2 + (1.0 - a2) * ndv * ndv)); }

Bsdf ptBsdf(uint m, float ndv) {
    Mat mt = mats[m];
    Bsdf b;
    float metal = mt.emission.a;
    b.diff = mt.albedo.rgb * (1.0 - metal);
    b.f0 = mix(vec3(mt.surface.x), mt.albedo.rgb, metal);
    b.spec = metal > 0.0 || mt.surface.x > 0.0;
    b.alpha = max(mt.albedo.a * mt.albedo.a, 0.002);
    b.a2 = b.alpha * b.alpha;
    b.coat = mt.surface.y;
    b.coatAlpha = max(mt.surface.z * mt.surface.z, 0.002);
    b.coatA2 = b.coatAlpha * b.coatAlpha;
    vec3 Fv = b.spec ? brdfSchlick(b.f0, ndv) : vec3(0.0);
    float Fc = b.coat * brdfSchlick(0.04, ndv);
    b.kS = 1.0 - Fc;
    b.kD = (1.0 - Fv) * b.kS;
    float wD = ptLum(b.diff * b.kD), wS = ptLum(Fv) * b.kS, wC = Fc, sum = wD + wS + wC;
    if (sum <= 0.0) { b.pD = 1.0; b.pS = 0.0; }
    else { b.pD = wD / sum; b.pS = wS / sum; }
    return b;
}

// f(V, L) cos(L), and the pdf (solid angle) with which ptSample draws L.
vec3 ptEval(Bsdf b, vec3 n, vec3 V, vec3 L, out float pdf) {
    float ndl = dot(n, L), ndv = max(dot(n, V), 1e-4);
    pdf = 0.0;
    if (ndl <= 0.0) return vec3(0.0);
    vec3 H = normalize(V + L);
    float ndh = max(dot(n, H), 0.0), vdh = max(dot(V, H), 0.0);
    vec3 f = b.diff * b.kD * (1.0 / PT_PI);
    pdf = b.pD * ndl * (1.0 / PT_PI);
    if (b.spec) {
        float D = brdfGgxD(ndh, b.a2);
        f += b.kS * D * brdfSmithVis(ndl, ndv, b.a2) * brdfSchlick(b.f0, vdh);
        pdf += b.pS * D * ptG1(ndv, b.a2) / (4.0 * ndv);
    }
    if (b.coat > 0.0) {
        float D = brdfGgxD(ndh, b.coatA2);
        f += vec3(b.coat * D * brdfSmithVis(ndl, ndv, b.coatA2) * brdfSchlick(0.04, vdh));
        pdf += (1.0 - b.pD - b.pS) * D * ptG1(ndv, b.coatA2) / (4.0 * ndv);
    }
    return f * ndl;
}

// A visible microfacet normal, in the frame where the normal is +z (Dupuy and Benyoub 2023).
vec3 ptVndf(vec3 v, float alpha, vec2 u) {
    vec3 vh = normalize(vec3(alpha * v.x, alpha * v.y, v.z));
    float phi = 2.0 * PT_PI * u.x;
    float z = (1.0 - u.y) * (1.0 + vh.z) - vh.z;
    float st = sqrt(clamp(1.0 - z * z, 0.0, 1.0));
    vec3 hh = vec3(st * cos(phi), st * sin(phi), z) + vh;
    return normalize(vec3(alpha * hh.x, alpha * hh.y, max(hh.z, 1e-6)));
}

// Draws L; returns f cos / pdf (0: the draw failed).
vec3 ptSample(Bsdf b, vec3 n, vec3 V, vec3 u, out vec3 L, out float pdf) {
    vec3 t, bt;
    ptBasis(n, t, bt);
    if (u.z < b.pD) {   // cosine-weighted
        float r = sqrt(u.x), phi = 2.0 * PT_PI * u.y;
        L = t * (r * cos(phi)) + bt * (r * sin(phi)) + n * sqrt(max(1.0 - u.x, 0.0));
    } else {
        float alpha = u.z < b.pD + b.pS ? b.alpha : b.coatAlpha;
        vec3 h = ptVndf(vec3(dot(V, t), dot(V, bt), dot(V, n)), alpha, u.xy);
        L = reflect(-V, t * h.x + bt * h.y + n * h.z);
    }
    vec3 f = ptEval(b, n, V, L, pdf);
    return pdf > 0.0 ? f / pdf : vec3(0.0);
}

/* ------------------------------------------------------------------ lights --- */

// What a sample brings, at most cap in its brightest channel (cap 0: no limit).
vec3 ptClamp(vec3 c, float cap) {
    float m = max(c.r, max(c.g, c.b));
    return cap > 0.0 && m > cap ? c * (cap / m) : c;
}

// How much of the light from o along d (unit) reaches tmax: 0 behind anything opaque, less a little through each pane.
float ptVisible(vec3 o, vec3 d, float tmax) {
    if (tmax <= 0.0) return 1.0;
    rayQueryEXT q;
    rayQueryInitializeEXT(q, tlas, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, MASK_CASTER | MASK_EMITTER, o, 0.0, d, tmax);
    while (rayQueryProceedEXT(q)) {}
    if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionTriangleEXT) return 0.0;
    float T = 1.0, t = 0.0;
    for (int i = 0; i < 8; ++i) {
        Hit h;
        if (!traceMasked(o, d, t, tmax, MASK_GLASS, h)) break;
        if (h.front) T *= (1.0 - glassReflect(abs(dot(h.n, d)))) * P_glass.y;
        t = h.t + 1e-4;
    }
    return T;
}

vec3 ptCentroid(Emitter e) { return e.p0.xyz + (e.e1.xyz + e.e2.xyz) * (1.0 / 3.0); }

// The copy of a periodic module's emitter nearest x (in periods from the one in the buffer).
float ptCopy(float x, Emitter e) { return floor((x - ptCentroid(e).x) / P_module.w + 0.5); }

// The pdf (solid angle at p) with which ptNee would have drawn point q, seen along a ray of
// cosine cosL to emitter e's normal. 0 where it would not: a copy beyond the three round p.
float ptEmitterPdf(Emitter e, vec3 p, vec3 q, float cosL) {
    float copies = 1.0;
    if (P_module.w > 0.0) {
        if (abs(ptCopy(q.x, e) - ptCopy(p.x, e)) > 1.5) return 0.0;
        copies = 3.0;
    }
    vec3 d = q - p;
    return e.e2.w / (copies * e.p0.w) * dot(d, d) / max(cosL, 1e-6);
}

// Next-event estimation at surface point p (normal n, offset start o) seen from V: a point on a light, MIS-weighted.
vec3 ptNee(vec3 p, vec3 o, vec3 n, vec3 V, Bsdf b, inout uint rng) {
    int count = P_trace2.z;
    float x = ptRand(rng) * float(count);
    int i = min(int(x), count - 1);
    Emitter e = emitters[i];
    if (x - float(i) >= e.e1.w) e = emitters[e.info.y];
    float su = sqrt(ptRand(rng)), v = ptRand(rng);
    vec3 q = e.p0.xyz + e.e1.xyz * (su * (1.0 - v)) + e.e2.xyz * (su * v);
    if (P_module.w > 0.0) q.x += (ptCopy(p.x, e) + floor(ptRand(rng) * 3.0) - 1.0) * P_module.w;
    vec3 nq = normalize(cross(e.e1.xyz, e.e2.xyz)), d = q - p;
    float dist = length(d);
    if (dist < 1e-5) return vec3(0.0);
    vec3 l = d / dist;
    float cosL = -dot(nq, l);
    if (cosL <= 0.0 || dot(n, l) <= 0.0) return vec3(0.0);
    float pdfB;
    vec3 f = ptEval(b, n, V, l, pdfB);
    if (max(f.r, max(f.g, f.b)) <= 0.0) return vec3(0.0);
    vec3 ds = q - o;
    float ls = length(ds);
    float T = ptVisible(o, ds / ls, ls - 2e-3);
    if (T <= 0.0) return vec3(0.0);
    float pl = ptEmitterPdf(e, p, q, cosL);
    float w = pl * pl / (pl * pl + pdfB * pdfB);
    return f * mats[e.info.x].emission.rgb * (T * w / pl);
}

/* ------------------------------------------------------------------- paths --- */

// Follows a ray from o along d to the first opaque surface, through panes (each picks
// reflection or transmission). thr loses what the panes absorb; delta becomes true where
// a pane reflected the ray. false: it left the building.
bool ptFollow(inout vec3 o, inout vec3 d, inout vec3 thr, inout bool delta, inout uint rng, out Hit h) {
    for (int g = 0; g < 8; ++g) {
        if (!traceNext(o, d, 0.0, pathExit(o, d), h)) return false;
        if ((insts[h.inst].flags & BF_GLASS) == 0u) return true;
        vec3 q = o + d * h.t;
        if (h.front) {
            if (ptRand(rng) < glassReflect(abs(dot(h.n, d)))) { d = reflect(d, h.n); delta = true; }
            else thr *= P_glass.y;
        }
        o = q + d * 1e-4;
    }
    return false;
}

/* The radiance leaving surface point p (normal n on V's side, material m)
 * towards V, from the lights by every path of up to P_trace.w bounces. The
 * surface's own emission is not included. */
vec3 ptRadiance(vec3 p, vec3 n, vec3 V, uint m, inout uint rng) {
    vec3 L = vec3(0.0), thr = vec3(1.0);
    float cap = P_trace3.x;
    bool nee = P_trace3.y > 0.5 && P_trace2.z > 0;
    for (int bounce = 0; bounce < 64; ++bounce) {
        if (bounce >= P_trace.w) break;
        float capNow = bounce > 0 ? cap : 0.0;   // the first bounce's light is direct: never clamped
        Bsdf b = ptBsdf(m, max(dot(n, V), 1e-4));
        vec3 o = p + n * 1e-3;
        if (nee) L += ptClamp(thr * ptNee(p, o, n, V, b, rng), capNow);
        vec3 wi;
        float pdf;
        vec3 w = ptSample(b, n, V, vec3(ptRand(rng), ptRand(rng), ptRand(rng)), wi, pdf);
        if (pdf <= 0.0 || max(w.r, max(w.g, w.b)) <= 0.0) break;
        thr *= w;
        vec3 ro = o, rd = wi;
        bool delta = false;
        Hit h;
        if (!ptFollow(ro, rd, thr, delta, rng, h)) break;
        uint hf = insts[h.inst].flags;
        if (!h.front && (hf & BF_OPEN) == 0u) break;   // inside a solid
        vec3 q = ro + rd * h.t;
        uint hm = insts[h.inst].material;
        vec3 Le = mats[hm].emission.rgb;
        if (h.front && max(Le.r, max(Le.g, Le.b)) > 0.0) {
            float wMis = 1.0;
            uint first = insts[h.inst].firstEmitter;
            if (nee && !delta && first != PT_NO_EMITTER) {
                float pl = ptEmitterPdf(emitters[first + h.prim], p, q, abs(dot(h.n, rd)));
                wMis = pdf * pdf / (pdf * pdf + pl * pl);
            }
            L += ptClamp(thr * Le * wMis, capNow);
        }
        if (bounce >= 2) {   // Russian roulette
            float keep = min(max(thr.r, max(thr.g, thr.b)), 0.95);
            if (ptRand(rng) >= keep) break;
            thr /= keep;
        }
        p = q;
        n = h.front ? h.n : -h.n;
        V = -rd;
        m = hm;
    }
    return L;
}

// The radiance arriving at o from direction -d: what a camera ray sees, through panes, emission and all.
vec3 ptTrace(vec3 o, vec3 d, inout uint rng) {
    vec3 thr = vec3(1.0);
    bool delta = false;
    Hit h;
    if (!ptFollow(o, d, thr, delta, rng, h)) return vec3(0.0);
    if (!h.front && (insts[h.inst].flags & BF_OPEN) == 0u) return vec3(0.0);
    uint m = insts[h.inst].material;
    vec3 Le = h.front ? mats[m].emission.rgb : vec3(0.0);
    return thr * (Le + ptRadiance(o + d * h.t, h.front ? h.n : -h.n, -d, m, rng));
}
