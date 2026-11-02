// Part of shaders/city.frag, which includes the parts in order: baked interior impostors behind office glass.

/* ======================================================================
 * Baked interior impostors (docs/interior-impostors.md)
 *
 * A share of the office floors behind full-height glass show a module of
 * open-plan floor baked offline (tools/interior_bake) in place of a box room
 * per window: its views, each a stack of depth layers, marched along the view
 * ray (city/impostor.glsl). The modules are loaded by src/render/interiors.c
 * from assets/interiors/; with none loaded, or the share at 0 (--impostors,
 * F8), every floor keeps interior mapping (rooms.glsl).
 * ==================================================================== */

#include "city/impostor_pick.glsl"

// Each module's channels, module * INTERIOR_CHANNELS + channel; the module is chosen per floor.
layout(set = 0, binding = 12) uniform sampler2DArray impTex[INTERIOR_TABLE_SIZE * INTERIOR_CHANNELS];

int gImpTex;   // the marched module's first image
int gImpK;     // its layers per view
float gImpScale, gImpExtMax;   // its bake scale (and this floor's lights: gImpLit); what ext's 1 means
float gImpLit;                 // how far this floor's own lights are on (impostorLight)
vec3 gImpC[4];                 // the radiance of each basis at this window (impostorLight): street, neon, sky, screens
float gImpOut;                 // the share of this pixel that left through a real window (impostorTrace)
vec3 gImpHit;                  // where the march being shaded hit, in the module's metres
uint gImpFloor;                // this floor's hash (impostorLight)
float gImpShownE;              // how much of the fetched colour is a screen's own face, under gImpC[3]'s unit

/* What a screen is showing now, as a tint on its baked image (doc, section 23). There is no per-screen
 * data in a bake, so "which screen" is a smooth field over the floor's plan, about a desk across. Its
 * values set each screen's programme: a share are off, and they come and go; code scrolls in jumps up
 * most; a few cycle through colours, a screensaver. */
vec3 impScreenShow() {
    float id = vnoise(gImpHit.xz * 1.4, gImpFloor & 0xFFFFu), k = fract(id * 17.0), t = gTime;
    vec3 c = gImpC[3];
    // a dark floor has fewer left on (gImpLit), and any of them goes off now and then
    float on = smoothstep(gImpLit > 0.0 ? 0.08 : 0.68, gImpLit > 0.0 ? 0.14 : 0.74, fract(id * 5.0 + t * 0.006));
    float show;
    if (k < 0.18) {
        c = max(dot(c, vec3(0.2126, 0.7152, 0.0722)), 0.1) * (0.65 + 0.55 * cos(t * 0.35 + id * 30.0 + vec3(0.0, 2.1, 4.2)));
        show = 1.1;
    } else {
        // lines of text, one screenful climbing in jumps, as a terminal does
        float rate = 2.0 + 7.0 * k, scroll = floor(t * rate) / rate * 0.09;
        show = 0.6 + 0.75 * smoothstep(0.25, 0.45, fract((gImpHit.y + scroll) / 0.09 + id * 9.0));
    }
    return c * show * on * (1.0 + 0.04 * sin(t * 47.0 + id * 90.0));
}

#define IMP_SCREEN_RATIO 1.2  // screens-basis radiance over albedo where a surface is certainly a screen; what it lights stays well under 1

vec2 impFetchDepth(int view, int layer, ivec2 t) { return texelFetch(impTex[nonuniformEXT(gImpTex + 2)], ivec3(t, view * gImpK + layer), 0).rg; }
bool impFetchFull(int view, ivec2 t) { return texelFetch(impTex[nonuniformEXT(gImpTex + 3)], ivec3(t, view), 0).r > 0.5; }
/* A hit's radiance, relit for this window (doc, section 6): the module's own light, plus each basis's
 * luminance in the colour the engine gives it, tinted by the surface where that light came diffusely. */
