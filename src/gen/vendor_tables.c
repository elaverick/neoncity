#include "vendor_internal.h"

/* ===========================================================================
 * The vendor grammar's tables: trades, archetype weights, palettes,
 * decorations, signature pieces, props, the tier table and steam (sections
 * 2.2, 7, 8, 9.1 and 10.2 of docs/street-vendor-generation.md).
 * ========================================================================= */

#define N 255
/* The street-food trades (street_food.h, roadmap SF1). The canopy bar builds each trade's line from its plan
 * (noodle_bar.c); the rest of a row here (painted stock, props, hangings, motifs, lamp, heaps) serves the old
 * archetypes until SF2 retires them. archetype affinity: canopy, hatch, lean-to, container, pair, market,
 * barrow; class: poor, middling, wealthy (the user: affluence moves condition, clutter and paint, not the
 * trade's material class, so the classes lean only gently) */
const VTradeInfo V_TRADES[VT_COUNT] = {
    { "noodles", 2, { 6.0f, 1.5f, 1.0f, 1.0f, 1.0f, 0.5f, 1.5f }, { 1.2f, 1.2f, 0.8f }, 1.0f,
      { VK_BOWLS, VK_BOTTLES, VK_JARS, VK_STEAMERS, N }, { VI_POT, VI_BOWLS, VI_WOK, VI_KETTLE, VI_BOTTLES, VI_CASHBOX },
      { VI_LANTERN, VI_BUNCH, N }, { VM_SERPENT, VM_BOWL, VM_SUNBURST }, 0xF2F6FFu, { 0xD8C27Au, 0xE9E1CCu, 0x8E1E1Eu } },
    { "soup and pho", 2, { 5.0f, 1.5f, 1.0f, 1.0f, 1.0f, 0.6f, 1.0f }, { 1.1f, 1.1f, 0.8f }, 1.0f,
      { VK_BOWLS, VK_JARS, VK_BOTTLES, N, N }, { VI_POT, VI_BOWLS, VI_BOTTLES, VI_CASHBOX, N, N },
      { VI_LANTERN, VI_BUNCH, N }, { VM_BOWL, VM_LEAF, N }, 0xF2F6FFu, { 0x6AA83Au, 0xE9E1CCu, 0xC9A03Au } },
    { "dumplings and bao", 2, { 5.0f, 2.0f, 0.8f, 1.0f, 0.6f, 0.6f, 1.0f }, { 1.0f, 1.0f, 0.9f }, 1.0f,
      { VK_STEAMERS, VK_BOWLS, VK_JARS, N, N }, { VI_STEAMER, VI_POT, VI_BOWLS, VI_CASHBOX, N, N },
      { VI_LANTERN, N, N }, { VM_BOWL, VM_LOTUS, N }, 0xFFE0B0u, { 0xEFE6D2u, 0xD8C9A8u, 0xB8A27Au } },
    { "skewers", 2, { 4.0f, 1.0f, 1.5f, 0.6f, 0.4f, 1.0f, 2.0f }, { 1.3f, 0.9f, 0.6f }, 0.6f,
      { VK_BOTTLES, VK_JARS, N, N, N }, { VI_GRILL, VI_BOWLS, VI_BOTTLES, VI_CASHBOX, N, N },
      { VI_LANTERN, VI_BUNTING, N }, { VM_SUNBURST, VM_FISH, N }, 0xFFB060u, { 0x8C4A2Au, 0xC07A3Au, 0x5A3A22u } },
    { "kebab and shawarma", 2, { 4.0f, 2.0f, 0.8f, 1.5f, 0.8f, 0.6f, 0.6f }, { 1.2f, 1.0f, 0.6f }, 1.2f,
      { VK_JOINTS, VK_BOTTLES, VK_JARS, N, N }, { VI_GRILL, VI_BOWLS, VI_BOTTLES, VI_CASHBOX, N, N },
      { VI_BUNTING, N, N }, { VM_SUNBURST, VM_LEAF, N }, 0xF2F6FFu, { 0x9A5A2Au, 0x6AA83Au, 0xE8D8B0u } },
    { "burgers and hot dogs", 2, { 4.0f, 2.0f, 0.6f, 1.5f, 1.0f, 0.6f, 0.8f }, { 1.0f, 1.0f, 0.8f }, 1.2f,
      { VK_BOXES, VK_BOTTLES, N, N, N }, { VI_GRILL, VI_BOTTLES, VI_CASHBOX, VI_CUPS, N, N },
      { VI_BUNTING, N, N }, { VM_SUNBURST, VM_CUP, N }, 0xFFE8C8u, { 0xC88A4Au, 0xD84A3Au, 0xE8C83Au } },
    { "fried food", 2, { 4.0f, 1.5f, 1.0f, 1.0f, 0.6f, 0.8f, 0.8f }, { 1.3f, 1.0f, 0.5f }, 0.8f,
      { VK_BOXES, VK_BOTTLES, VK_JARS, N, N }, { VI_POT, VI_BOWLS, VI_BOTTLES, VI_CASHBOX, N, N },
      { VI_LANTERN, N, N }, { VM_SUNBURST, VM_FISH, N }, 0xFFE0B0u, { 0xE0B06Au, 0xC88A4Au, 0x9A5A2Au } },
    { "curry and rice", 2, { 5.0f, 1.5f, 1.0f, 1.0f, 1.0f, 0.8f, 0.8f }, { 1.1f, 1.1f, 0.8f }, 2.0f,
      { VK_BOXES, VK_BOWLS, VK_JARS, N, N }, { VI_LUNCHBOXES, VI_POT, VI_BOWLS, VI_CASHBOX, N, N },
      { VI_BUNTING, N, N }, { VM_BOWL, VM_SUNBURST, N }, 0xFFE8C8u, { 0xE8D8B0u, 0xD8902Au, 0x8A4A1Au } },
    { "tacos", 2, { 4.0f, 1.5f, 1.0f, 1.0f, 0.8f, 0.8f, 1.0f }, { 1.1f, 1.0f, 0.8f }, 1.2f,
      { VK_BOWLS, VK_BOTTLES, VK_JARS, N, N }, { VI_GRILL, VI_BOWLS, VI_BOTTLES, VI_CASHBOX, N, N },
      { VI_BUNTING, VI_BUNCH, N }, { VM_SUNBURST, VM_LEAF, N }, 0xFFE8C8u, { 0xE8C83Au, 0x6AA83Au, 0xD84A3Au } },
    { "crepes", 2, { 3.0f, 2.0f, 0.6f, 1.5f, 1.0f, 0.8f, 1.0f }, { 0.8f, 1.0f, 1.3f }, 1.0f,
      { VK_JARS, VK_BOXES, VK_CUPS, N, N }, { VI_BOWLS, VI_JARS, VI_CASHBOX, VI_CUPS, N, N },
      { VI_BUNTING, N, N }, { VM_SUNBURST, VM_CUP, N }, 0xFFD8A0u, { 0xE0B06Au, 0x5A3A2Au, 0xE8D8C8u } },
    { "chestnuts and yams", 2, { 3.0f, 1.0f, 0.6f, 0.4f, 0.2f, 1.2f, 2.5f }, { 1.3f, 1.0f, 0.5f }, 0.5f,
      { VK_JARS, VK_BOXES, N, N, N }, { VI_ROASTER, VI_CRATE, VI_CASHBOX, VI_BUCKET, N, N },
      { VI_LANTERN, N, N }, { VM_SUNBURST, N, N }, 0xFFA850u, { 0x6A3A1Au, 0x8A5A2Au, 0x4A2A1Au } },
    { "tea, coffee and drinks", 1, { 3.0f, 3.0f, 0.6f, 2.5f, 1.0f, 0.6f, 1.5f }, { 0.9f, 1.0f, 1.4f }, 2.0f,
      { VK_CUPS, VK_BOTTLES, VK_BOXES, N, N }, { VI_ESPRESSO, VI_CUPS, VI_CASHBOX, VI_KETTLE, N, N },
      { VI_BUNTING, N, N }, { VM_CUP, VM_BOTTLE, N }, 0xFFD9A8u, { 0x5A3A2Au, 0xE8DCC8u, 0x2A2A2Au } },
};
#undef N

