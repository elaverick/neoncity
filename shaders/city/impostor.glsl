/* Baked interior impostors (docs/interior-impostors.md): the view grid, the
 * 3-view pick and the march through a view's depth layers. The baker's lab
 * reconstructs novel views with it (shaders/interior/recon.comp), and from
 * IM2 the facade shader does.
 *
 * Module frame: x along the wall (the facade's u), y up, z into the building,
 * the glass at z = 0. A direction d runs from the eye into the building
 * (d.z > 0). View k is an oblique orthographic projection onto the glass
 * plane along its direction d_k: texel (s, t) is the ray that crosses z = 0
 * at the plane point origin + (s + 0.5, t + 0.5) / tpm and travels along d_k. A point at
 * depth z on a ray entering the glass at Q with slope sigma = d.xy / d.z lies
 * on view k's texel Q + z * (sigma - d_k.xy / d_k.z), so marching the ray is a
 * 2D march in the plane.
 *
 * Each texel stores K intervals along its ray, nearest first: the depth where
 * the ray enters a surface's object and where it leaves it, as fractions of
 * the module's depth D (entry 1: no more layers). A texel whose ray met more
 * surfaces than it stores is "full": past what it holds, a march has to guess.
 *
 * The includer defines, before including this file:
 *   vec2 impFetchDepth(int view, int layer, ivec2 texel)   entry, exit
 *   bool impFetchFull(int view, ivec2 texel)
 *   vec3 impFetchColor(int view, int layer, ivec2 texel)
 * or IMP_VIEWS_ONLY, for the view grid alone.
 */

#define IMP_GRID_AZEL    0   // azimuth and elevation, each linear over its range
#define IMP_GRID_HEMIOCT 1   // the hemi-octahedral square, inset from the grazing edge
#define IMP_GRID_SLOPE   2   // azimuth linear; elevation by its slope s = tan(el), uniform in asinh(s / s0)

/* IMP_GRID_SLOPE: a view at slope s, used for a ray at slope sigma, looks up
 * texels |s - sigma| * z off the ray's own at depth z, and a climbing ray
 * meets the ceiling within (height / sigma): so the views are spaced in
 * proportion to their slope (geometrically) where it is steep, and evenly
 * near the horizontal, where rays see to the far glass. s0 sets the change
 * over (about the room's height over its depth). */
struct ImpViews {
    ivec2 n;       // grid nodes across (azimuth) and up (elevation)
    int   kind;    // IMP_GRID_*
    vec4  range;   // AZEL: the largest azimuth, the lowest and highest elevation (radians); HEMIOCT: w, the inset;
                   // SLOPE: the largest azimuth, the lowest and highest asinh(s / s0), s0
};

struct ImpGrid {
    vec2  origin;    // the plane point at texel (0, 0)'s corner, metres
    float tpm;       // texels per metre
    ivec2 dim;       // texels
    bool  wrapX;     // a periodic module repeats in x
    float D;         // the module's depth, metres
    int   K;         // layers stored per texel
    bool  lastSlot;  // layer K - 1 holds the ray's last surface rather than its K-th
};

/* The direction at grid position uv (0..1 each way). */
vec3 impViewDir(ImpViews v, vec2 uv) {
    if (v.kind != IMP_GRID_HEMIOCT) {
        float az = mix(-v.range.x, v.range.x, uv.x), el = mix(v.range.y, v.range.z, uv.y);
        if (v.kind == IMP_GRID_SLOPE) el = atan(v.range.w * sinh(el));
        return vec3(sin(az) * cos(el), sin(el), cos(az) * cos(el));
    }
    vec2 q = mix(vec2(-v.range.w), vec2(v.range.w), uv);
    vec2 p = vec2(q.x - q.y, q.x + q.y) * 0.5;
    return normalize(vec3(p, max(1.0 - abs(p.x) - abs(p.y), 1e-3)));
}

vec3 impViewDirIndex(ImpViews v, int view) {
    ivec2 ij = ivec2(view % v.n.x, view / v.n.x);
    return impViewDir(v, vec2(ij) / vec2(v.n - 1));
}

