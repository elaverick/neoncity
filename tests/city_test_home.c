/* Part of tests/city_test.c (#included, after city_test_slice.c): the
 * residential slice (roadmap HM0 in ~/.claude/plans/residential-buildings.md,
 * the target in docs/residential-target.md).
 *
 *   city_test --home [ARCHETYPE] [--style STYLE]
 *                        the slice block (HOME_SLICE_X/Z): its zone, fringe,
 *                        affluence and region, its street frontage, and the
 *                        capture views: --cam lines at 3, 10, 30 and 100 m from
 *                        its frontage and one from the air over it. ARCHETYPE
 *                        (detached, semi, terrace) and STYLE (one of the
 *                        region palettes') are checked and echoed: nothing
 *                        builds houses until HM2/HM3, which force them here.
 *                        It lists the block's plots (HM1, src/city/plots.c)
 *   city_test --home find [X Z]
 *                        suburb blocks through the home city's fringe
 *                        (or near X Z): candidates for HOME_SLICE_X/Z
 *   city_test --home regions
 *                        every settlement's region (the palette its houses
 *                        take), and how many fall in each
 *   city_test --home ring [RAYS]
 *                        the land in from the city's edge by real distance,
 *                        by zone, and how deep the houses reach (HM1)
 *   city_test --home rays [RAYS]
 *                        each ray's zones every 250 m out from the centre
 *
 * tools/home_capture.py reads the --cam lines (they start "view ") and takes
 * the shots; tools/home_board.py lays them beside the references. */

/* The slice block: a suburb block in the home city's ring of houses (HM1).
 * If a change to the zoning moves it out of the suburbs, the check fails:
 * pick another with `city_test --home find` and update these and the table
 * in docs/residential-target.md. */
#define HOME_SLICE_X 8196.7
#define HOME_SLICE_Z 7477.6

/* The regions (docs/residential-target.md, "Regions"; plan_region_of): each
 * settlement's houses take the palette of the region its centre lies in. */
#define HR_COUNT PLAN_REGION_COUNT
/* the styles of each region's palette (the mood board's), "" ending each */
static const char *const HR_STYLES[HR_COUNT][8] = {
    { "craftsman", "spanish", "ranch", "tract", "rowhouse", "tripledecker", "" },
    { "victorian", "georgian", "interwar", "postwar", "modern", "" },
    { "villa", "townhouse", "" },
    { "japan", "china", "korea", "" },
    { "shophouse", "linkhouse", "" },
    { "rcc", "bungalow", "" },
    { "villa", "courtyard", "" },
    { "block", "walled", "" },
    { "selfbuilt", "colonial", "" },
};
static const char *const HM_ARCH[] = { "detached", "semi", "terrace" };

static int home_regions(void) {
    int n[HR_COUNT] = { 0 };
    for (int k = 0; k < plan_settlement_count(); ++k) {
        double lat, lon;
        plan_settlement_latlon(k, &lat, &lon);
        const int r = plan_settlement_region(k);
        n[r]++;
        printf("  %-28s %7.2f %8.2f  %s\n", plan_settlement_name(k), lat, lon, plan_region_code(r));
    }
    for (int r = 0; r < HR_COUNT; ++r) printf("%-5s %-30s %d\n", plan_region_code(r), plan_region_title(r), n[r]);
    CHECK(plan_settlement_region(0) == PLAN_REGION_NA, "the home city falls in %s, not na", plan_region_code(plan_settlement_region(0)));
    return g_failures ? 1 : 0;
}

/* The block of the plan round (x, z) whose lot holds it; 0 if none. */
static int hm_block_at(PlanArea *pa, double x, double z, Block *out) {
    plan_area(x - 1.0, z - 1.0, x + 1.0, z + 1.0, pa);
    for (int i = 0; i < pa->nblocks; ++i)
        if (pa->blocks[i].lot.n >= 3 && poly_contains(&pa->blocks[i].lot, x, z, 0.0)) { *out = pa->blocks[i]; return 1; }
    return 0;
}

