#include "city_internal.h"
#include "pmodel.h"

/* ===========================================================================
 * The near vendor layer (roadmap NB2 in ~/.claude/plans/street-vendors.md):
 * the modelling kit's modules (src/gen/pmodel.h) each vendor sets down, as
 * instances (Mesh.kit) the renderer draws from its module library
 * (src/render/kit_gpu.c), within KIT_DISTANCE.
 *
 * Until the noodle bar's body and cooking line are built (NB4, NB5), the slice
 * sets down a stand-in arrangement after docs/noodle-bar-target.md: the range
 * (burners, grates, stock pots, knobs), a bain-marie of hotel pans, the steamer
 * and the rice cooker on the back bench's line, shelves of bowls, jars and
 * bottles on brackets, the utensil rail, the extractor fan, the power panel,
 * the gas bottles and their line (saddles, valve, gauge), the bell on the
 * counter's line, the stools and the menu A-frame. The bench, counter and
 * shelves themselves are NB4's: the modules stand where those will be.
 *
 * city_set_kit picks which vendors get it: the slice only (the default), every
 * vendor (KIT_ALL: NB2's cost spike, a lane's worth of slices), or none.
 * ========================================================================= */

static int g_kit = KIT_SLICE;
void city_set_kit(int mode) { g_kit = mode; }
int  city_kit_mode(void) { return g_kit; }

#define HALF_PI 1.5707963f

typedef struct KitEmit { Mesh *m; PmInstances inst; PmMesh none; } KitEmit;

static void put(Pm *pm, float x, float y, float z, int id) {
    pm_push(pm);
    pm_translate(pm, x, y, z);
    pm_place(pm, id);
    pm_pop(pm);
}
/* On the back wall (z = zw), facing out (+z): modules whose surface is z 0 out +z need no turn. */
static void put_turned_y(Pm *pm, float x, float y, float z, float a, int id) {
    pm_push(pm);
    pm_translate(pm, x, y, z);
    pm_rotate_y(pm, a);
    pm_place(pm, id);
    pm_pop(pm);
}

/* The stand-in noodle bar kit in the site's frame: x across its frontage (centred), z out from its back,
 * y up from its floor. Sizes after the target sheet. */
