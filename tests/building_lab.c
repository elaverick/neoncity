/* Building generator lab: generates every building in an area of the plan,
 * reports what the grammar chose per district, how often proposals were
 * retried or simplified and why they were rejected, and fails if any
 * accepted building does not validate or comes out differently twice.
 *
 *   building_lab [size_m] [verbose_count]
 *   building_lab industry [size_m] [verbose_count]
 *
 * With verbose_count > 0 it also prints that many full decision logs. The
 * industry mode generates only the industrial zones' plots (and every
 * substation), so it can survey the city's fringe, where they lie. */
#include "building.h"
#include "hash.h"
#include "mathlib.h"

#define MAX_REASONS 64

typedef struct Reason { char text[96]; int count; } Reason;
static Reason g_reasons[MAX_REASONS];
static int g_nreasons;

static void count_reasons(const char *log) {
    for (const char *p = strstr(log, "rejected: "); p; p = strstr(p + 1, "rejected: ")) {
        char line[96];
        const char *s = p + 10;
        int n = 0;
        /* strip section numbers so similar rejections group together */
        while (*s && *s != '\n' && n < 95) {
            if (*s >= '0' && *s <= '9') { while ((*s >= '0' && *s <= '9') || *s == '.') s++; line[n++] = '#'; continue; }
            line[n++] = *s++;
        }
        line[n] = 0;
        int k = 0;
        while (k < g_nreasons && strcmp(g_reasons[k].text, line)) k++;
        if (k == g_nreasons && g_nreasons < MAX_REASONS) { strcpy(g_reasons[k].text, line); g_nreasons++; }
        if (k < g_nreasons) g_reasons[k].count++;
    }
}

