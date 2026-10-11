/* Building generator lab: generates every building in an area of the plan,
 * reports what the grammar chose per district, how often proposals were
 * retried or simplified and why they were rejected, and fails if any
 * accepted building does not validate or comes out differently twice.
 *
 *   building_lab [size_m] [verbose_count]
 *
 * With verbose_count > 0 it also prints that many full decision logs. */
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
    const double T = 300.0;
    for (double tz = c.z - size * 0.5; tz < c.z + size * 0.5; tz += T)
        for (double tx = c.x - size * 0.5; tx < c.x + size * 0.5; tx += T) {
            plan_area(tx, tz, tx + T, tz + T, pa);
            for (int bi = 0; bi < pa->nblocks; ++bi) {
                int n = city_block_buildings(&pa->blocks[bi], bl, 256);
                for (int i = 0; i < n; ++i) {
                    const Building *b = &bl[i];
                    if (b->ox < tx || b->ox >= tx + T || b->oz < tz || b->oz >= tz + T) continue;   /* once each */
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
