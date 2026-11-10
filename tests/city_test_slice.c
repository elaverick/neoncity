/* Part of tests/city_test.c (#included, after city_test_vendors.c): the noodle
 * bar slice (roadmap NB0 in ~/.claude/plans/street-vendors.md, the target in
 * docs/noodle-bar-target.md).
 *
 *   city_test --slice            the slice's site and vendor, its checks, and
 *                                the capture views: --cam lines at 3, 8, 25 and
 *                                80 m from its counter (turned down the street, or as near
 *                                as 50 m, if the line of sight needs it), and an office impostor
 *                                tower's curtain wall at the same distances
 *   city_test --slice find [X Z] apron sites against a wall under the open sky
 *                                near X Z (default the old canopy bar's lane):
 *                                candidates for VENDOR_SLICE_X/Z
 *
 * tools/slice_capture.py reads the --cam lines (they start "view ") and takes
 * the shots; tools/slice_board.py lays them out beside the references. */

/* The collision outlines (walls, columns, street furniture, vendors) of the
 * chunks within r chunks of (x, z), for the lines of sight below. */
static void sl_load(double x, double z, int r) {
    static BlockLayout L;
    int cx = (int)floor(x / CHUNK_SIZE), cz = (int)floor(z / CHUNK_SIZE);
    g_nvsp = 0;
    for (int j = -r; j <= r; ++j)
        for (int i = -r; i <= r; ++i) {
            city_layout(cx + i, cz + j, &L);
            for (int c = 0; c < L.ncoll; ++c) vs_add_poly(&L, &L.coll[c]);
        }
}

/* Whether (x, z) lies on a building's plot (under an arcade or an overhang,
 * where nothing stops a walker but the view is closed in), of the chunks
 * round it. */
static int sl_under_building(double x, double z) {
    static BlockLayout L;
    int cx = (int)floor(x / CHUNK_SIZE), cz = (int)floor(z / CHUNK_SIZE);
    for (int dj = -1; dj <= 1; ++dj)
        for (int di = -1; di <= 1; ++di) {
            city_layout(cx + di, cz + dj, &L);
            for (int b = 0; b < L.nbuild; ++b) {
                const Building *B = &L.build[b];
                double dx = x - B->ox, dz = z - B->oz, lx = B->c * dx + B->s * dz, lz = -B->s * dx + B->c * dz;
                if (lx > B->foot.x0 - 0.5 && lx < B->foot.x1 + 0.5 && lz > B->foot.z0 - 0.5 && lz < B->foot.z1 + 0.5) return 1;
            }
        }
    return 0;
}

/* Nothing solid on the way from a to b (sampled every 0.15 m). */
static int sl_clear(double ax, double az, double bx, double bz) {
    double l = hypot(bx - ax, bz - az);
    int n = (int)ceil(l / 0.15);
    for (int k = 0; k <= n; ++k) {
        double t = n ? (double)k / n : 0.0;
        if (vs_solid(ax + (bx - ax) * t, az + (bz - az) * t)) return 0;
    }
    return 1;
}

typedef struct SlFind { const VendorSpot *want; int found, slices, tris, dress; Vendor v; VendorSpot spot; } SlFind;

static void sl_visit(void *ctx, const VendorSpot *s, const Vendor *v) {
    SlFind *F = (SlFind *)ctx;
    if (!s->site.slice) return;
    F->slices++;
    F->found = 1;
    F->spot = *s;
    F->v = *v;
    static VendorMesh vm;
    vm.count = 0;
    F->tris = vendor_mesh(v, VL_SHELL, &vm);
    vm.count = 0;
    F->dress = vendor_mesh(v, VL_DRESSING, &vm);
}

static const int SL_DIST[4] = { 3, 8, 25, 80 };

/* A view of the point (tx, ty, tz) from d m away along f turned by up to 82
 * degrees either way, the first whose line of sight is clear; eye at the
 * ground + 1.68 m (+ lift). Prints "view NAME --cam X Z YAW PITCH EYE_Y". */
