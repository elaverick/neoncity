#include "bake.h"
#include "interior_def.h"
#include "json.h"
#include <time.h>

/* The catalogue's baker (docs/interior-impostors.md, section 22): assets/interiors/catalogue.json
 * names the modules and their generator's inputs, and each module has two tiers. A tier's sidecar
 * records the hash of what went into it (scene_input_hash), so whatever the hash no longer matches
 * is what needs baking. The sidecar is written last, and taken away before a bake starts: files a
 * bake left half written never count as up to date. */

BakeSettings bake_settings_default(void) {
    /* the defaults IM1 chose (section 13), the view grid in degrees until bake_settings_finish */
    BakeSettings b;
    memset(&b, 0, sizeof(b));
    b.v = (ViewGrid){ 5, 10, GRID_SLOPE, 60.0f, -30.0f, 70.0f, 0.8f, 0.12f };
    b.tpm = 16.0f;
    b.margin = 1.0f;
    b.ic.K = 3;
    b.ic.maxSteps = 64; b.ic.maxTexStep = 1.0f; b.ic.refine = 6; b.ic.unknownW = 0.15f; b.ic.offGridW = 0.1f;
    b.tc = trace_defaults(b.ic.K);
    b.tc.visibleCap = BAKE_VISIBLE_CAP;
    b.extSpp = 8192;
    return b;
}

void bake_settings_finish(BakeSettings *b) {
    b->tc.slots = b->ic.lastSlot ? CAP_SLOTS : b->ic.K;
    /* a march keeps to a texel a step on a finer grid: as many more steps */
    b->ic.maxSteps = (int)ceilf((float)b->ic.maxSteps * fmaxf(b->tpm / 16.0f, 1.0f));
    b->v.azMax *= 3.14159265f / 180.0f;
    b->v.elMin *= 3.14159265f / 180.0f;
    b->v.elMax *= 3.14159265f / 180.0f;
}

bool bake_module(const char *dir, Scene *s, const BakeSettings *b, bool validation, const char *preview) {
    s->inputHash = b->direct ? 0 : scene_input_hash(s, b);
    Gpu *g = gpu_create(validation);
    gpu_load_scene(g, s);
    gpu_set_trace(g, b->direct ? NULL : &b->tc);
    TexGrid t = tex_grid(s, b->tpm, b->margin);
    Capture c;
    if (b->direct) gpu_capture(g, s, &b->v, &t, &c);
    else gpu_capture_bases(g, s, &b->v, &t, b->extSpp, &c);
    bool ok = bake_write(dir, s, &c, &b->ic, b->direct ? NULL : &b->tc);
    if (ok && preview) {   /* straight on, and as the street sees it, 25 degrees up */
        char up[1200];
        const char *dot = strrchr(preview, '.');
        snprintf(up, sizeof(up), "%.*s_up%s", dot ? (int)(dot - preview) : (int)strlen(preview), preview, dot ? dot : "");
        ok = bake_preview(preview, &c, 0.0f, 5.0f) && bake_preview(up, &c, 25.0f * 3.14159265f / 180.0f, 5.0f);
    }
    free(c.layers);
    free(c.counts);
    free(c.ext);
    gpu_destroy(g);
    return ok;
}

/* base: the settings with its view grid still in degrees. Tier 1 is the near tier, at twice the texels. */
static BakeSettings tier_settings(const BakeSettings *base, int tier) {
    BakeSettings b = *base;
    if (tier) b.tpm *= 2.0f;
    bake_settings_finish(&b);
    return b;
}

static void tier_name(char *out, size_t size, const char *module, int tier) {
    snprintf(out, size, "%s%s", module, tier ? ".near" : "");
}

/* What a tier's files say of it. "" means they are whole and carry this hash; otherwise why not. */
typedef struct TierState { char why[300]; InteriorModule m; bool loaded; double diskMb; } TierState;

/* Reads the tier's sidecar and each file's header. *hard: the files are broken, rather than just old. */
static void tier_read(const char *out, const char *name, uint64_t hash, TierState *st, bool *hard) {
    char path[1200], err[1400];
    memset(st, 0, sizeof(*st));
    *hard = false;
    snprintf(path, sizeof(path), "%s/%s.json", out, name);
    FILE *fp = fopen(path, "rb");
    if (!fp) { snprintf(st->why, sizeof(st->why), "not baked"); *hard = true; return; }
    fclose(fp);
    if (!interior_module_load(path, &st->m, err, sizeof(err))) { snprintf(st->why, sizeof(st->why), "%.280s", err); *hard = true; return; }
    st->loaded = true;
    for (int c = 0; c < INTERIOR_CHANNELS; ++c) {
        InteriorImage img;
        if (!interior_image_load(&st->m, c, &img, err, sizeof(err))) { snprintf(st->why, sizeof(st->why), "%.280s", err); *hard = true; return; }
        st->diskMb += (double)img.size / 1e6;
        interior_image_free(&img);
    }
    Json *doc = json_load(path, err, sizeof(err));
    const Json *h = doc ? json_get(doc, "input_hash") : NULL;
    char want[32];
    snprintf(want, sizeof(want), "%016llx", (unsigned long long)hash);
    if (!h || h->type != JSON_STRING) snprintf(st->why, sizeof(st->why), "no input hash");
    else if (strcmp(h->string, want)) snprintf(st->why, sizeof(st->why), "inputs changed");
    json_free(doc);
}