/* The lot's longest edge along a road (its street frontage): its index, or -1. */
static int hm_frontage(const Block *b) {
    int best = -1;
    double bl = 0.0;
    for (int i = 0; i < b->lot.n; ++i) {
        const DVec2 a = b->lot.v[i], c = b->lot.v[(i + 1) % b->lot.n];
        const double l = hypot(c.x - a.x, c.z - a.z);
        if (b->lot.e[i].cls == ROAD_NONE || b->lot.e[i].bank) continue;
        if (l > bl) { bl = l; best = i; }
    }
    return best;
}

static const char *hm_zone(int z) {
    static const char *const N[ZONE_COUNT] = { "core", "centre", "residential", "industrial", "park", "rural", "suburb" };
    return z >= 0 && z < ZONE_COUNT ? N[z] : "?";
}

static const char *hm_road(int c) { return c == ROAD_LOCAL ? "local" : c == ROAD_ARTERIAL ? "arterial" : c == ROAD_BOULEVARD ? "boulevard" : "none"; }

/* Lot width across its frontage and depth back from it (m). */
static void hm_lot_size(const Block *b, int f, double *w, double *d) {
    const DVec2 a = b->lot.v[f], c = b->lot.v[(f + 1) % b->lot.n];
    const double l = hypot(c.x - a.x, c.z - a.z), ux = (c.x - a.x) / l, uz = (c.z - a.z) / l;
    double s0 = 1e30, s1 = -1e30, t0 = 1e30, t1 = -1e30;
    for (int i = 0; i < b->lot.n; ++i) {
        const double px = b->lot.v[i].x - a.x, pz = b->lot.v[i].z - a.z, s = px * ux + pz * uz, t = -px * uz + pz * ux;
        s0 = fmin(s0, s); s1 = fmax(s1, s); t0 = fmin(t0, t); t1 = fmax(t1, t);
    }
    *w = s1 - s0;
    *d = t1 - t0;
}

static int home_find(double x, double z, int around) {
    PlanArea *pa = (PlanArea *)malloc(sizeof(PlanArea));
    if (!pa) fatal("out of memory finding home sites");
    const DVec2 home = plan_town_centre(0, 0);
    int shown = 0;
    for (int ray = 0; ray < (around ? 1 : 16) && shown < 40; ++ray) {
        const double th = ray * 2.0 * 3.14159265358979 / 16.0, dx = cos(th), dz = sin(th);
        /* out along the ray to the fringe's inner half (fringe 0.2 to 0.7), where the suburbs go, every 150 m */
        const double far = around ? 600.0 : dlen(home) + plan_home_reach() + 3000.0;
        for (double r = 0.0; r <= far; r += 150.0) {
            const double px = around ? x + r * dx : home.x + r * dx, pz = around ? z + r * dz : home.z + r * dz;
            const float fr = plan_fringe(px, pz);
            if (!around && fr < 0.2f) continue;
            if (!around && fr > 0.7f) break;
            Block b;
            if (!hm_block_at(pa, px, pz, &b) || b.zone != ZONE_SUBURB) continue;
            const int f = hm_frontage(&b);
            if (f < 0) continue;
            double w, d;
            hm_lot_size(&b, f, &w, &d);
            if (w < 60.0 || d < 30.0) continue;
            const DVec2 c = poly_centroid(&b.lot);
            printf("  suburb lot %.0f x %.0f m (%s frontage), fringe %.2f, affluence %.2f, density %.2f at (%.1f, %.1f)\n", w, d,
                   hm_road(b.lot.e[f].cls), plan_fringe(c.x, c.z), plan_affluence(c.x, c.z), b.density, c.x, c.z);
            shown++;
            break;   /* one a ray */
        }
        if (around) for (double a = 0.5; a < 6.3 && shown < 12; a += 0.5) {
            const double px = x + 150.0 * cos(a), pz = z + 150.0 * sin(a);
            Block b;
            if (!hm_block_at(pa, px, pz, &b) || b.zone != ZONE_SUBURB || hm_frontage(&b) < 0) continue;
            const DVec2 c = poly_centroid(&b.lot);
            printf("  suburb lot at (%.1f, %.1f), fringe %.2f\n", c.x, c.z, plan_fringe(c.x, c.z));
            shown++;
        }
    }
    free(pa);
    printf("%d candidates\n", shown);
    return 0;
}

/* Along rays out from the home city's centre: the zone every 250 m (one letter: C core, c centre, e estate,
 * i industrial, p park, r rural, S suburb, . unbuilt) and the depth inside the built edge every 2 km. */