/*                                           canopy hatch  leanto box   pair  market barrow */
const float V_ARCH_CLASS[VC_COUNT][VA_COUNT] = {
    { 1.0f, 0.7f, 1.6f, 0.5f, 0.3f, 1.0f, 1.8f },   /* poor: shacks, tarps, barrows */
    { 1.2f, 1.0f, 0.9f, 0.9f, 0.6f, 1.0f, 1.0f },
    { 0.6f, 1.2f, 0.2f, 1.8f, 1.2f, 1.0f, 0.6f },   /* wealthy: containers and kiosks (refs 3-4) */
};
const float V_ARCH_SITE[VSITE_COUNT][VA_COUNT] = {
    { 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.6f, 3.0f },   /* kerb: barrows and kiosks, the walk kept clear */
    { 1.5f, 1.0f, 1.5f, 0.6f, 0.3f, 1.0f, 0.5f },   /* apron in front of a row */
    { 1.0f, 0.6f, 2.0f, 0.3f, 0.2f, 1.5f, 0.6f },   /* market lane */
    { 1.5f, 0.8f, 1.0f, 1.5f, 2.0f, 1.0f, 0.4f },   /* court */
    { 1.0f, 0.8f, 0.6f, 1.0f, 1.0f, 2.0f, 0.5f },   /* pocket market */
    { 0.6f, 1.5f, 0.0f, 1.5f, 0.8f, 1.2f, 1.0f },   /* lunch plaza */
    { 1.5f, 1.0f, 0.4f, 0.3f, 0.0f, 1.0f, 1.0f },   /* arcology */
};
const float V_ARCH_LUNCH[VA_COUNT] = { 0.8f, 1.5f, 0.3f, 1.4f, 1.0f, 1.3f, 1.2f };