/* Records the hash in a sidecar whose bake is known to match its inputs, without baking: only if the
 * floor's layout and the trace it records are what they would be now. */
static bool tier_adopt(const char *out, const char *name, const Scene *s, const BakeSettings *b, uint64_t hash) {
    char path[1200], trace[160], floorInfo[200], line[80];
    snprintf(path, sizeof(path), "%s/%s.json", out, name);
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *text = malloc((size_t)size + 1);
    if (!text) fatal("out of memory adopting a bake");
    size_t got = fread(text, 1, (size_t)size, fp);
    fclose(fp);
    text[got] = 0;
    snprintf(trace, sizeof(trace), "\"trace\": { \"samples\": %d, \"bounces\": %d, \"clamp\": %.1f, \"seed\": %u }", b->tc.spp, b->tc.bounces, b->tc.clamp, b->tc.seed);
    snprintf(floorInfo, sizeof(floorInfo), "\"floor\": { %s }", s->floorInfo);
    char *baker = strstr(text, "\"baker\":"), *end = baker ? strchr(baker, '\n') : NULL;
    bool ok = strstr(text, trace) && (!s->floorInfo[0] || strstr(text, floorInfo)) && end && !strstr(text, "\"input_hash\"");
    if (!ok) {
        printf("  %s: not adopted: its sidecar already has a hash, or its layout or trace differs\n", name);
        free(text);
        return false;
    }
    const char *eol = end > text && end[-1] == '\r' ? "\r\n" : "\n";
    snprintf(line, sizeof(line), "  \"input_hash\": \"%016llx\",%s", (unsigned long long)hash, eol);
    fp = fopen(path, "wb");
    ok = fp != NULL;
    if (fp) {
        fwrite(text, 1, (size_t)(end + 1 - text), fp);
        fputs(line, fp);
        fwrite(end + 1, 1, got - (size_t)(end + 1 - text), fp);
        ok = !ferror(fp);
        fclose(fp);
    }
    free(text);
    return ok;
}

int catalogue_bake(const BakeSettings *base, const CatalogueRun *run) {
    if (!floor_catalogue_load(run->assets)) return 1;
    time_t start = time(NULL);
    int baked = 0, fresh = 0, stale = 0, seen = 0;
    bool failed = false;
    for (int i = 0; i < FLOOR_CATALOGUE_COUNT && !failed; ++i) {
        const FloorEntry *e = &FLOOR_CATALOGUE[i];
        if (run->only ? strcmp(run->only, e->name) != 0 : !e->drawn) continue;
        seen++;
        Scene s;
        if (!scene_build(&s, e->name, -1, run->assets)) { failed = true; break; }
        for (int tier = 0; tier < 2 && !failed; ++tier) {
            BakeSettings b = tier_settings(base, tier);
            uint64_t hash = scene_input_hash(&s, &b);
            char name[80], path[1200];
            tier_name(name, sizeof(name), e->name, tier);
            TierState st;
            bool hard;
            tier_read(run->out, name, hash, &st, &hard);
            if (!run->force && !st.why[0]) { printf("%-28s up to date\n", name); fresh++; continue; }
            const char *why = st.why[0] ? st.why : "forced";
            if (run->dry) { printf("%-28s would bake: %s\n", name, why); stale++; continue; }
            if (run->adopt) {
                if (hard) { printf("%-28s can't adopt: %s\n", name, why); failed = true; }
                else if (tier_adopt(run->out, name, &s, &b, hash)) { printf("%-28s adopted\n", name); baked++; }
                else failed = true;
                continue;
            }
            printf("%-28s baking: %s\n", name, why);
            fflush(stdout);
            snprintf(path, sizeof(path), "%s/%s.json", run->out, name);
            remove(path);
            size_t len = strlen(s.name);
            if (tier) snprintf(s.name + len, sizeof(s.name) - len, ".near");
            time_t t0 = time(NULL);
            if (!bake_module(run->out, &s, &b, run->validation, NULL)) failed = true;
            s.name[len] = 0;
            printf("%-28s took %.1f min\n", name, difftime(time(NULL), t0) / 60.0);
            fflush(stdout);
            baked++;
        }
        scene_free(&s);
    }
    if (run->only && !seen) { fprintf(stderr, "no module called %s in the catalogue\n", run->only); return 2; }
    if (run->dry) printf("%d tiers up to date, %d to bake\n", fresh, stale);
    else if (run->adopt) printf("%d tiers up to date, %d adopted\n", fresh, baked);
    else printf("%d tiers up to date, %d baked in %.1f min\n", fresh, baked, difftime(time(NULL), start) / 60.0);
    return failed ? 1 : 0;
}