static int home_rays(int rays) {
    static const char Z[ZONE_COUNT] = { 'C', 'c', 'e', 'i', 'p', 'r', 'S' };
    PlanArea *pa = (PlanArea *)malloc(sizeof(PlanArea));
    if (!pa) fatal("out of memory");
    const DVec2 home = plan_town_centre(0, 0);
    const double far = dlen(home) + plan_home_reach() + 3000.0;
    for (int ray = 0; ray < rays; ++ray) {
        const double th = (ray + 0.5) * 2.0 * 3.14159265358979 / rays, dx = cos(th), dz = sin(th);
        char line[1024];
        int n = 0, built = 0;
        for (double r = 0.0; r <= far && n < 1000; r += 250.0) {
            Block b;
            const double px = home.x + r * dx, pz = home.z + r * dz;
            line[n++] = hm_block_at(pa, px, pz, &b) ? Z[b.zone] : plan_city_edge(px, pz) < 0.0 ? ',' : '.';
            if (line[n - 1] != '.' && line[n - 1] != ',') built = n;
        }
        line[built] = 0;
        printf("%5.1f deg: %s\n", th * 57.2958, line);
    }
    free(pa);
    return 0;
}

/* The land in from the city's edge, by real distance: along `rays` rays from
 * the home centre, from where the last block ends (ray_end), each block whose
 * centre lies within 300 m of the ray counted once by its lot's area into the
 * 500 m band its centre falls in. Fills share[band][zone] (of the band's
 * land) and, for each ray, how deep its houses reach: the innermost band
 * still mostly (half or more) suburb, counting in from the edge. */
#define RING_BANDS 16
static double g_ringS[RING_BANDS], g_ringN[RING_BANDS];   /* the edge measure's depth (plan_edge_depth) summed per band, and how many */
static void ring_survey(int rays, double share[RING_BANDS][ZONE_COUNT], double *depthMean, double *depthMin, double *depthMax) {
    PlanArea *pa = (PlanArea *)malloc(sizeof(PlanArea));
    if (!pa) fatal("out of memory surveying the ring");
    const DVec2 home = plan_town_centre(0, 0);
    double land[RING_BANDS][ZONE_COUNT];
    memset(land, 0, sizeof(land));
    *depthMean = 0.0; *depthMin = 1e30; *depthMax = 0.0;
    uint32_t *seen = (uint32_t *)malloc(sizeof(uint32_t) * 65536);
    for (int ray = 0; ray < rays; ++ray) {
        const double th = (ray + 0.5) * 2.0 * 3.14159265358979 / rays, dx = cos(th), dz = sin(th);
        const double end = ray_end(pa, home, dx, dz, 1);
        double ray_land[RING_BANDS][ZONE_COUNT];
        memset(ray_land, 0, sizeof(ray_land));
        int nseen = 0;
        for (double r = end; r > end - 500.0 * RING_BANDS && r > 0.0; r -= 400.0) {
            const double px = home.x + r * dx, pz = home.z + r * dz;
            plan_area(px - 300.0, pz - 300.0, px + 300.0, pz + 300.0, pa);
            for (int i = 0; i < pa->nblocks; ++i) {
                const Block *b = &pa->blocks[i];
                if (b->lot.n < 3) continue;
                const DVec2 c = poly_centroid(&b->lot);
                const double along = (c.x - home.x) * dx + (c.z - home.z) * dz, across = fabs(-(c.x - home.x) * dz + (c.z - home.z) * dx);
                const int band = (int)floor((end - along) / 500.0);
                if (across > 300.0 || band < 0 || band >= RING_BANDS) continue;
                int dup = 0;
                for (int k = 0; k < nseen && !dup; ++k) dup = seen[k] == b->seed;
                if (dup || nseen >= 65536) continue;
                seen[nseen++] = b->seed;
                ray_land[band][b->zone] += poly_area(&b->lot);
                g_ringS[band] += plan_edge_depth(c.x, c.z);
                g_ringN[band] += 1.0;
            }
        }
        int deepest = -1;
        for (int k = 0; k < RING_BANDS; ++k) {
            double all = 0.0;
            for (int z = 0; z < ZONE_COUNT; ++z) { all += ray_land[k][z]; land[k][z] += ray_land[k][z]; }
            if (all > 0.0 && ray_land[k][ZONE_SUBURB] >= 0.5 * all) deepest = k;
        }
        const double depth = 500.0 * (deepest + 1);
        *depthMean += depth / rays;
        *depthMin = fmin(*depthMin, depth);
        *depthMax = fmax(*depthMax, depth);
    }
    for (int k = 0; k < RING_BANDS; ++k) {
        double all = 0.0;
        for (int z = 0; z < ZONE_COUNT; ++z) all += land[k][z];
        for (int z = 0; z < ZONE_COUNT; ++z) share[k][z] = all > 0.0 ? land[k][z] / all : 0.0;
    }
    free(seen);
    free(pa);
}