vec3 impFetchColor(int view, int layer, ivec2 t) {
    ivec3 at = ivec3(t, view * gImpK + layer);
    vec3 lint = texelFetch(impTex[nonuniformEXT(gImpTex)], at, 0).rgb;
    vec4 alb = texelFetch(impTex[nonuniformEXT(gImpTex + 1)], at, 0);
    vec4 e = texelFetch(impTex[nonuniformEXT(gImpTex + 4)], at, 0);
    e *= e * gImpExtMax;
    float albL = dot(alb.rgb, vec3(0.2126, 0.7152, 0.0722));
    vec3 tint = mix(alb.rgb / max(albL, 0.01), vec3(1.0), alb.a);
    /* A screen is the one surface far brighter under the screens basis than its albedo could reflect:
     * it's nearly black and emits. What it sheds stays with the floor's gImpC[3]. Its programme is worked out
     * once a view, by impostorTrace: in here it would run for every fetch. */
    gImpShownE = max(gImpShownE, smoothstep(IMP_SCREEN_RATIO * 0.5, IMP_SCREEN_RATIO, e.a / max(albL, 0.005)) * e.a);
    return lint * gImpScale + tint * (gImpC[0] * e.r + gImpC[1] * e.g + gImpC[2] * e.b + gImpC[3] * e.a);
}

#include "city/impostor.glsl"

/* What a span of bays of a floor shows. */
struct ImpPick {
    int   m;        // the module, or -1: this floor keeps interior mapping
    bool  mirror;   // flipped in x
    vec3  scale;    // the face's metres to the module's (x along the wall, y up, z in)
    float x0;       // where along the module the span starts, at u0, m
    float u0;       // where along the face it starts, m
};

/* How a floor shows its module (impostorModule) behind this face. It repeats along the floor (modules
 * are periodic in x), but every span of about a module's width gets its own start and may flip, so the
 * tile's period doesn't show; the seam falls on a mullion. It stretches so its width is whole bays, if
 * that takes under 10%, so its columns meet the mullions; else not at all. z stretches so a through
 * module's far glass is the far face. */
ImpPick impostorPick(uint seed, int section, int floorIdx, int faceDir, int bay, float bayW, float floorH, uint plate) {
    ImpPick p;
    p.mirror = false; p.scale = vec3(1.0); p.x0 = 0.0; p.u0 = 0.0;
    p.m = impostorModule(seed, section, floorIdx, plate);
    if (p.m < 0) return p;

    ImpModule M = interiors.impMod[p.m];
    float W = M.size.x, n = max(1.0, floor(W / bayW + 0.5)), sx = W / (n * bayW);
    p.scale.x = abs(sx - 1.0) <= 0.1 ? sx : 1.0;
    p.scale.y = M.size.y / floorH;
    if (M.dim.w == 0) p.scale.z = M.size.z / (float(plate) * 0.5);
    int span = bay / int(n);
    p.u0 = float(span * int(n)) * bayW;
    uint hs = hash3i(seed ^ 0x1A7Fu, section, floorIdx, faceDir * 1024 + span);
    p.mirror = hfloat(hs) < 0.5;
    p.x0 = floor(hf(hs, 1u) * float(M.bays.x)) * M.bays.y;   // on one of its bays
    return p;
}

/* A place along the face as a place along the module. */
float impostorX(ImpPick p, float u) {
    float x = p.x0 + (u - p.u0) * p.scale.x;
    return p.mirror ? -x : x;
}

/* What lights a module at this window (doc, section 16), as the radiance of the source each basis
 * stands for. lamps: the city's lamps and shops on this facade (wallLighting, less its ambient and
 * neon). A source filling the half of a wall's view below it puts half its radiance on the wall, so
 * the street's is twice that. The neon and the towers across carry on above where the lamps give
 * out. glassT: the near glass the light comes through. floorHash: this floor's own screens. */