/* A sidecar's bay means: never negative (the loader's check), and neither black all over nor absurd. */
static const char *means_fault(const InteriorModule *m) {
    float sum = 0.0f;
    for (int b = 0; b < m->bays; ++b) {
        for (int k = 0; k < 3; ++k) {
            if (!(m->bayMean[b][k] < 100.0f)) return "has a radiance mean out of range";
            sum += m->bayMean[b][k];
        }
        for (int k = 0; k < 4; ++k) {
            if (!(m->bayExt[b][k] <= m->extMax)) return "has an exterior mean out of range";
            sum += m->bayExt[b][k];
        }
    }
    return sum > 0.0f ? NULL : "has means that are all zero";
}

int catalogue_check(const BakeSettings *base, const char *assets, const char *out) {
    if (!floor_catalogue_load(assets)) return 1;
    int faults = 0, stale = 0, modules = 0;
    double disk[2] = { 0, 0 };
    /* the same answers as the engine's reader, so what's in the file is what the engine will load */
    char path[1200], err[1400], names[FLOOR_CATALOGUE_MAX][64];
    int named = 0;
    snprintf(path, sizeof(path), "%s/interiors/catalogue.json", assets);
    if (!interior_catalogue_load(path, names, INTERIOR_MAX_MODULES, &named, err, sizeof(err))) {
        printf("FAIL: %s\n", err);
        return 1;
    }
    for (int i = 0; i < FLOOR_CATALOGUE_COUNT; ++i) {
        const FloorEntry *e = &FLOOR_CATALOGUE[i];
        if (!e->drawn) continue;
        modules++;
        Scene s;
        if (!scene_build(&s, e->name, -1, assets)) { printf("FAIL: %s doesn't build\n", e->name); faults++; continue; }
        float baseTpm = 0.0f;
        for (int tier = 0; tier < 2; ++tier) {
            BakeSettings b = tier_settings(base, tier);
            char name[80];
            tier_name(name, sizeof(name), e->name, tier);
            TierState st;
            bool hard;
            tier_read(out, name, scene_input_hash(&s, &b), &st, &hard);
            if (hard) { printf("FAIL: %s: %s\n", name, st.why); faults++; continue; }
            const InteriorModule *m = &st.m;
            int type = s.type == MOD_CORE ? INTERIOR_CORE : s.type == MOD_CORNER ? INTERIOR_CORNER : INTERIOR_THROUGH;
            static const int STATES[LIGHTS_COUNT] = { INTERIOR_STATE_AFTER_HOURS, INTERIOR_STATE_LATE_SHIFT, INTERIOR_STATE_CLEANERS, INTERIOR_STATE_SCREENS };
            const char *fault = means_fault(m);
            if (strcmp(m->name, name)) fault = "names another module";
            else if (m->type != type) fault = "has the wrong type";
            else if (m->state != STATES[e->state]) fault = "has the wrong light state";
            else if (m->bays != e->p.bays || fabsf(m->bayWidth - e->p.bayW) > 0.001f) fault = "has the wrong bays";
            else if (fabsf(m->size[0] - s.W) > 0.001f || fabsf(m->size[1] - s.H) > 0.001f || fabsf(m->size[2] - s.D) > 0.001f) fault = "has the wrong size";
            else if (tier && m->tpm != 2.0f * baseTpm) fault = "isn't twice its base's texels";
            if (!tier) baseTpm = m->tpm;
            if (fault) { printf("FAIL: %s %s\n", name, fault); faults++; }
            if (st.why[0]) stale++;
            disk[tier] += st.diskMb;
            printf("%-28s %-11s %2d x %.1f m, %4.1f deep  %3d x %3d  %6.1f MB  %s\n", name, LIGHT_STATE_NAMES[e->state], m->bays, m->bayWidth, m->size[2],
                   m->dim[0], m->dim[1], st.diskMb, fault ? "FAULT" : st.why[0] ? st.why : "up to date");
        }
        scene_free(&s);
    }
    if (named != modules) { printf("FAIL: the engine reads %d modules, the baker %d\n", named, modules); faults++; }
    printf("%d modules: base %.0f MB, near %.0f MB on disk\n", modules, disk[0], disk[1]);
    /* Old bakes aren't a fault: re-bakes are committed at a milestone's end, and a hash can differ on another compiler. */
    if (stale) printf("%d tiers out of date: interior_bake --catalogue bakes them\n", stale);
    printf(faults ? "%d faults\n" : "catalogue ok\n", faults);
    return faults ? 1 : 0;
}