static void slice_kit(Pm *pm, float w, float d) {
    const float hw = w * 0.5f;
    const float zw = 0.10f;                 /* the back wall's lining */
    const float yb = 0.90f, zb = 0.45f;     /* the back bench's top, its middle */
    const float aisle = clampf(d - 0.12f - 0.65f - 0.45f - 0.75f, 0.6f, 0.95f);
    const float zc0 = 0.77f + aisle, zc1 = zc0 + 0.45f, yc = 1.02f;   /* the counter */

    /* the range: two burners on a 0.45 m pitch, their grates, a large pot and a small lidded one; knobs */
    int part = 0;
    for (int i = 0; i < 2; ++i) {
        float x = -hw + 0.45f + 0.45f * (float)i;
        pm->part = (uint8_t)part++;
        put(pm, x, yb, zb, PMK_BURNER);
        put(pm, x, yb, zb, PMK_GRATE);
        put(pm, x, yb + 0.047f, zb, i == 0 ? PMK_STOCKPOT_L : PMK_STOCKPOT_S);
        if (i == 1) put(pm, x, yb + 0.047f + 0.300f, zb, PMK_LID);
        for (int k = 0; k < 2; ++k) {
            pm_push(pm);
            pm_translate(pm, x - 0.06f + 0.12f * (float)k, yb - 0.05f, 0.77f);
            pm_rotate_x(pm, HALF_PI);   /* its face up +y turned out +z */
            pm_place(pm, PMK_KNOB);
            pm_pop(pm);
        }
    }
    /* the bain-marie: three lidded hotel pans and an open one, long sides front to back */
    float xp = -hw + 1.30f;
    for (int i = 0; i < 4; ++i) {
        pm->part = (uint8_t)part++;
        put_turned_y(pm, xp + 0.19f * (float)i, yb, zb, HALF_PI, i == 2 ? PMK_GN_PAN_OPEN : PMK_GN_PAN);
    }
    pm->part = (uint8_t)part++;
    put(pm, xp + 0.19f * 4.0f + 0.12f, yb, zb, PMK_STEAMER);
    put(pm, xp + 0.19f * 4.0f + 0.52f, yb, zb, PMK_RICE_COOKER);

    /* shelves at 1.45 and 1.80 m over the bench right of the range: brackets, bowls and jars, bottles */
    float s0 = -hw + 1.20f, s1 = minf(hw - 0.80f, s0 + 1.6f);
    for (int k = 0; k < 2; ++k) {
        float y = k ? 1.80f : 1.45f;
        put(pm, s0 + 0.05f, y - 0.02f, zw, PMK_SHELF_BRACKET);
        put(pm, s1 - 0.05f, y - 0.02f, zw, PMK_SHELF_BRACKET);
    }
    static const int LOW[] = { PMK_BOWL_STACK, PMK_JAR_OIL, PMK_JAR_GINGER, PMK_JAR_OIL, PMK_BOWL_STACK, PMK_JAR_GINGER, PMK_JAR_OIL };
    for (int i = 0; i < 7; ++i) {
        pm->part = (uint8_t)part++;
        put(pm, s0 + 0.15f + (s1 - s0 - 0.3f) * (float)i / 6.0f, 1.45f, zw + 0.15f, LOW[i]);
    }
    for (int i = 0; i < 4; ++i) {
        pm->part = (uint8_t)part++;
        put(pm, s0 + 0.2f + (s1 - s0 - 0.4f) * (float)i / 3.0f, 1.80f, zw + 0.14f, PMK_BOTTLE);
    }

    /* the utensil rail over the range at 1.55 m, and what hangs from it */
    float xr = -hw + 0.68f, yr = 1.55f, zr = zw + 0.045f;
    pm->part = (uint8_t)part++;
    put(pm, xr, yr, zw, PMK_UTENSIL_RAIL);
    put(pm, xr - 0.38f, yr + 0.02f, zr, PMK_LADLE);
    put(pm, xr - 0.20f, yr + 0.02f, zr, PMK_SPIDER);
    put(pm, xr - 0.02f, yr + 0.02f, zr, PMK_SPATULA);
    for (int i = 0; i < 4; ++i) put(pm, xr + 0.12f + 0.09f * (float)i, yr + 0.02f, zr, PMK_S_HOOK);
    put(pm, xr + 0.12f, yr + 0.02f - 0.063f, zr, PMK_CLEAVER);

    /* the extractor fan over the range, the power panel and the gas */
    pm->part = (uint8_t)part++;
    put(pm, -hw + 0.68f, 2.15f, zw, PMK_FAN);
    put(pm, hw - 0.40f, 1.25f, zw, PMK_POWER_PANEL);
    float xg = hw - 0.32f;
    put(pm, xg, 0.0f, 0.32f, PMK_GAS_47);
    put(pm, xg + 0.05f, 0.0f, 0.75f, PMK_GAS_13);
    /* the gas line along the wall at 1.0 m: saddles (their pipe along x), a valve and its gauge */
    for (int i = 0; i < 6; ++i) {
        pm_push(pm);
        pm_translate(pm, -hw + 0.3f + (xg - 0.5f + hw - 0.3f) * (float)i / 5.0f, 1.0f, zw);
        pm_rotate_x(pm, HALF_PI);   /* its surface (y 0) on the wall, out +z */
        pm_rotate_y(pm, HALF_PI);   /* its pipe along x */
        pm_place(pm, PMK_SADDLE);
        pm_pop(pm);
    }
    put(pm, xg - 0.35f, 1.0f, zw + 0.03f, PMK_VALVE_LEVER);
    put(pm, xg - 0.20f, 1.0f, zw + 0.03f, PMK_GAUGE);

    /* the counter's line: the bell; the stools out in front; the A-frame by the front corner */
    pm->part = (uint8_t)part++;
    put(pm, hw - 0.55f, yc, zc0 + 0.22f, PMK_BELL);
    int ns = (int)((w - 0.3f) / 0.62f);
    if (ns < 1) ns = 1;
    if (ns > 6) ns = 6;
    for (int i = 0; i < ns; ++i) {
        pm->part = (uint8_t)part++;
        put(pm, ((float)i - 0.5f * (float)(ns - 1)) * 0.62f, 0.0f, zc1 + 0.30f + 0.20f, PMK_STOOL);
    }
    pm->part = (uint8_t)part++;
    put(pm, -hw + 0.40f, 0.0f, maxf(d - 0.25f, zc1 + 0.8f), PMK_A_FRAME);
}

