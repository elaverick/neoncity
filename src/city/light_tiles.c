#include "city_internal.h"

/* ===========================================================================
 * Light tiles
 *
 * Street lamps and lit shopfronts near the chunk are packed into a small
 * table the fragment shader walks instead of evaluating every light: the
 * chunk's mesh bounds are cut into 4x4 tiles, each listing the lights that
 * can reach it. Record layout (4 x uint32 each):
 *   0      bounds x0, z0 (chunk-local) and 4/width, 4/depth (floats)
 *   1..4   16 tile words: first record << 16 | count
 *   5..    lights: x, y, z (floats), w = type | data
 *          type 0/1: sodium/LED street lamp, w >> 2 its fault (lamp_fault, 24
 *          bits), w >> 26 its emitter (LE_*: how large a source it is, for
 *          soft shadows); type 2: shop (bits 4..6: the shop hash's colour);
 *          type 3: office or estate lobby (white). Shops and lobbies stand 1 m
 *          out from their glass: bits 16..23 are the way it faces (256ths of a
 *          turn from +x towards +z) and bits 24..29 the bay's width (quarter
 *          metres), so shadows can be cast from the whole pane.
 * ========================================================================= */

enum { LE_LAMP = 0, LE_ARC_SODIUM = 1, LE_ARC_CROWN = 2 };   /* must match emitterPoint in shaders/common.glsl */

typedef struct LightSrc { float x, y, z, range; uint32_t w; } LightSrc;

/* A lamp or shopfront lighting the rain mist, facing (nx, nz) if it has a
 * front. Several chunks see the same light; only the chunk it stands in
 * records it. */
static void air_light(Mesh *m, float x, float y, float z, float power, float nx, float nz, uint32_t seed, uint32_t kind) {
    if (x < 0.0f || x >= CHUNK_SIZE || z < 0.0f || z >= CHUNK_SIZE) return;
    SkyLight L = { { x, y, z }, power, nx, nz, seed, kind };
    push_light(&m->air, &m->nair, &m->capair, &L);
}