/* sRGB colours (0xRRGGBB, as every colour in these tables): body, trim, two accents; the engine's neon colours (shaders/common.glsl: neonColor) */
const VPalette V_PALETTE[V_PALETTES] = {
    { { 0x2F6F6Au, 0xC9B98Fu, 0xC0392Bu, 0xE8C547u }, { 1, 0 }, 3, 0 },   /* street teal */
    { { 0x8E1E1Eu, 0x1C1C1Cu, 0xE8C547u, 0xF2EAD3u }, { 2, 3 }, 3, 0 },   /* noodle red */
    { { 0x4F6B3Au, 0xB8A27Au, 0x7A3B2Eu, 0xD9CBA3u }, { 5, 7 }, 1, 0 },   /* faded green */
    { { 0xD4A017u, 0x222222u, 0x3A6EA5u, 0xEEEEEEu }, { 7, 1 }, 3, 0 },   /* hazard yellow */
    { { 0x2E5C9Au, 0x8A8A80u, 0xE07B39u, 0xDADADAu }, { 1, 6 }, 1, 0 },   /* blue tarp */
    { { 0x9C4A21u, 0x3B3B3Bu, 0x5E8C8Au, 0xCFC6B0u }, { 3, 0 }, 1, 0 },   /* rust orange */
    { { 0x3C2A5Au, 0x111111u, 0xD94F8Cu, 0x7FD1E0u }, { 4, 0 }, 2, 0 },   /* violet night */
    { { 0xE9E1CCu, 0x8E1E1Eu, 0x2B4C3Fu, 0xC9A45Cu }, { 2, 6 }, 6, 0 },   /* cream and red */
    { { 0x2B2B2Bu, 0xA67B4Fu, 0xD9C7A7u, 0x6E7F5Eu }, { 6, 3 }, 4, 0 },   /* charcoal and timber */
    { { 0x7C8C76u, 0xB08D57u, 0xEFE9DDu, 0x2F3B36u }, { 6, 7 }, 4, 0 },   /* sage and brass */
    { { 0x9AA0A3u, 0x3A3F44u, 0xD8B67Au, 0xF1EFEAu }, { 6, 1 }, 6, 0 },   /* gallery grey (ref 3) */
    { { 0x161616u, 0xC2A67Eu, 0x5B4636u, 0xE8E2D6u }, { 3, 6 }, 4, 0 },   /* matte black (ref 4) */
};