static int home_ring(int rays) {
    double share[RING_BANDS][ZONE_COUNT], mean, lo, hi;
    ring_survey(rays, share, &mean, &lo, &hi);
    printf("land in from the edge (%d rays), %% by zone:\n   km    core centre estate  indus   park  rural suburb\n", rays);
    for (int k = 0; k < RING_BANDS; ++k) {
        printf("  %4.1f", 0.5 * k);
        for (int z = 0; z < ZONE_COUNT; ++z) printf(" %6.0f", 100.0 * share[k][z]);
        printf("   (measure %.0f m)\n", g_ringN[k] > 0.0 ? g_ringS[k] / g_ringN[k] : 0.0);
    }
    printf("houses reach %.1f km in (%.1f to %.1f)\n", mean / 1000.0, lo / 1000.0, hi / 1000.0);
    return 0;
}

static int home_slice_plots(const Block *b);

static int home_cams(int argc, char **argv) {
    if (argc > 2 && strcmp(argv[2], "ring") == 0) return home_ring(argc > 3 ? atoi(argv[3]) : 24);
    if (argc > 2 && strcmp(argv[2], "rays") == 0) return home_rays(argc > 3 ? atoi(argv[3]) : 8);
    if (argc > 2 && strcmp(argv[2], "find") == 0) return home_find(argc > 4 ? atof(argv[3]) : 0.0, argc > 4 ? atof(argv[4]) : 0.0, argc > 4);
    if (argc > 2 && strcmp(argv[2], "regions") == 0) return home_regions();
    const char *arch = NULL, *style = NULL;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--style") == 0 && i + 1 < argc) style = argv[++i];
        else arch = argv[i];
    }
    const int region = plan_settlement_region(0);
    if (arch) {
        int ok = 0;
        for (int k = 0; k < 3; ++k) ok |= strcmp(arch, HM_ARCH[k]) == 0;
        CHECK(ok, "no house archetype %s (detached, semi, terrace)", arch);
    }
    if (style) {
        int ok = -1;
        for (int r = 0; r < HR_COUNT && ok < 0; ++r)
            for (int k = 0; HR_STYLES[r][k][0]; ++k)
                if (strcmp(style, HR_STYLES[r][k]) == 0) { ok = r; break; }
        CHECK(ok >= 0, "no house style %s", style);
        if (ok >= 0 && ok != region) printf("note: %s is a %s style, forced in a %s city\n", style, plan_region_code(ok), plan_region_code(region));
    }

    PlanArea *pa = (PlanArea *)malloc(sizeof(PlanArea));
    if (!pa) fatal("out of memory");
    Block b;
    const int found = hm_block_at(pa, HOME_SLICE_X, HOME_SLICE_Z, &b);
    free(pa);
    CHECK(found, "no block's lot holds the slice (%.1f, %.1f)", HOME_SLICE_X, HOME_SLICE_Z);
    if (!found) { printf("%d failure(s)\n", g_failures); return 1; }
    CHECK(b.zone == ZONE_SUBURB, "the slice's block is %s, not suburb", hm_zone(b.zone));
    const int f = hm_frontage(&b);
    CHECK(f >= 0, "the slice's block has no street frontage");
    if (f < 0) { printf("%d failure(s)\n", g_failures); return 1; }
    double w, d;
    hm_lot_size(&b, f, &w, &d);
    const DVec2 c = poly_centroid(&b.lot);
    printf("home slice: a %s block, lot %.0f x %.0f m (%.0f m2) on a %s street, at (%.1f, %.1f); fringe %.2f, affluence %.2f, density %.2f\n",
           hm_zone(b.zone), w, d, poly_area(&b.lot), hm_road(b.lot.e[f].cls), c.x, c.z, plan_fringe(c.x, c.z),
           plan_affluence(c.x, c.z), b.density);
    printf("  %.0f m inside the built edge (city edge %.0f, built edge %.0f); the suburbs' ring %.0f m deep, open land %.0f m\n",
           plan_edge_depth(c.x, c.z), plan_city_edge(c.x, c.z), plan_built_edge(c.x, c.z), plan_suburb_depth(c.x, c.z), plan_open_depth(c.x, c.z));
    printf("  city %s, region %s (%s); forced: %s, %s\n", plan_settlement_name(0), plan_region_code(region), plan_region_title(region),
           arch ? arch : "no archetype", style ? style : "no style");
    CHECK(w >= 60.0 && d >= 30.0, "the slice's lot is %.0f x %.0f m, under 60 x 30", w, d);
    home_slice_plots(&b);

    /* the views: the frontage's middle, looking in at a house front 6 m back */
    const DVec2 a = b.lot.v[f], e = b.lot.v[(f + 1) % b.lot.n];
    const double l = hypot(e.x - a.x, e.z - a.z), ux = (e.x - a.x) / l, uz = (e.z - a.z) / l;
    double nx = uz, nz = -ux;   /* out of the lot, into the street */
    const DVec2 mid = dv((a.x + e.x) * 0.5, (a.z + e.z) * 0.5);
    if ((c.x - mid.x) * nx + (c.z - mid.z) * nz > 0.0) { nx = -nx; nz = -nz; }
    const double tx = mid.x - nx * 6.0, tz = mid.z - nz * 6.0, gy = city_ground_y(tx, tz);
    sl_load(tx, tz, 2);
    static const int DIST[4] = { 3, 10, 30, 100 };
    int views = 0;
    for (int k = 0; k < 4; ++k) {
        char name[32];
        snprintf(name, sizeof(name), "home_%03dm", DIST[k]);
        views += sl_view(name, tx, gy + 3.0, tz, nx, nz, DIST[k] + 6.0, gy, 0.0, 0);
    }
    {   /* from the air: 150 m out over the street, 90 m up, looking down at the block */
        const double px = c.x + nx * 150.0, pz = c.z + nz * 150.0, eye = city_ground_y(c.x, c.z) + 90.0;
        const double yaw = atan2(c.x - px, -(c.z - pz)) * 57.2958, pitch = atan2(city_ground_y(c.x, c.z) - eye, 150.0) * 57.2958;
        printf("view home_aerial --cam %.2f %.2f %.1f %.1f %.2f   (150 m out, 90 m up)\n", px, pz, yaw, pitch, eye);
        views++;
    }
    CHECK(views == 5, "only %d of the 5 views have a line of sight", views);
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("ok\n");
    return 0;
}