void build_lights(Mesh *m, ChunkGen *g) {
    if (!m->count) return;
    float bx0 = m->mn[0], bz0 = m->mn[2], bx1 = m->mx[0], bz1 = m->mx[2];
    int cap = 1024, ns = 0;
    LightSrc *src = (LightSrc *)malloc(sizeof(LightSrc) * cap);
    if (!src) fatal("out of memory building chunk lights");
#define ADD_LIGHT(X, Y, Z, R, W) do { if (ns == cap) { cap *= 2; src = (LightSrc *)realloc(src, sizeof(LightSrc) * cap); \
        if (!src) fatal("out of memory building chunk lights"); } \
        src[ns].x = (X); src[ns].y = (Y); src[ns].z = (Z); src[ns].range = (R); src[ns].w = (W); ns++; } while (0)

    for (int i = 0; i < g->nl; ++i) {
        const Lamp *L = &g->lamp[i];
        float x = (float)(L->x - g->ox) + L->dx * 1.7f, z = (float)(L->z - g->oz) + L->dz * 1.7f;
        if (x < bx0 - 34.0f || x > bx1 + 34.0f || z < bz0 - 34.0f || z > bz1 + 34.0f) continue;
        uint32_t fault = lamp_fault(L);
        if ((fault & 3u) == 1u) continue;   /* dead: lights nothing */
        ADD_LIGHT(x, LAMP_HEIGHT - 0.4f, z, 34.0f, (uint32_t)L->type | fault << 2);
        air_light(m, x, LAMP_HEIGHT - 0.4f, z, 70.0f, 0.0f, 0.0f, fault, (L->type ? AIR_LAMP_LED : AIR_LAMP_SODIUM) << 8);
    }
    for (int i = 0; i < g->nb; ++i) {
        const Building *b = &g->bld[i];
        /* plots wholly out of reach can be skipped without generating them */
        double rr = 0.5 * sqrt((double)(b->foot.x1 - b->foot.x0) * (b->foot.x1 - b->foot.x0) + (double)(b->foot.z1 - b->foot.z0) * (b->foot.z1 - b->foot.z0)) + 14.0;
        float lx = (float)(b->ox - g->ox), lz = (float)(b->oz - g->oz);
        if (lx < bx0 - rr || lx > bx1 + rr || lz < bz0 - rr || lz > bz1 + rr) continue;
        const BGraph *bg = chunk_gen_graph(g, i);
        if (bg->archetype == ARCH_ARCOLOGY) {
            /* the arcology's real lights: cold light from the crown, sodium in the portals */
            Interiors in;
            interiors_plan(b, bg, &in);
            for (int k = 0; in.arc && k < in.arc->nlight; ++k) {
                const ALight *al = &in.arc->light[k];
                float ax, az;
                arcology_to_local(in.arc, al->x, al->z, &ax, &az);
                DVec2 w = to_world(b, ax, az);
                float x = (float)(w.x - g->ox), z = (float)(w.z - g->oz);
                if (x < bx0 - 34.0f || x > bx1 + 34.0f || z < bz0 - 34.0f || z > bz1 + 34.0f) continue;
                ADD_LIGHT(x, al->y, z, 34.0f, (uint32_t)al->type | (uint32_t)(al->type == AL_COLD ? LE_ARC_CROWN : LE_ARC_SODIUM) << 26);
            }
            interiors_free(&in);
        }
        int retail = bg->groundMode == GF_RETAIL;
        if (retail && !b->shop) continue;
        for (int si = 0; si < bg->nsec; ++si) {
            const BSection *s = &bg->sec[si];
            if (s->y0 > SIDEWALK_HEIGHT + 0.01f) continue;
            const BLoop *l = &s->foot.outer;
            for (int k = 0; k < l->n; ++k) {
                FaceKey fk = face_key(l, k, 0);
                EdgeInfo e = edge_of(l, k);
                float step = retail ? 6.0f : 8.0f;
                for (int seg = 0; seg * step < e.len; ++seg) {
                    float u0 = seg * step, u1 = minf(u0 + step, e.len);
                    if (u1 - u0 < 2.0f) continue;
                    uint32_t h = 3u;                               /* lobby: warm white */
                    if (retail) {
                        h = city_shop_hash(b->seed, si, fk.face, seg, e.len, fk.next, fk.endConvex);
                        if (!city_shop_lit(h, b->shop)) continue;
                        h = (h & ~3u) | 2u;
                    }
                    float u = (u0 + u1) * 0.5f;
                    DVec2 w = to_world(b, e.ax + e.ex * u - e.ez * 1.0f, e.az + e.ez * u + e.ex * 1.0f);
                    float x = (float)(w.x - g->ox), z = (float)(w.z - g->oz);
                    if (x < bx0 - 12.0f || x > bx1 + 12.0f || z < bz0 - 12.0f || z > bz1 + 12.0f) continue;
                    int party = 0;   /* a face against a neighbour is not a shopfront */
                    for (int j = 0; j < g->nb && !party; ++j)
                        if (j != i && g->bblk[j] == g->bblk[i] && inside_building(&g->bld[j], w.x, w.z, 0.0)) party = 1;
                    if (party) continue;
                    float nx = b->c * -e.ez - b->s * e.ex, nz = b->s * -e.ez + b->c * e.ex;   /* out of the shopfront */
                    uint32_t face = (uint32_t)(int)floorf(atan2f(nz, nx) / 6.2831853f * 256.0f + 256.5f) & 255u;
                    uint32_t bay = (uint32_t)minf((u1 - u0) * 4.0f + 0.5f, 63.0f);
                    ADD_LIGHT(x, SIDEWALK_HEIGHT + 1.4f, z, 12.0f, (h & 0xFFFFu) | face << 16 | bay << 24);
                    air_light(m, x, SIDEWALK_HEIGHT + 2.0f, z, 5.0f, nx, nz, h, ((h & 3u) == 2u ? AIR_SHOP : AIR_LOBBY) << 8);
                }
            }
        }
    }
#undef ADD_LIGHT

    uint32_t *rec = (uint32_t *)calloc(LIGHT_PAGE_RECS * 4, sizeof(uint32_t));
    if (!rec) fatal("out of memory building chunk lights");
    float sx = maxf((bx1 - bx0) * 0.25f, 0.25f), sz = maxf((bz1 - bz0) * 0.25f, 0.25f);
    float hdr[4] = { bx0, bz0, 1.0f / sx, 1.0f / sz };
    memcpy(rec, hdr, sizeof(hdr));
    uint32_t next = 5;
    for (int t = 0; t < 16; ++t) {
        float tx0 = bx0 + (t & 3) * sx, tz0 = bz0 + (t >> 2) * sz, tx1 = tx0 + sx, tz1 = tz0 + sz;
        uint32_t start = next, count = 0;
        for (int i = 0; i < ns && next < LIGHT_PAGE_RECS; ++i) {
            float dx = maxf(maxf(tx0 - src[i].x, src[i].x - tx1), 0.0f);
            float dz = maxf(maxf(tz0 - src[i].z, src[i].z - tz1), 0.0f);
            if (dx * dx + dz * dz > src[i].range * src[i].range) continue;
            uint32_t *o = &rec[next * 4];
            memcpy(&o[0], &src[i].x, 4); memcpy(&o[1], &src[i].y, 4); memcpy(&o[2], &src[i].z, 4);
            o[3] = src[i].w;
            next++; count++;
        }
        rec[4 + t] = (start << 16) | count;
    }
    free(src);
    m->lights = rec;
    m->nlights = next;
}