/* The grid position of direction d, clamped to the grid. */
vec2 impDirUV(ImpViews v, vec3 d) {
    vec2 uv;
    if (v.kind != IMP_GRID_HEMIOCT) {
        float az = atan(d.x, d.z), el = v.kind == IMP_GRID_SLOPE ? asinh(d.y / (length(d.xz) * v.range.w)) : asin(clamp(d.y, -1.0, 1.0));
        uv = vec2((az + v.range.x) / (2.0 * v.range.x), (el - v.range.y) / (v.range.z - v.range.y));
    } else {
        vec2 p = d.xy / (abs(d.x) + abs(d.y) + d.z);
        uv = vec2(p.x + p.y, p.y - p.x) / v.range.w * 0.5 + 0.5;
    }
    return clamp(uv, 0.0, 1.0);
}

/* The 3 views round d and their barycentric weights: the grid cell holding
 * d, split along its diagonal. */
void impPickViews(ImpViews v, vec3 d, out ivec3 views, out vec3 w) {
    vec2 g = impDirUV(v, d) * vec2(v.n - 1);
    ivec2 c = min(ivec2(g), v.n - 2);
    vec2 f = g - vec2(c);
    ivec2 a, b, e;
    if (f.x + f.y < 1.0) { a = c;               b = c + ivec2(1, 0); e = c + ivec2(0, 1); w = vec3(1.0 - f.x - f.y, f.x, f.y); }
    else                 { a = c + ivec2(1, 1); b = c + ivec2(1, 0); e = c + ivec2(0, 1); w = vec3(f.x + f.y - 1.0, 1.0 - f.y, 1.0 - f.x); }
    views = ivec3(a.y * v.n.x + a.x, b.y * v.n.x + b.x, e.y * v.n.x + e.x);
}

#ifndef IMP_VIEWS_ONLY
/* The texel holding plane point p. outside: set where p lies off the grid
 * (the texel is then the nearest edge one). */
ivec2 impTexel(ImpGrid g, vec2 p, inout bool outside) {
    ivec2 i = ivec2(floor((p - g.origin) * g.tpm));
    if (g.wrapX) i.x -= g.dim.x * int(floor(float(i.x) / float(g.dim.x)));
    else if (i.x < 0 || i.x >= g.dim.x) outside = true;
    if (i.y < 0 || i.y >= g.dim.y) outside = true;
    return clamp(i, ivec2(0), g.dim - 1);
}

/* The first layer at texel tx whose interval overlaps depths [a, b]
 * (fractions of D), or -1. unknown: set where [a, b] reaches into depths a
 * full texel doesn't describe. The layers lie nearest first, so a layer
 * beyond b ends the search, and the full flag is read only where it
 * matters: most steps cost one fetch. */
int impOverlap(int view, ivec2 tx, ImpGrid g, float a, float b, inout bool unknown) {
    float prevExit = 0.0;
    for (int l = 0; l < g.K; ++l) {
        vec2 ex = impFetchDepth(view, l, tx);
        if (ex.x >= 1.0) return -1;   // no more layers: empty to the module's end
        if (g.lastSlot && l == g.K - 1 && b > prevExit && a < ex.x && impFetchFull(view, tx)) unknown = true;
        if (ex.x > b) return -1;      // [a, b] lies before this layer, in known empty space
        if (ex.y >= a) return l;
        prevExit = ex.y;
    }
    if (!g.lastSlot && b > prevExit && impFetchFull(view, tx)) unknown = true;
    return -1;
}

struct ImpHit {
    int   layer;     // -1: the ray left the module without a hit
    ivec2 texel;
    float z;         // the hit's depth, metres
    bool  unknown;   // the march crossed depths the view doesn't describe
    bool  outside;   // the march looked up texels off the grid
    int   steps;
};