int main(int argc, char **argv) {
    int industry = argc > 1 && !strcmp(argv[1], "industry");
    if (industry) { argv++; argc--; }
    double size = argc > 1 ? atof(argv[1]) : 3000.0;
    int verbose = argc > 2 ? atoi(argv[2]) : 0;
    DVec2 c = plan_town_centre(0, 0);
    PlanArea *pa = (PlanArea *)malloc(sizeof(PlanArea));
    Building *bl = (Building *)malloc(sizeof(Building) * 256);
    BGraph *g = (BGraph *)malloc(sizeof(BGraph)), *g2 = (BGraph *)malloc(sizeof(BGraph));
    char *buf = (char *)malloc(1 << 16);
    int counts[DIST_COUNT][ARCH_COUNT] = { { 0 } }, total = 0, simplified = 0, retried = 0, failures = 0;
    int ground[4] = { 0 }, arcades = 0, projections = 0, steps = 0, spans = 0, bridges = 0, courts = 0, doors = 0;
    long attempts = 0;
    int styles[PST_COUNT] = { 0 }, colossal = 0, units[IU_COUNT] = { 0 }, pipes = 0, ways = 0, decks = 0, holes = 0, framed = 0;
    float tallest = 0.0f, stackH = 0.0f;
    double stackX = 0.0, stackZ = 0.0;
    Building stackB;
    memset(&stackB, 0, sizeof(stackB));
    enum { MAX_PCH = 160 };   /* chunks holding the biggest plants, whose meshes are measured against the page budget */
    int pch[MAX_PCH][2], npch = 0;
    const double T = 300.0;
    for (double tz = c.z - size * 0.5; tz < c.z + size * 0.5; tz += T)
        for (double tx = c.x - size * 0.5; tx < c.x + size * 0.5; tx += T) {
            plan_area(tx, tz, tx + T, tz + T, pa);
            for (int bi = 0; bi < pa->nblocks; ++bi) {
                int n = city_block_buildings(&pa->blocks[bi], bl, 256);
                for (int i = 0; i < n; ++i) {
                    const Building *b = &bl[i];
                    if (b->ox < tx || b->ox >= tx + T || b->oz < tz || b->oz >= tz + T) continue;   /* once each */
                    if (industry && b->zone != ZONE_INDUSTRIAL) {
                        /* elsewhere only the substations: the archetype is chosen before anything is built */
                        float W = b->foot.x1 - b->foot.x0, D = b->foot.z1 - b->foot.z0;
                        if (b->kind == BK_MEGA || (W < 27.0f || D < 27.0f) || (W < 34.0f && D < 34.0f)) continue;
                        bgen_build(b, g, NULL);
                        if (g->archetype != ARCH_SUBSTATION) continue;
                    }
                    BLog log = { buf, 1 << 16, 0 };
                    buf[0] = 0;
                    bgen_build(b, g, &log);
                    bgen_build(b, g2, NULL);
                    if (memcmp(g, g2, sizeof(BGraph)) != 0) { printf("FAIL: building %08x differs when regenerated\n", b->seed); failures++; }
                    if (!bgen_validate(g, b, NULL)) { printf("FAIL: building %08x does not validate\n%s", b->seed, buf); failures++; }
                    total++;
                    counts[g->district][g->archetype]++;
                    attempts += g->attempts;
                    if (g->attempts > 1) retried++;
                    if (g->simplified) simplified++;
                    ground[g->groundMode]++;
                    doors += g->ndoor;
                    for (int s = 0; s < g->nsec; ++s) {
                        arcades += g->sec[s].arr == ARR_ARCADE;
                        projections += g->sec[s].arr == ARR_PROJECTION;
                        steps += g->sec[s].arr == ARR_STEP;
                        spans += g->sec[s].arr == ARR_SPAN;
                        bridges += g->sec[s].arr == ARR_BRIDGE;
                        courts += g->sec[s].foot.hole.n > 0;
                    }
                    if (bgen_industrial(g)) {
                        if (g->archetype == ARCH_PLANT) { styles[g->style]++; colossal += g->tier == PT_COLOSSAL; }
                        pipes += g->npipe; ways += g->nacc; holes += g->nhole;
                        for (int s = 0; s < g->nsec; ++s) {
                            units[g->sec[s].unit]++;
                            decks += g->sec[s].type == SEC_DECK;
                            framed += g->sec[s].braceBy != 0;
                            if (g->sec[s].type == SEC_UNIT) tallest = g->sec[s].y1 > tallest ? g->sec[s].y1 : tallest;
                            if (g->sec[s].unit == IU_STACK && g->sec[s].y1 > stackH) {   /* the tallest chimney, and where to stand to see it */
                                float fx, fz, fr;
                                bloop_circle(&g->sec[s].foot.outer, &fx, &fz, &fr);
                                stackH = g->sec[s].y1;
                                stackX = b->ox + b->c * fx - b->s * fz; stackZ = b->oz + b->s * fx + b->c * fz;
                                stackB = *b;
                            }
                        }
                        if (verbose > 0 && g->attempts > 1) { printf("%s\n", buf); verbose--; }
                        /* a few of each kind to look at: a camera out in the street, looking across the plot */
                        static int shown[PST_COUNT + 3];
                        int flares = 0;
                        for (int s = 0; s < g->nsec; ++s) flares += g->sec[s].unit == IU_FLARE;
                        int kind = g->archetype == ARCH_SUBSTATION ? PST_COUNT : g->tier == PT_COLOSSAL ? PST_COUNT + 1 : flares ? PST_COUNT + 2 : g->style;
                        if (industry && shown[kind] < 2 && g->nsec >= (kind == PST_COUNT ? 5 : 12)) {
                            shown[kind]++;
                            float W = b->foot.x1 - b->foot.x0, D = b->foot.z1 - b->foot.z0;
                            double lx = 0.0, lz = -(D * 0.5 + 25.0);   /* off the plot's -z side, or whichever side faces the street */
                            if (b->frontage & SIDE_N) lz = D * 0.5 + 25.0;
                            else if (b->frontage & SIDE_W) { lx = -(W * 0.5 + 25.0); lz = 0.0; }
                            else if (b->frontage & SIDE_E) { lx = W * 0.5 + 25.0; lz = 0.0; }
                            double wx = b->ox + b->c * lx - b->s * lz, wz = b->oz + b->s * lx + b->c * lz;
                            printf("%s %s%s at world (%.0f, %.0f), plot %.0fx%.0f m; --cam %.1f %.1f %.1f 12\n",
                                   g->archetype == ARCH_SUBSTATION ? "substation" : bgen_style_name(g->style), g->tier == PT_COLOSSAL ? "(colossal)" : "", flares ? " with flares" : "",
                                   b->ox, b->oz, W, D, wx, wz, atan2(b->ox - wx, -(b->oz - wz)) * 57.29578);
                            for (int s = 0; s < g->nsec; ++s)
                                if (g->sec[s].unit == IU_FLARE || g->sec[s].unit == IU_STACK) {
                                    float fx, fz, fr;
                                    bloop_circle(&g->sec[s].foot.outer, &fx, &fz, &fr);
                                    printf("  %s at world (%.1f, %.1f), %.0f m", bgen_unit_name(g->sec[s].unit), b->ox + b->c * fx - b->s * fz, b->oz + b->s * fx + b->c * fz, g->sec[s].y1);
                                    if (g->sec[s].unit == IU_FLARE)   /* its fire's timing id: flare_id in src/city/mesh_industry.c */
                                        printf(", flame id %u", hash_combine(g->seed ^ 0xF1A3u, (uint32_t)s) & 63u);
                                    printf("\n");
                                }
                        }
                        int cx = (int)floor(b->ox / CHUNK_SIZE), cz = (int)floor(b->oz / CHUNK_SIZE), seen = 0;
                        for (int k = 0; k < npch && !seen; ++k) seen = pch[k][0] == cx && pch[k][1] == cz;
                        if (!seen && npch < MAX_PCH && g->nsec >= 12) { pch[npch][0] = cx; pch[npch][1] = cz; npch++; }
                    }
                    count_reasons(buf);
                    if (g->archetype == ARCH_TWIN || g->archetype == ARCH_GATEWAY) {
                        /* a camera looking across the plot's long axis (through a gateway's passage) */
                        float W = b->foot.x1 - b->foot.x0, D = b->foot.z1 - b->foot.z0;
                        double lx = W >= D ? 0.0 : -(W * 0.5 + 30.0), lz = W >= D ? -(D * 0.5 + 30.0) : 0.0;
                        double dx = W >= D ? -b->s : b->c, dz = W >= D ? b->c : b->s;
                        printf("%s at world (%.0f, %.0f), plot %.0fx%.0f m, height %.0f m; --cam %.1f %.1f %.1f 8 6\n",
                               bgen_archetype_name(g->archetype), b->ox, b->oz, W, D, b->height,
                               b->ox + b->c * lx - b->s * lz, b->oz + b->s * lx + b->c * lz, atan2(dx, -dz) * 57.29578);
                    }
                    if (verbose > 0 && (g->archetype == ARCH_TWIN || g->archetype == ARCH_GATEWAY || g->archetype == ARCH_PROJECTION || g->attempts > 2)) {
                        printf("%s\n", buf);
                        verbose--;
                    }
                }
            }
        }
    printf("%d buildings in a %.0f m square around the spawn\n", total, size);
    printf("%-22s", "");
    for (int d = 0; d < DIST_COUNT; ++d) printf("%11s", bgen_district_name(d));
    printf("\n");
    for (int a = 0; a < ARCH_COUNT; ++a) {
        printf("%-22s", bgen_archetype_name(a));
        for (int d = 0; d < DIST_COUNT; ++d) printf("%11d", counts[d][a]);
        printf("\n");
    }
    printf("ground floors: %d shops, %d lobbies, %d double-height lobbies, %d void decks\n", ground[0], ground[1], ground[2], ground[3]);
    printf("arrangements: %d arcades, %d projections, %d step-outs, %d spans, %d sky bridges; %d courtyards; %.2f doors per building\n",
           arcades, projections, steps, spans, bridges, courts, total ? (double)doors / total : 0.0);
    printf("plants: %d gas, %d gothic, %d night; %d colossal; tallest unit %.0f m\n", styles[PST_GAS], styles[PST_GOTHIC],
           styles[PST_NIGHT], colossal, tallest);
    if (stackH > 0.0f) {
        /* from the street off the plot's front, 60 m back, looking at the chimney */
        const Building *b = &stackB;
        float W = b->foot.x1 - b->foot.x0, D = b->foot.z1 - b->foot.z0;
        double lx = 0.0, lz = -(D * 0.5 + 60.0);
        if (b->frontage & SIDE_N) lz = D * 0.5 + 60.0;
        else if (b->frontage & SIDE_W) { lx = -(W * 0.5 + 60.0); lz = 0.0; }
        else if (b->frontage & SIDE_E) { lx = W * 0.5 + 60.0; lz = 0.0; }
        double wx = b->ox + b->c * lx - b->s * lz, wz = b->oz + b->s * lx + b->c * lz;
        printf("tallest stack: %.0f m at world (%.1f, %.1f); --cam %.1f %.1f %.1f 25\n", stackH, stackX, stackZ, wx, wz,
               atan2(stackX - wx, -(stackZ - wz)) * 57.29578);
    }
    printf("units:");
    for (int u = 1; u < IU_COUNT; ++u) if (units[u]) printf(" %d %s,", units[u], bgen_unit_name(u));
    printf(" %d decks (%d vessel collars), %d framed columns; %d pipes, %d ladders and stairs\n", decks, holes, framed, pipes, ways);
    if (npch) {
        uint32_t most = 0, mostL = 0;
        double sum = 0.0, sumL = 0.0;
        int full = 0;
        for (int k = 0; k < npch; ++k) {
            Mesh m = { 0 }, lt = { 0 };
            city_build_mesh(pch[k][0], pch[k][1], &m);
            city_build_litter(pch[k][0], pch[k][1], &lt);
            sum += m.count; sumL += lt.count;
            if (m.count > most) most = m.count;
            if (lt.count > mostL) mostL = lt.count;
            full += lt.count >= 170000u;   /* near litter.c's own BUDGET, where it starts leaving things out */
            mesh_free(&m);
            mesh_free(&lt);
        }
        printf("%d chunks with large plants: mesh vertices avg %.0f, max %u; litter avg %.0f, max %u, %d near its limit (budget %d)\n",
               npch, sum / npch, most, sumL / npch, mostL, full, MAX_CHUNK_PAGES * PAGE_VERTS);
        if (most > (uint32_t)(MAX_CHUNK_PAGES * PAGE_VERTS) || mostL > (uint32_t)(MAX_CHUNK_PAGES * PAGE_VERTS)) {
            printf("FAIL: a plant's chunk exceeds the page budget\n");
            failures++;
        }
    }
    printf("attempts per building %.2f, %d retried (%.1f%%), %d simplified (%.1f%%)\n", total ? (double)attempts / total : 0.0,
           retried, total ? 100.0 * retried / total : 0.0, simplified, total ? 100.0 * simplified / total : 0.0);
    printf("rejections:\n");
    for (int k = 0; k < g_nreasons; ++k) printf("  %6d  %s\n", g_reasons[k].count, g_reasons[k].text);
    if (simplified * 10 > total) { printf("FAIL: more than 10%% of buildings fell back to a plain block\n"); failures++; }
    free(pa); free(bl); free(g); free(g2); free(buf);
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all buildings valid and repeatable\n");
    return 0;
}