void impostorLight(vec3 lamps, vec3 glassT, float height, uint floorHash, float litChance) {
    // the floor's own lights: on as often as a room's in this building are (makeRoom), which is also what the
    // far term averages, and no two floors quite alike. Most of a dark floor's screens are off too.
    gImpFloor = floorHash;
    gImpLit = hf(floorHash, 33u) < litChance ? mix(0.75, 1.25, hf(floorHash, 34u)) : 0.0;
    vec3 across = vec3(0.11, 0.085, 0.06) * 2.5;     // the towers across: paneSheen's lit facades
    gImpC[0] = (lamps * 2.0 + across * mix(0.25, 1.0, exp(-height / 80.0))) * glassT;
    // a floor sees signs from afar, even where none stand just below it
    gImpC[1] = neonHue(gWorld) * mix(0.4, 1.0, neonDensity(gWorld)) * mix(0.35, 1.0, exp(-height / 40.0)) * 5.0 * glassT;
    gImpC[2] = mix(CLOUD_DECK, vec3(0.03, 0.035, 0.045), 0.5) * 1.4 * glassT;
    // mostly cool, some warmer or green, breathing as what they show changes
    float k = hf(floorHash, 30u);
    vec3 screen = k < 0.65 ? vec3(0.30, 0.50, 0.85) : k < 0.85 ? vec3(0.62, 0.70, 0.80) : vec3(0.32, 0.72, 0.55);
    // and what they show changes every few seconds: the light they shed steps, over a quarter of a second
    float beat = gTime * (0.15 + 0.2 * hf(floorHash, 35u)) + hf(floorHash, 36u), scene = floor(beat);
    float was = hfloat(floorHash ^ (uint(scene) * 0x9E3779B1u)), now = hfloat(floorHash ^ (uint(scene + 1.0) * 0x9E3779B1u));
    float shows = mix(0.6, 1.2, mix(was, now, smoothstep(0.0, 0.08, fract(beat))));
    gImpC[3] = screen * shows * (0.75 + 0.25 * sin(gTime * (0.2 + 0.2 * hf(floorHash, 31u)) + 6.28 * hf(floorHash, 32u))) * (gImpLit > 0.0 ? 1.0 : 0.35);
}

/* The mean radiance through the glass of the module's bay at x (the mid LOD), under gImpC. */
vec3 impostorBayMean(int m, float x) {
    ImpModule M = interiors.impMod[m];
    float W = M.size.x;
    int b = clamp(int((x - W * floor(x / W)) / M.bays.y), 0, int(M.bays.x) - 1);
    vec4 e = M.bayExt[b];
    return M.bayMean[b].rgb * M.bays.z * gImpLit + gImpC[0] * e.r + gImpC[1] * e.g + gImpC[2] * e.b + gImpC[3] * e.a;
}

/* What the eye sees through the glass of a pick, along d (the face's frame: x along the wall, y up, z
 * into the building), entering at (u, y) on the glass (metres). The 3 views round d are each marched
 * and blended by weight, less for a view that guessed or looked off its grid. zSide: how deep (the
 * face's metres) the ray reaches the side of the building; a ray that gets that far, or to the far
 * glass of a through module, leaves: through a real window (realWindow) gImpOut says the share of the
 * pixel that does, and the rest is what's returned; else it sees the view out (throughView, from
 * worldDir and the window's height eyeY). Below the floor or above the ceiling the glass covers the
 * slab's edge, slabC. Near the eye (eyeDist) a hit's colour is filtered over its 2 x 2 texels, so a
 * texel doesn't show as a block; further off it isn't worth the fetches. */