/*                                          painted corrug tile  plank board rust */
const float V_FINISH_CLASS[VC_COUNT][VF_COUNT] = {
    { 0.6f, 2.0f, 0.4f, 1.4f, 1.6f, 2.2f },
    { 1.2f, 1.4f, 0.6f, 1.2f, 1.8f, 0.6f },
    { 1.8f, 0.8f, 0.6f, 1.4f, 1.4f, 0.0f },
};

const VDecorInfo V_DECOR[VI_DECOR_COUNT] = {
    { "poster",           VSL_WALL,   0.60f, 0.85f, 0.02f, 0.0f },
    { "stickers",         VSL_WALL,   0.40f, 0.40f, 0.01f, 0.3f },
    { "speaker",          VSL_WALL,   0.25f, 0.35f, 0.20f, 0.0f },
    { "camera",           VSL_WALL,   0.15f, 0.15f, 0.25f, 0.0f },
    { "extractor",        VSL_WALL,   0.40f, 0.40f, 0.25f, 0.0f },
    { "pipe",             VSL_WALL,   0.12f, 1.60f, 0.12f, 0.2f },
    { "AC unit",          VSL_WALL,   0.80f, 0.55f, 0.30f, 0.0f },
    { "crate",            VSL_GROUND, 0.50f, 0.35f, 0.40f, 0.2f },
    { "barrel",           VSL_GROUND, 0.58f, 0.88f, 0.58f, 0.4f },
    { "bag",              VSL_GROUND, 0.50f, 0.35f, 0.35f, 0.4f },
    { "cooler",           VSL_GROUND, 0.60f, 0.45f, 0.40f, 0.0f },
    { "bench",            VSL_GROUND, 1.40f, 0.45f, 0.40f, 0.0f },
    { "bin",              VSL_GROUND, 0.50f, 0.80f, 0.50f, 0.0f },
    { "gas bottle",       VSL_GROUND, 0.30f, 0.90f, 0.30f, 0.2f },
    { "roof string",      VSL_ROOF,   2.00f, 0.30f, 0.10f, 0.0f },
    { "antenna",          VSL_ROOF,   0.30f, 1.20f, 0.30f, 0.2f },
    { "water tank",       VSL_ROOF,   0.80f, 0.80f, 0.80f, 0.4f },
    { "lantern string",   VSL_ROOF,   2.00f, 0.40f, 0.20f, 0.0f },
    { "dish",             VSL_ROOF,   0.60f, 0.60f, 0.40f, 0.3f },
    { "vent fan",         VSL_ROOF,   0.50f, 0.40f, 0.50f, 0.0f },
    { "menu board",       VSL_GROUND, 0.60f, 1.00f, 0.50f, 0.0f },
    { "planter",          VSL_GROUND, 0.50f, 0.60f, 0.50f, 0.0f },
    { "stacked crates",   VSL_GROUND, 0.55f, 0.90f, 0.40f, 0.3f },
    { "rubbish bags",     VSL_GROUND, 0.70f, 0.50f, 0.50f, 0.5f },
    { "folding chair",    VSL_GROUND, 0.45f, 0.85f, 0.45f, 0.0f },
};