/* Marches the ray entering the glass at Q, drifting by drift (its slope less
 * the view's) per metre of depth, through view `view` up to depth zExit.
 * Steps are spaced so the texel moves at most maxTexStep a step (at most
 * maxSteps of them); each tests the depths it spans against the intervals of
 * the texel at its end (and with bothEnds, at its start too: a thin part
 * between the two is then caught by either), and the first that overlaps is
 * refined by bisection. */
ImpHit impMarch(int view, ImpGrid g, vec2 Q, vec2 drift, float zExit, int maxSteps, float maxTexStep, int refine, bool bothEnds) {
    ImpHit h;
    h.layer = -1; h.texel = ivec2(0); h.z = zExit; h.unknown = false; h.outside = false; h.steps = 0;
    float span = length(drift) * g.tpm * zExit;
    int n = clamp(int(ceil(span / maxTexStep)), 1, maxSteps);
    float invD = 1.0 / g.D, z0 = 0.0;
    ivec2 prev = impTexel(g, Q, h.outside);
    for (int j = 1; j <= n; ++j) {
        float z1 = zExit * float(j) / float(n);
        ivec2 tx = impTexel(g, Q + drift * z1, h.outside);
        h.steps++;
        int l = impOverlap(view, tx, g, z0 * invD, z1 * invD, h.unknown);
        if (l < 0 && bothEnds && prev != tx) {
            l = impOverlap(view, prev, g, z0 * invD, z1 * invD, h.unknown);
            if (l >= 0) tx = prev;
        }
        prev = tx;
        if (l < 0) { z0 = z1; continue; }
        /* Refine. Where the step's end lies inside the solid, bisect on that (the ray's point inside
         * its texel's interval), which finds the surface; else the solid is thinner than the step,
         * and only the overlap test sees it. */
        vec2 ex1 = impFetchDepth(view, l, tx);
        bool point = ex1.x <= z1 * invD && ex1.y >= z1 * invD;
        float lo = z0, hi = z1;
        for (int r = 0; r < refine; ++r) {
            float m = 0.5 * (lo + hi);
            bool o = false, u = false;
            ivec2 tm = impTexel(g, Q + drift * m, o);
            int lm = impOverlap(view, tm, g, (point ? m : lo) * invD, m * invD, u);
            if (lm >= 0) { hi = m; l = lm; tx = tm; } else lo = m;
        }
        h.layer = l; h.texel = tx; h.z = hi;
        if (impFetchDepth(view, l, tx).x <= 0.0) h.unknown = true;   // its ray began in the solid: it never saw this face
        return h;
    }
    return h;
}

/* The colour at a march's hit. Nearest: the hit texel's layer. Filtered: the
 * 2 x 2 texels round the hit's point on the plane, bilinearly, each with its
 * layer at the hit's depth (within tol metres) and none that lies elsewhere,
 * so edges blend without bleeding across depths. */
vec3 impHitColor(int view, ImpGrid g, ImpHit h, vec2 Q, vec2 drift, bool filtered, float tol) {
    vec3 nearest = impFetchColor(view, h.layer, h.texel);
    if (!filtered) return nearest;
    vec2 f = (Q + drift * h.z - g.origin) * g.tpm - 0.5;
    vec2 i0 = floor(f), fr = f - i0;
    float a = (h.z - tol) / g.D, b = (h.z + tol) / g.D;
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    for (int k = 0; k < 4; ++k) {
        ivec2 o = ivec2(k & 1, k >> 1);
        bool off = false;
        ivec2 tx = impTexel(g, (i0 + vec2(o) + 0.5) / g.tpm + g.origin, off);
        float w = (o.x == 1 ? fr.x : 1.0 - fr.x) * (o.y == 1 ? fr.y : 1.0 - fr.y);
        for (int l = 0; l < g.K; ++l) {
            vec2 ex = impFetchDepth(view, l, tx);
            if (ex.x >= 1.0 || ex.x > b) break;
            if (ex.y >= a) { sum += impFetchColor(view, l, tx) * w; wsum += w; break; }
        }
    }
    return wsum > 1e-3 ? sum / wsum : nearest;
}
#endif