static void kit_vendor(void *ctx, const VendorSpot *s, const Vendor *v) {
    KitEmit *E = (KitEmit *)ctx;
    if (g_kit == KIT_OFF || (g_kit == KIT_SLICE && !s->site.slice)) return;
    Mesh *m = E->m;
    E->inst.n = 0;
    Pm pm;
    pm_begin(&pm, &E->none, &E->inst, PM_LOD0);
    slice_kit(&pm, s->site.w, s->site.d);
    /* the site's frame to chunk-local: x across (fz, -fx), z out (fx, fz), y up from its floor */
    const float R[3][3] = { { s->fz, 0.0f, s->fx }, { 0.0f, 1.0f, 0.0f }, { -s->fx, 0.0f, s->fz } };
    const float t[3] = { (float)(s->x - m->ox), s->y, (float)(s->z - m->oz) };
    /* the flags: the vendor's wear, and whether a module stands at the range (the stand-in kit's: the bench's first
     * 1.25 m, back to front as far as the aisle; NB5's cooking line will say where its heat is) */
    const uint8_t wear = (uint8_t)(clampf(v->wear, 0.0f, 1.0f) * 15.0f + 0.5f);
    const float hw = s->site.w * 0.5f;
    if (m->nkit + E->inst.n > m->capkit) {
        m->capkit = (m->nkit + E->inst.n) * 2;
        m->kit = (KitInst *)realloc(m->kit, sizeof(KitInst) * m->capkit);
        if (!m->kit) fatal("out of memory setting down the vendors' kit");
    }
    for (uint32_t i = 0; i < E->inst.n; ++i) {
        const PmInstance *pi = &E->inst.v[i];
        KitInst *k = &m->kit[m->nkit++];
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c)
                k->xf[r * 4 + c] = R[r][0] * pi->xf[c] + R[r][1] * pi->xf[4 + c] + R[r][2] * pi->xf[8 + c];
            k->xf[r * 4 + 3] += t[r];
        }
        k->module = pi->module;
        k->part = pi->part;
        const float lx = pi->xf[3], ly = pi->xf[7], lz = pi->xf[11];
        const int heat = lx > -hw - 0.05f && lx < -hw + 1.25f && lz < 0.85f && ly > 0.5f && ly < 1.9f;
        k->flags = (uint8_t)(wear | (heat ? KIT_FLAG_HEAT : 0u));
        k->base = s->y;
        k->seed = hash_combine(s->site.seed, i + 0x4B49u);
        for (int a = 0; a < 3; ++a) {
            float p = k->xf[a * 4 + 3];
            m->mn[a] = minf(m->mn[a], p - KIT_PAD);
            m->mx[a] = maxf(m->mx[a], p + KIT_PAD);
        }
    }
}

void city_build_vendor_kit(int cx, int cz, Mesh *m) {
    mesh_begin(m, cx, cz);
    m->nkit = 0;
    if (g_kit == KIT_OFF) return;
    KitEmit E;
    memset(&E, 0, sizeof(E));
    E.m = m;
    city_chunk_vendors(cx, cz, kit_vendor, &E);
    pm_inst_free(&E.inst);
    pm_mesh_free(&E.none);
}