/* ---------------------------------------------------------------------------
 * HM1: the suburbs' plots and where the suburbs go
 * ------------------------------------------------------------------------- */

typedef struct PlotStats { int blocks, plots, rows, arch[HOME_ARCH_COUNT], semisNA; double lotArea, plotArea; } PlotStats;

static void plot_corner(const HomePlot *p, double u, double v, double *x, double *z) {
    *x = p->ox + p->tx * u + p->nx * v;
    *z = p->oz + p->tz * u + p->nz * v;
}

static int plots_overlap(const HomePlot *p, const HomePlot *q) {
    const double eps = 0.02;
    double cp[4][2], cq[4][2];
    plot_corner(p, p->u0 + eps, eps, &cp[0][0], &cp[0][1]); plot_corner(p, p->u1 - eps, eps, &cp[1][0], &cp[1][1]);
    plot_corner(p, p->u1 - eps, p->depth - eps, &cp[2][0], &cp[2][1]); plot_corner(p, p->u0 + eps, p->depth - eps, &cp[3][0], &cp[3][1]);
    plot_corner(q, q->u0 + eps, eps, &cq[0][0], &cq[0][1]); plot_corner(q, q->u1 - eps, eps, &cq[1][0], &cq[1][1]);
    plot_corner(q, q->u1 - eps, q->depth - eps, &cq[2][0], &cq[2][1]); plot_corner(q, q->u0 + eps, q->depth - eps, &cq[3][0], &cq[3][1]);
    const double ax[4][2] = { { p->tx, p->tz }, { p->nx, p->nz }, { q->tx, q->tz }, { q->nx, q->nz } };
    for (int k = 0; k < 4; ++k) {
        double a0 = 1e30, a1 = -1e30, b0 = 1e30, b1 = -1e30;
        for (int i = 0; i < 4; ++i) {
            const double s = cp[i][0] * ax[k][0] + cp[i][1] * ax[k][1], t = cq[i][0] * ax[k][0] + cq[i][1] * ax[k][1];
            a0 = fmin(a0, s); a1 = fmax(a1, s); b0 = fmin(b0, t); b1 = fmax(b1, t);
        }
        if (a1 <= b0 || b1 <= a0) return 0;
    }
    return 1;
}