static int sl_view(const char *name, double tx, double ty, double tz, double fx, double fz, double d, double floorY, double lift, int quiet) {
    static const int ANG[] = { 20, -20, 35, -35, 0, 50, -50, 65, -65, 75, -75, 82, -82 };
    for (int k = 0; k < (int)(sizeof(ANG) / sizeof(ANG[0])); ++k) {
        double a = ANG[k] / 57.2958, c = cos(a), s = sin(a);
        double dx = fx * c - fz * s, dz = fx * s + fz * c;
        double px = tx + dx * d, pz = tz + dz * d;
        /* the eye's 0.4 m round it clear too, and the way to the point less its last 0.3 m (the counter) */
        /* a corridor 1.2 m wide (a thin line slips past columns), the eye off the buildings' plots */
        double ox = -dz * 0.6, oz = dx * 0.6;
        if (!sl_clear(px + ox, pz + oz, tx + dx * 0.3 + ox, tz + dz * 0.3 + oz) || !sl_clear(px - ox, pz - oz, tx + dx * 0.3 - ox, tz + dz * 0.3 - oz) ||
            sl_under_building(px, pz)) continue;
        if (!sl_clear(px, pz, tx + dx * 0.3, tz + dz * 0.3) || vs_solid(px + 0.4, pz) || vs_solid(px - 0.4, pz) || vs_solid(px, pz + 0.4) || vs_solid(px, pz - 0.4)) continue;
        double eye = fmax(floorY, city_ground_y(px, pz) + SIDEWALK_HEIGHT) + 1.68 + lift;
        double yaw = atan2(tx - px, -(tz - pz)) * 57.2958, pitch = atan2(ty - eye, d) * 57.2958;
        printf("view %s --cam %.2f %.2f %.1f %.1f %.2f   (%.0f m, %+d degrees off its front)\n", name, px, pz, yaw, pitch, eye, d, ANG[k]);
        return 1;
    }
    if (!quiet) printf("view %s: no clear line of sight from %.0f m\n", name, d);
    return 0;
}

static int slice_find(double x, double z) {
    VendorSpot sp[MAX_VENDOR_SPOTS];
    int cx = (int)floor(x / CHUNK_SIZE), cz = (int)floor(z / CHUNK_SIZE), shown = 0;
    for (int j = -3; j <= 3; ++j)
        for (int i = -3; i <= 3; ++i) {
            int n = city_chunk_vendor_spots(cx + i, cz + j, sp, MAX_VENDOR_SPOTS);
            for (int k = 0; k < n; ++k) {
                const VendorSpot *s = &sp[k];
                if (s->space != SPACE_APRON || !s->site.hostWall || s->site.ceiling > 0.0f || s->site.w < 3.8f) continue;
                double mx, mz;
                vendor_spot_middle(s, &mx, &mz);
                printf("  apron site %.2f x %.2f, affluence %.2f, walk %.1f, at (%.2f, %.2f) facing (%.2f, %.2f)%s\n", s->site.w, s->site.d,
                       s->site.affluence, s->walk, mx, mz, s->fx, s->fz, s->site.slice ? ": the slice" : "");
                shown++;
            }
        }
    printf("%d candidates within 3 chunks of (%.0f, %.0f)\n", shown, x, z);
    return 0;
}