/* slot 3: on the counter; slot 4: hung from a rail; slot 255: no geometry of its own */
const VSigInfo V_SIG[VSIG_COUNT] = {
    { "lucky cat",        3,          0.18f, 0.25f, 0.15f },
    { "incense shrine",   VSL_WALL,   0.42f, 0.55f, 0.22f },
    { "fish rack",        4,          0.80f, 0.60f, 0.10f },
    { "cargo bike",       VSL_GROUND, 1.80f, 1.00f, 0.60f },
    { "hanging umbrellas", 4,         1.00f, 0.60f, 0.60f },
    { "mural",            VSL_WALL,   2.00f, 1.50f, 0.01f },
    { "half-dead neon",   255,        0.00f, 0.00f, 0.00f },
    { "wall of screens",  VSL_WALL,   0.90f, 0.70f, 0.30f },
    { "pennants",         VSL_ROOF,   3.00f, 0.40f, 0.05f },
    { "birdcage",         4,          0.35f, 0.45f, 0.35f },
    { "roof planters",    VSL_ROOF,   1.20f, 0.50f, 0.40f },
    { "inflatable mascot", VSL_ROOF,  0.80f, 1.60f, 0.80f },
};

/* emitter: VEM_* + 1 (0 none) */
const VPropInfo V_PROP[VI_PROP_END] = {
    { "pot",          0.35f, 0.30f, 0.35f, VEM_STEAM + 1 },
    { "bowls",        0.25f, 0.20f, 0.25f, 0 },
    { "wok",          0.45f, 0.15f, 0.45f, VEM_STEAM + 1 },
    { "grill",        0.70f, 0.20f, 0.35f, VEM_SMOKE + 1 },
    { "steamers",     0.35f, 0.45f, 0.35f, VEM_STEAM + 1 },
    { "scale",        0.30f, 0.25f, 0.25f, 0 },
    { "crate",        0.50f, 0.30f, 0.35f, 0 },
    { "radio",        0.30f, 0.18f, 0.12f, 0 },
    { "bottles",      0.30f, 0.30f, 0.15f, 0 },
    { "jars",         0.35f, 0.20f, 0.20f, 0 },
    { "cash box",     0.25f, 0.12f, 0.20f, 0 },
    { "fruit box",    0.50f, 0.25f, 0.35f, 0 },
    { "bucket",       0.30f, 0.35f, 0.30f, 0 },
    { "loaves",       0.45f, 0.12f, 0.30f, 0 },
    { "roaster",      0.50f, 0.40f, 0.40f, VEM_SMOKE + 1 },
    { "block",        0.45f, 0.15f, 0.35f, 0 },
    { "book stack",   0.30f, 0.25f, 0.22f, 0 },
    { "kettle",       0.20f, 0.25f, 0.20f, 0 },
    { "espresso",     0.45f, 0.40f, 0.40f, VEM_STEAM + 1 },
    { "cups",         0.20f, 0.25f, 0.20f, 0 },
    { "lunch boxes",  0.40f, 0.20f, 0.30f, 0 },
    { "ceramics",     0.40f, 0.25f, 0.25f, 0 },
    { "tank",         0.60f, 0.40f, 0.35f, 0 },
    { "screen",       0.40f, 0.32f, 0.30f, 0 },
    { "parts bin",    0.45f, 0.20f, 0.30f, 0 },
    { "plant pot",    0.25f, 0.35f, 0.25f, 0 },
    { "ice tray",     0.70f, 0.12f, 0.45f, VEM_MIST + 1 },
};

/*                                    tris  particles hang neon props decor */
const VTierInfo V_TIER[VTIER_COUNT] = {
    {  800,  48,  5, 3,  8, 2 },
    { 1700, 100, 12, 5, 16, 5 },
    { 2600, 180, 20, 7, 26, 8 },
};

/*                                  life  size0  size1 rise  opacity spread turbulence */
const VEmitInfo V_EMIT[VEM_COUNT] = {
    { 2.4f, 0.11f,  0.60f, 0.55f, 0.55f, 0.35f, 0.12f },   /* steam */
    { 3.2f, 0.12f,  0.85f, 0.70f, 0.42f, 0.50f, 0.18f },   /* smoke */
    { 3.6f, 0.20f,  1.10f, 1.20f, 0.50f, 0.60f, 0.20f },   /* vent */
    { 3.2f, 0.035f, 0.20f, 0.45f, 0.38f, 0.12f, 0.05f },   /* incense */
    { 2.8f, 0.15f,  0.50f, 0.12f, 0.26f, 0.60f, 0.08f },   /* mist */
};