/* One suburb block's plots (src/city/plots.c) against docs/residential-target.md, "Plots": each inside
 * the lot and clear of the others; frontages, depths and front gardens in range; terraces in rows of
 * 3 or more units at most 48 m long, semis in pairs (none in North America). */
static void check_block_plots(const Block *b, PlotStats *st) {
    static HomePlot P[HOME_MAX_PLOTS];
    static double rowLen[HOME_MAX_PLOTS];
    const int n = city_block_plots(b, P, HOME_MAX_PLOTS);
    st->blocks++;
    st->plots += n;
    st->lotArea += poly_area(&b->lot);
    memset(rowLen, 0, sizeof(rowLen));
    int lastRow = -1;
    for (int i = 0; i < n; ++i) {
        const HomePlot *p = &P[i];
        const double w = p->u1 - p->u0, c[4][2] = { { p->u0, 0.0 }, { p->u1, 0.0 }, { p->u1, p->depth }, { p->u0, p->depth } };
        for (int k = 0; k < 4; ++k) {
            double x, z;
            plot_corner(p, c[k][0], c[k][1], &x, &z);
            CHECK(poly_contains(&b->lot, x, z, 0.05), "block %08x plot %d corner %d (%.1f, %.1f) is off its lot", b->seed, i, k, x, z);
        }
        for (int j = i + 1; j < n; ++j) CHECK(!plots_overlap(p, &P[j]), "block %08x plots %d and %d overlap", b->seed, i, j);
        static const double LO[HOME_ARCH_COUNT] = { 10.0, 7.0, 4.2 }, HI[HOME_ARCH_COUNT] = { 18.0, 10.0, 6.0 };
        CHECK(p->arch < HOME_ARCH_COUNT && w >= LO[p->arch % 3] - 1e-3 && w <= HI[p->arch % 3] + 1e-3, "block %08x plot %d (%s) is %.2f m wide",
              b->seed, i, HM_ARCH[p->arch % 3], w);
        CHECK(p->depth >= 20.0f && p->depth <= 45.0f, "block %08x plot %d is %.1f m deep", b->seed, i, p->depth);
        CHECK(p->front >= 3.0f && p->front <= p->depth - 17.0f + 1e-3f, "block %08x plot %d's front garden is %.1f m of %.1f", b->seed, i, p->front, p->depth);
        CHECK(p->storeys >= 1 && p->storeys <= 3, "block %08x plot %d has %d storeys", b->seed, i, p->storeys);
        CHECK(p->unit < p->units, "block %08x plot %d is unit %d of %d", b->seed, i, p->unit, p->units);
        if (p->arch == HOME_TERRACE) CHECK(p->units >= 3 && p->units <= 12, "block %08x plot %d's terrace has %d units", b->seed, i, p->units);
        if (p->arch == HOME_SEMI) CHECK(p->units == 2, "block %08x plot %d's pair has %d halves", b->seed, i, p->units);
        if (p->arch == HOME_SEMI && p->region == PLAN_REGION_NA) st->semisNA++;
        rowLen[p->row % HOME_MAX_PLOTS] += w;
        if (p->row != lastRow) { st->rows++; lastRow = p->row; }
        st->arch[p->arch % 3]++;
        st->plotArea += w * p->depth;
    }
    for (int i = 0; i < n; ++i)
        if (P[i].arch == HOME_TERRACE)
            CHECK(rowLen[P[i].row % HOME_MAX_PLOTS] <= 48.0 + 1e-3, "block %08x's terrace %d is %.1f m long", b->seed, P[i].row, rowLen[P[i].row % HOME_MAX_PLOTS]);
}