static int slice_cams(int argc, char **argv) {
    if (argc > 2 && strcmp(argv[2], "find") == 0)
        return slice_find(argc > 4 ? atof(argv[3]) : 732.2, argc > 4 ? atof(argv[4]) : -735.1);
    int cx = (int)floor(VENDOR_SLICE_X / CHUNK_SIZE), cz = (int)floor(VENDOR_SLICE_Z / CHUNK_SIZE);
    static SlFind F;
    memset(&F, 0, sizeof(F));
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) city_chunk_vendors(cx + i, cz + j, sl_visit, &F);
    CHECK(F.slices == 1, "%d slice sites round (%.1f, %.1f), not 1", F.slices, VENDOR_SLICE_X, VENDOR_SLICE_Z);
    if (!F.found) {
        /* the site without its vendor: why the grammar built none */
        VendorSpot sp[MAX_VENDOR_SPOTS];
        int n = city_chunk_vendor_spots(cx, cz, sp, MAX_VENDOR_SPOTS);
        for (int k = 0; k < n; ++k) {
            if (!sp[k].site.slice) continue;
            static char buf[1 << 15];
            BLog lg = { buf, (int)sizeof(buf), 0 };
            buf[0] = 0;
            vendor_generate(&sp[k].site, VTIER_STANDARD, NULL, 0, &F.v, &lg);
            printf("the slice's site %.2f x %.2f has no valid vendor:\n%s\n", sp[k].site.w, sp[k].site.d, lg.len > 3000 ? buf + lg.len - 3000 : buf);
        }
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    const VendorSpot *s = &F.spot;
    CHECK(F.v.archetype == VA_CANOPY && F.v.trade == VT_NOODLE, "the slice is a %s (%s), not a canopy noodle bar",
          vendor_archetype_name(F.v.archetype), vendor_trade_name(F.v.trade));
    double mx, mz;
    vendor_spot_middle(s, &mx, &mz);
    printf("slice: a %s (%s) on an apron site %.2f x %.2f at (%.2f, %.2f), facing (%.2f, %.2f); floor %.2f, affluence %.2f (%s), walk %.1f m\n",
           vendor_archetype_name(F.v.archetype), vendor_trade_name(F.v.trade), s->site.w, s->site.d, mx, mz, s->fx, s->fz, s->y,
           s->site.affluence, vendor_class_name(F.v.cls), s->walk);
    printf("  now: %d shell triangles, %d dressing triangles, %d parts, %d items, %d lights, %d emitters\n", F.tris, F.dress, F.v.npart,
           F.v.nitem, F.v.nlight, F.v.nemit);

    /* the views: the counter's middle at 1.3 m (between counter and hood), from in front */
    sl_load(mx, mz, 2);
    double tx, tz;
    vendor_spot_world(s, 0.0, s->site.d, &tx, &tz);
    int ok = 0;
    char name[32];
    for (int k = 0; k < 4; ++k) {
        snprintf(name, sizeof(name), "slice_%02dm", SL_DIST[k]);
        /* the far view: the longest clear line of sight from 80 m down to 50 (a street seldom runs straight for 80) */
        int got = 0;
        for (double d = SL_DIST[k]; d >= (k == 3 ? 50.0 : SL_DIST[k]) && !got; d -= 5.0)
            got = sl_view(name, tx, s->y + 1.3, tz, s->fx, s->fz, d, s->y, k == 3 ? 3.0 : 0.0, d > 50.0 && k == 3);
        ok += got;
    }
    CHECK(ok == 4, "only %d of the slice's 4 views see it", ok);

    /* an office impostor tower (the impostor work's capture spot, its curtain
     * wall filling the middle): the wall where the line of sight meets it,
     * then views at the same distances, at its fourth floor */
    const double ox = 1139.3, oz = 262.0, oyaw = 1.7 / 57.2958;
    double fx = sin(oyaw), fz = -cos(oyaw);
    sl_load(ox, oz, 2);
    double hit = -1.0;
    for (double t = 0.0; t < 300.0; t += 0.1)
        if (vs_solid(ox + fx * t, oz + fz * t)) { hit = t; break; }
    CHECK(hit > 0.0, "no office tower in front of the impostor capture spot");
    if (hit > 0.0) {
        double wx = ox + fx * hit, wz = oz + fz * hit, wy = city_ground_y(wx, wz) + 14.0;
        printf("office: a curtain wall at (%.2f, %.2f), %.1f m from the impostor capture spot\n", wx, wz, hit);
        sl_load(wx, wz, 2);
        int ook = 0;
        for (int k = 0; k < 4; ++k) {
            snprintf(name, sizeof(name), "office_%02dm", SL_DIST[k]);
            /* fly mode at the fourth floor, level (the camera's own height) */
            int found = 0;
            static const int ANG[] = { 0, 15, -15, 30, -30 };
            for (int a = 0; a < 5 && !found; ++a) {
                double r = ANG[a] / 57.2958, dx = -fx * cos(r) + fz * sin(r), dz = -fz * cos(r) - fx * sin(r);
                double px = wx + dx * SL_DIST[k], pz = wz + dz * SL_DIST[k];
                if (!sl_clear(px, pz, wx + dx * 0.3, wz + dz * 0.3)) continue;
                printf("view %s --cam %.2f %.2f %.1f %.1f %.2f\n", name, px, pz, atan2(wx - px, -(wz - pz)) * 57.2958, 0.0, wy);
                found = 1;
            }
            if (!found) printf("view %s: no clear line of sight from %d m\n", name, SL_DIST[k]);
            ook += found;
        }
        CHECK(ook >= 3, "only %d of the office's 4 views see it", ook);
    }
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    return 0;
}