void vendor_palette_rgb(int palette, int slot, float rgb[3]) {
    const VPalette *P = &V_PALETTE[(unsigned)palette % V_PALETTES];
    uint32_t c = P->c[slot & 3];
    for (int k = 0; k < 3; ++k) {
        float s = (float)((c >> (16 - 8 * k)) & 255u) / 255.0f;
        rgb[k] = s <= 0.04045f ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f);
    }
}

int v_pick(Rng *r, const float *w, int n) {
    float sum = 0.0f;
    for (int i = 0; i < n; ++i) sum += w[i] > 0.0f ? w[i] : 0.0f;
    float x = rng_float(r) * sum;
    if (sum <= 0.0f) return -1;
    for (int i = 0; i < n; ++i) {
        if (w[i] <= 0.0f) continue;
        if (x < w[i]) return i;
        x -= w[i];
    }
    for (int i = n - 1; i >= 0; --i) if (w[i] > 0.0f) return i;
    return -1;
}

int vendor_class(float a) { return a < 1.0f / 3.0f ? VC_POOR : a < 2.0f / 3.0f ? VC_MID : VC_RICH; }

static const char *const ARCH_NAME[VA_COUNT] = { "canopy noodle bar", "hatch kiosk", "lean-to", "container shop",
                                                 "container pair", "market stall", "hand barrow" };
static const char *const CLASS_NAME[VC_COUNT] = { "poor", "middling", "wealthy" };
static const char *const SITE_NAME[VSITE_COUNT] = { "kerb", "apron", "lane", "court", "pocket market", "lunch plaza", "arcology" };
static const char *const MOTIF_NAME[VM_COUNT] = { "serpent", "bowl", "fish", "cup", "bottle", "eye", "lotus", "sunburst", "gear", "leaf" };
static const char *const TIER_NAME[VTIER_COUNT] = { "lean", "standard", "hero" };
static const char *const HANG_NAME[VI_HANG_END - VI_PROP_END] = { "lantern", "dried bunch", "duck", "cable", "garment", "bunting", "tools", "joint", "umbrella" };

const char *vendor_archetype_name(int a) { return a >= 0 && a < VA_COUNT ? ARCH_NAME[a] : "?"; }
const char *vendor_trade_name(int t) { return t >= 0 && t < VT_COUNT ? V_TRADES[t].name : "?"; }
const char *vendor_class_name(int c) { return c >= 0 && c < VC_COUNT ? CLASS_NAME[c] : "?"; }
const char *vendor_site_name(int k) { return k >= 0 && k < VSITE_COUNT ? SITE_NAME[k] : "?"; }
const char *vendor_motif_name(int m) { return m >= 0 && m < VM_COUNT ? MOTIF_NAME[m] : "?"; }
const char *vendor_tier_name(int t) { return t >= 0 && t < VTIER_COUNT ? TIER_NAME[t] : "?"; }
const char *vendor_signature_name(int s) { return s >= 0 && s < VSIG_COUNT ? V_SIG[s].name : s == VSIG_COUNT ? "none" : "?"; }
const char *vendor_item_name(int k) {
    if (k >= 0 && k < VI_PROP_END) return V_PROP[k].name;
    if (k >= VI_PROP_END && k < VI_HANG_END) return HANG_NAME[k - VI_PROP_END];
    if (k == VI_STOOL) return "stool";
    if (k == VI_GASBOTTLE) return "gas bottle";
    if (k == VI_HEAP) return "heap";
    if (k >= VI_DECOR_FIRST && k < VI_COUNT) return V_DECOR[k - VI_DECOR_FIRST].name;
    return "?";
}