static void print_plot_stats(const char *what, const PlotStats *st) {
    printf("  %s: %d suburb blocks, %d plots in %d rows (%d detached, %d semi, %d terrace), %.0f%% of the lots' land; %.1f plots a block\n",
           what, st->blocks, st->plots, st->rows, st->arch[HOME_DETACHED], st->arch[HOME_SEMI], st->arch[HOME_TERRACE],
           st->lotArea > 0.0 ? 100.0 * st->plotArea / st->lotArea : 0.0, st->blocks ? (double)st->plots / st->blocks : 0.0);
}

/* Where the suburbs go (the user's answers, 2026-11-16, in the roadmap): a ring of houses 2-4 km deep
 * round the edge (varied), houses most of its land, the estates inside; inner suburbs only as pockets of
 * a few streets, a few per cent of the inner residential land; every suburb block's plots valid. */
static void check_suburbs(void) {
    double share[RING_BANDS][ZONE_COUNT], mean, lo, hi;
    ring_survey(24, share, &mean, &lo, &hi);
    double ring = 0.0, inner = 0.0;
    for (int k = 1; k <= 4; ++k) ring += share[k][ZONE_SUBURB] / 4.0;
    for (int k = 12; k < RING_BANDS; ++k) inner += share[k][ZONE_SUBURB] / (RING_BANDS - 12);
    printf("suburbs: houses reach %.1f km in from the edge (%.1f to %.1f over 24 rays); %.0f%% of the land 0.5-2.5 km in, %.0f%% 6-8 km in\n",
           mean / 1000.0, lo / 1000.0, hi / 1000.0, 100.0 * ring, 100.0 * inner);
    CHECK(mean >= 2000.0 && mean <= 4500.0, "the houses reach %.1f km in, not 2-4", mean / 1000.0);
    CHECK(ring >= 0.4, "houses are only %.0f%% of the ring's land", 100.0 * ring);
    CHECK(inner <= 0.1, "houses are %.0f%% of the land 6-8 km in", 100.0 * inner);

    /* the inner pockets: round the home centre, clusters of suburb blocks among the estates */
    PlanArea *pa = (PlanArea *)malloc(sizeof(PlanArea));
    if (!pa) fatal("out of memory checking the suburbs");
    enum { MAXP = 4096 };
    static DVec2 pc[MAXP];
    static double parea[MAXP];
    static int parent[MAXP];
    int np = 0;
    double estate = 0.0, pocket = 0.0;
    const DVec2 home = plan_town_centre(0, 0);
    PlotStats st;
    memset(&st, 0, sizeof(st));
    const double H = 5000.0, T = 500.0;
    for (double tz = home.z - H; tz < home.z + H; tz += T)
        for (double tx = home.x - H; tx < home.x + H; tx += T) {
            plan_area(tx, tz, tx + T, tz + T, pa);
            for (int i = 0; i < pa->nblocks; ++i) {
                const Block *b = &pa->blocks[i];
                const DVec2 c = poly_centroid(&b->curb);
                if (c.x < tx || c.x >= tx + T || c.z < tz || c.z >= tz + T || b->lot.n < 3) continue;   /* each block once, by its centre */
                if (plan_edge_depth(c.x, c.z) < plan_suburb_depth(c.x, c.z)) continue;          /* the ring's, not a pocket */
                if (b->zone == ZONE_RESIDENTIAL) estate += poly_area(&b->lot);
                if (b->zone != ZONE_SUBURB || np >= MAXP) continue;
                pocket += poly_area(&b->lot);
                pc[np] = c; parea[np] = poly_area(&b->lot); parent[np] = np; np++;
                if (st.blocks < 400) check_block_plots(b, &st);
            }
        }
    /* a pocket: the blocks whose centres lie within 300 m of one another's, chained */
    for (int i = 0; i < np; ++i)
        for (int j = i + 1; j < np; ++j) {
            if (dlen(dsub(pc[i], pc[j])) > 300.0) continue;
            int a = i, b = j;
            while (parent[a] != a) a = parent[a];
            while (parent[b] != b) b = parent[b];
            if (a != b) parent[b] = a;
        }
    int pockets = 0, biggest = 0;
    double biggestArea = 0.0;
    for (int i = 0; i < np; ++i) {
        int r = i;
        while (parent[r] != r) r = parent[r];
        if (r != i) continue;
        pockets++;
        int n = 0;
        double a = 0.0;
        for (int j = 0; j < np; ++j) {
            int q = j;
            while (parent[q] != q) q = parent[q];
            if (q == i) { n++; a += parea[j]; }
        }
        if (n > biggest) biggest = n;
        biggestArea = fmax(biggestArea, a);
    }
    const double pshare = estate + pocket > 0.0 ? pocket / (estate + pocket) : 0.0;
    printf("  inner suburbs within %.0f km of the centre: %d pockets of %d blocks (the largest %d blocks, %.0f m2), %.1f%% of the inner residential land\n",
           H / 1000.0, pockets, np, biggest, biggestArea, 100.0 * pshare);
    CHECK(pockets > 0, "no inner suburbs within %.0f km of the centre", H / 1000.0);
    CHECK(biggest <= 10 && biggestArea <= 200000.0, "an inner suburb of %d blocks (%.0f m2) is a district, not a few streets", biggest, biggestArea);
    CHECK(pshare <= 0.1, "inner suburbs are %.1f%% of the inner residential land", 100.0 * pshare);
    print_plot_stats("the pockets' plots", &st);

    /* the ring's plots: along 8 rays, 0.3-3 km in from the edge */
    memset(&st, 0, sizeof(st));
    int seenN = 0;
    static uint32_t seen[8192];
    for (int ray = 0; ray < 8; ++ray) {
        const double th = (ray + 0.25) * 2.0 * 3.14159265358979 / 8.0, dx = cos(th), dz = sin(th), end = ray_end(pa, home, dx, dz, 1);
        for (double r = end - 300.0; r > end - 3000.0 && r > 0.0; r -= 450.0) {
            const double x = home.x + r * dx, z = home.z + r * dz;
            plan_area(x - 200.0, z - 200.0, x + 200.0, z + 200.0, pa);
            for (int i = 0; i < pa->nblocks; ++i) {
                const Block *b = &pa->blocks[i];
                if (b->zone != ZONE_SUBURB || b->lot.n < 3) continue;
                int dup = 0;
                for (int k = 0; k < seenN && !dup; ++k) dup = seen[k] == b->seed;
                if (dup || seenN >= 8192) continue;
                seen[seenN++] = b->seed;
                check_block_plots(b, &st);
            }
        }
    }
    print_plot_stats("the ring's plots", &st);
    CHECK(st.blocks >= 20, "only %d suburb blocks along 8 rays through the ring", st.blocks);
    CHECK(st.plotArea >= 0.45 * st.lotArea, "the ring's plots take only %.0f%% of their blocks' land", 100.0 * st.plotArea / fmax(st.lotArea, 1.0));
    CHECK(st.semisNA == 0, "%d semi-detached houses in North America", st.semisNA);
    CHECK(st.arch[HOME_DETACHED] > st.arch[HOME_TERRACE], "the home city's suburbs have more terraced (%d) than detached (%d) houses",
          st.arch[HOME_TERRACE], st.arch[HOME_DETACHED]);
    free(pa);
}

/* The slice block's plots, checked and listed (HM1). */
static int home_slice_plots(const Block *b) {
    static HomePlot P[HOME_MAX_PLOTS];
    const int n = city_block_plots(b, P, HOME_MAX_PLOTS);
    PlotStats st;
    memset(&st, 0, sizeof(st));
    check_block_plots(b, &st);
    print_plot_stats("the slice's plots", &st);
    for (int i = 0; i < n; ++i) {
        const HomePlot *p = &P[i];
        printf("    plot %2d: %-8s row %2d, unit %d of %d, %4.1f m wide, %4.1f deep, front garden %.1f m, %d storeys, %.1f m high\n", i,
               HM_ARCH[p->arch % 3], p->row, p->unit + 1, p->units, p->u1 - p->u0, p->depth, p->front, p->storeys, city_home_height(p));
    }
    CHECK(n >= 6, "the slice's block has only %d plots", n);
    return n;
}
