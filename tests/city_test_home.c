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
 *                        builds houses until HM2/HM3, which force them here
 *   city_test --home find [X Z]
 *                        residential blocks through the home city's fringe
 *                        (or near X Z): candidates for HOME_SLICE_X/Z
 *   city_test --home regions
 *                        every settlement's region (the palette its houses
 *                        take), and how many fall in each
 *
 * tools/home_capture.py reads the --cam lines (they start "view ") and takes
 * the shots; tools/home_board.py lays them beside the references. */

/* The slice block: a residential block in the home city's inner fringe, where
 * HM1's suburbs go. If HM1's re-banding moves it out of residential, the check
 * fails: pick another with `city_test --home find` and update these and the
 * table in docs/residential-target.md. */
#define HOME_SLICE_X 8196.7
#define HOME_SLICE_Z 7477.6

/* The regions (docs/residential-target.md, "Regions"): each settlement's
 * houses take the palette of the region its centre lies in. */
enum { HR_NA, HR_UK, HR_SEU, HR_EAS, HR_SEA, HR_SAS, HR_MENA, HR_SSA, HR_LAT, HR_COUNT };
static const char *const HR_NAME[HR_COUNT] = { "na", "uk", "seu", "eas", "sea", "sas", "mena", "ssa", "lat" };
static const char *const HR_TITLE[HR_COUNT] = {
    "North America", "Britain and NW Europe", "Southern Europe", "East Asia", "South-East Asia",
    "South Asia", "Middle East and North Africa", "Sub-Saharan Africa", "Latin America" };
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

static int hr_of(double lat, double lon) {
    if (lon < -30.0) return lat >= 24.0 ? HR_NA : HR_LAT;
    if (lon >= 110.0 && lat < -10.0) return HR_UK;           /* Australia and New Zealand: brick and tile */
    if (lon >= 92.0) return lat >= 22.0 ? HR_EAS : HR_SEA;
    if (lon >= 60.0 && lat >= 5.0 && lat < 37.0) return HR_SAS;
    if (lat >= 45.0) return HR_UK;
    if (lat >= 36.0) return HR_SEU;
    if (lat >= 12.0) return HR_MENA;
    return HR_SSA;
}

static int hr_settlement(int k) {
    double lat, lon;
    terrain_dir_latlon(plan_settlement(k).dir, &lat, &lon);
    return hr_of(lat, lon);
}

static int home_regions(void) {
    int n[HR_COUNT] = { 0 };
    for (int k = 0; k < plan_settlement_count(); ++k) {
        double lat, lon;
        terrain_dir_latlon(plan_settlement(k).dir, &lat, &lon);
        const int r = hr_of(lat, lon);
        n[r]++;
        printf("  %-28s %7.2f %8.2f  %s\n", plan_settlement_name(k), lat, lon, HR_NAME[r]);
    }
    for (int r = 0; r < HR_COUNT; ++r) printf("%-5s %-30s %d\n", HR_NAME[r], HR_TITLE[r], n[r]);
    CHECK(hr_settlement(0) == HR_NA, "the home city falls in %s, not na", HR_NAME[hr_settlement(0)]);
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
    static const char *const N[ZONE_COUNT] = { "core", "centre", "residential", "industrial", "park", "rural" };
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
            if (!hm_block_at(pa, px, pz, &b) || b.zone != ZONE_RESIDENTIAL) continue;
            const int f = hm_frontage(&b);
            if (f < 0) continue;
            double w, d;
            hm_lot_size(&b, f, &w, &d);
            if (w < 60.0 || d < 30.0) continue;
            const DVec2 c = poly_centroid(&b.lot);
            printf("  residential lot %.0f x %.0f m (%s frontage), fringe %.2f, affluence %.2f, density %.2f at (%.1f, %.1f)\n", w, d,
                   hm_road(b.lot.e[f].cls), plan_fringe(c.x, c.z), plan_affluence(c.x, c.z), b.density, c.x, c.z);
            shown++;
            break;   /* one a ray */
        }
        if (around) for (double a = 0.5; a < 6.3 && shown < 12; a += 0.5) {
            const double px = x + 150.0 * cos(a), pz = z + 150.0 * sin(a);
            Block b;
            if (!hm_block_at(pa, px, pz, &b) || b.zone != ZONE_RESIDENTIAL || hm_frontage(&b) < 0) continue;
            const DVec2 c = poly_centroid(&b.lot);
            printf("  residential lot at (%.1f, %.1f), fringe %.2f\n", c.x, c.z, plan_fringe(c.x, c.z));
            shown++;
        }
    }
    free(pa);
    printf("%d candidates\n", shown);
    return 0;
}

static int home_cams(int argc, char **argv) {
    if (argc > 2 && strcmp(argv[2], "find") == 0) return home_find(argc > 4 ? atof(argv[3]) : 0.0, argc > 4 ? atof(argv[4]) : 0.0, argc > 4);
    if (argc > 2 && strcmp(argv[2], "regions") == 0) return home_regions();
    const char *arch = NULL, *style = NULL;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--style") == 0 && i + 1 < argc) style = argv[++i];
        else arch = argv[i];
    }
    const int region = hr_settlement(0);
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
        if (ok >= 0 && ok != region) printf("note: %s is a %s style, forced in a %s city\n", style, HR_NAME[ok], HR_NAME[region]);
    }

    PlanArea *pa = (PlanArea *)malloc(sizeof(PlanArea));
    if (!pa) fatal("out of memory");
    Block b;
    const int found = hm_block_at(pa, HOME_SLICE_X, HOME_SLICE_Z, &b);
    free(pa);
    CHECK(found, "no block's lot holds the slice (%.1f, %.1f)", HOME_SLICE_X, HOME_SLICE_Z);
    if (!found) { printf("%d failure(s)\n", g_failures); return 1; }
    CHECK(b.zone == ZONE_RESIDENTIAL, "the slice's block is %s, not residential", hm_zone(b.zone));
    const int f = hm_frontage(&b);
    CHECK(f >= 0, "the slice's block has no street frontage");
    if (f < 0) { printf("%d failure(s)\n", g_failures); return 1; }
    double w, d;
    hm_lot_size(&b, f, &w, &d);
    const DVec2 c = poly_centroid(&b.lot);
    printf("home slice: a %s block, lot %.0f x %.0f m (%.0f m2) on a %s street, at (%.1f, %.1f); fringe %.2f, affluence %.2f, density %.2f\n",
           hm_zone(b.zone), w, d, poly_area(&b.lot), hm_road(b.lot.e[f].cls), c.x, c.z, plan_fringe(c.x, c.z),
           plan_affluence(c.x, c.z), b.density);
    printf("  city %s, region %s (%s); forced: %s, %s\n", plan_settlement_name(0), HR_NAME[region], HR_TITLE[region],
           arch ? arch : "no archetype", style ? style : "no style");
    CHECK(w >= 60.0 && d >= 30.0, "the slice's lot is %.0f x %.0f m, under 60 x 30", w, d);

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