vec3 impostorTrace(ImpPick p, float u, float y, vec3 d, vec3 worldDir, float eyeY, float eyeDist, float zSide, vec3 slabC, bool realWindow) {
    int m = p.m;
    gImpOut = 0.0;
    ImpModule M = interiors.impMod[m];
    /* The near tier, where it's resident: the same floor at twice the texels. Whole within info.w; from
     * there it dithers away to the base tier, and likewise as it arrives, so neither pops. */
    if (M.tier.x >= 0.0) {
        float whole = interiors.info.w, k = (1.0 - smoothstep(whole, whole * 1.5, eyeDist)) * M.tier.y;
        if (k > 0.0 && fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) < k) {
            m = int(M.tier.x);
            M = interiors.impMod[m];
        }
    }
    vec2 q = vec2(impostorX(p, u), y * p.scale.y);
    if (q.y < 0.0 || q.y > M.size.w) return slabC;
    d *= p.scale;
    if (p.mirror) d.x = -d.x;
    gImpTex = m * INTERIOR_CHANNELS;
    gImpK = M.dim.z;
    gImpScale = M.bays.z * gImpLit;
    gImpExtMax = M.bayMean[0].a;
    ImpViews v;
    v.n = M.views.xy; v.kind = M.views.z; v.range = M.range;
    ImpGrid g;
    g.origin = M.grid.xy; g.tpm = M.grid.z; g.dim = M.dim.xy; g.wrapX = M.grid.w > 0.5;
    g.D = M.size.z; g.K = M.dim.z; g.lastSlot = M.views.w != 0;
    d.z = max(d.z, 1e-3);
    vec2 sigma = d.xy / d.z;
    float near = interiors.info.z, filt = near > 0.0 ? 1.0 - smoothstep(near * 0.75, near, eyeDist) : 0.0;
    float zExit = min(g.D, zSide * p.scale.z);

    ivec3 views;
    vec3 w;
    impPickViews(v, d, views, w);
    // a view with little say isn't worth its march: the weights narrow towards the nearest, smoothly
    w = max(w - 0.12, vec3(0.0));
    w /= w.x + w.y + w.z;
    vec3 sum = vec3(0.0);
    float wsum = 0.0, wOut = 0.0;
    for (int i = 0; i < 3; ++i) {
        if (w[i] <= 1e-4) continue;
        vec3 dk = impViewDirIndex(v, views[i]);
        ImpHit h = impMarch(views[i], g, q, sigma - dk.xy / dk.z, zExit, int(M.march.x), M.march.y, int(M.march.z), false);
        float wt = w[i] * (h.unknown ? M.march.w : 1.0) * (h.outside ? M.bays.w : 1.0);
        if (h.layer >= 0) {
            gImpShownE = 0.0;
            vec3 c = impFetchColor(views[i], h.layer, h.texel);
            if (filt > 0.0) c = mix(c, impHitColor(views[i], g, h, q, sigma - dk.xy / dk.z, true, 0.15), filt);
            [[dont_flatten]] if (gImpShownE > 0.0) {
                gImpHit = vec3(q + sigma * h.z, h.z);
                c = max(c + (impScreenShow() - gImpC[3]) * gImpShownE, vec3(0.0));
            }
            sum += c * wt;
        }
        else wOut += wt;
        wsum += wt;
    }
    // past the last layer: the far glass of a through module or the building's side, else the core wall's dark.
    // With real windows the city behind shows there, by the samples the glass leaves uncovered (windowCoverage);
    // they fade to the modelled view out as they reach their limit.
    gImpOut = 0.0;
    if (wOut > 0.0) {
        bool leaves = M.dim.w == 0 || zExit < g.D;
        float real = leaves && realWindow ? 1.0 - smoothstep(interiors.windows.y, interiors.windows.x, eyeDist) : 0.0;
        gImpOut = real * wOut / max(wsum, 1e-6);
        sum += (leaves ? throughView(worldDir, eyeY) : vec3(0.004)) * wOut * (1.0 - real);
    }
    return sum / max(wsum, 1e-6);
}

/* A real window's pixel: the glass covers the share of its samples that isn't seen through, and always
 * sample 0 and paneCoverage's share at least, so depth and reflections see a pane. through: the share of
 * the pixel looking out. A pixel between two shares dithers, where the views part or a window nears its
 * limit. Returns the covered share, to divide the pixel's own light by. */
float windowCoverage(float through, float F) {
    float dither = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    int k = clamp(int(floor((1.0 - through) * 4.0 + dither)), F < 0.25 ? 1 : F < 0.5 ? 2 : 3, 4);
    gl_SampleMask[0] = (1 << k) - 1;
    return float(k) * 0.25;
}
