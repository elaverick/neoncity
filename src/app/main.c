/* NeonCity - a vast, deterministic, cyberpunk city walk in C + Vulkan.
 *
 * Controls: mouse look, WASD move, Shift sprint, Space jump,
 *           F toggle fly (debug: Space/Ctrl up/down), F7 vendor debug view, F9 toggle shadows, F10 shadow debug view,
 *           F11 fullscreen, Esc release mouse (Esc again to quit), click to recapture. */
#include <time.h>
#include "vkcore.h"
#include "renderer.h"
#include "chunks.h"
#include "player.h"
#include "layouts.h"
#include "plan.h"
#include "terrain.h"
#include "thread.h"
#include "weather.h"
#include "models.h"
#include "vendor.h"
#include "frames.h"
#include "lobby_doors.h"
#include "grass_rules.h"
#include "grass_wake.h"
#include "sky_clock.h"
#include "light_units.h"
#include "kit_gpu.h"
#include "city.h"
#include "noodle_bar.h"

static bool   g_resized;
static bool   g_captured = true;
static double g_lastMX, g_lastMY;
static bool   g_haveMouse;
static float  g_mouseDX, g_mouseDY;
static bool   g_flyPressed, g_jumpPressed, g_shotPressed, g_shadowPressed, g_debugPressed, g_impostorPressed, g_vendorDebugPressed;
static int    g_windowed[4];

static void on_resize(GLFWwindow *w, int width, int height) { g_resized = true; }

static void on_mouse(GLFWwindow *w, double x, double y) {
    if (g_haveMouse && g_captured) {
        g_mouseDX += (float)(x - g_lastMX);
        g_mouseDY += (float)(y - g_lastMY);
    }
    g_lastMX = x; g_lastMY = y;
    g_haveMouse = true;
}

static void set_capture(GLFWwindow *w, bool on) {
    g_captured = on;
    glfwSetInputMode(w, GLFW_CURSOR, on ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (on && glfwRawMouseMotionSupported()) glfwSetInputMode(w, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    g_haveMouse = false;
}

static void toggle_fullscreen(GLFWwindow *w) {
    GLFWmonitor *mon = glfwGetWindowMonitor(w);
    if (mon) {
        glfwSetWindowMonitor(w, NULL, g_windowed[0], g_windowed[1], g_windowed[2], g_windowed[3], 0);
    } else {
        glfwGetWindowPos(w, &g_windowed[0], &g_windowed[1]);
        glfwGetWindowSize(w, &g_windowed[2], &g_windowed[3]);
        GLFWmonitor *pm = glfwGetPrimaryMonitor();
        const GLFWvidmode *mode = glfwGetVideoMode(pm);
        glfwSetWindowMonitor(w, pm, 0, 0, mode->width, mode->height, mode->refreshRate);
    }
    g_resized = true;
}

static void on_key(GLFWwindow *w, int key, int sc, int action, int mods) {
    if (action != GLFW_PRESS) return;
    if (key == GLFW_KEY_ESCAPE) {
        if (g_captured) set_capture(w, false);
        else glfwSetWindowShouldClose(w, GLFW_TRUE);
    }
    if (key == GLFW_KEY_F) g_flyPressed = true;
    if (key == GLFW_KEY_SPACE) g_jumpPressed = true;
    if (key == GLFW_KEY_F7) g_vendorDebugPressed = true;
    if (key == GLFW_KEY_F8) g_impostorPressed = true;
    if (key == GLFW_KEY_F9) g_shadowPressed = true;
    if (key == GLFW_KEY_F10) g_debugPressed = true;
    if (key == GLFW_KEY_F11) toggle_fullscreen(w);
    if (key == GLFW_KEY_F12) g_shotPressed = true;
}

static void on_click(GLFWwindow *w, int button, int action, int mods) {
    if (action == GLFW_PRESS && !g_captured) set_capture(w, true);
}

static void usage(void) {
    printf("NeonCity [--validation] [--noshadows] [--city NAME] [--cam X Z YAW_DEG PITCH_DEG [HEIGHT]] [--walk SECONDS [--turn DEG_PER_S] [--walk-shot]] [--rain 0..1] [--time SECONDS] [--utc YYYY-MM-DDTHH:MM] [--ev EV100] [--preexpose STOPS] [--hdr [PAPER_NITS [PEAK_NITS]]] [--impostors [SHARE]] [--lockdoors] [--kit off|slice|all] [--bars all] [--vendor-debug] [--hours open|shut|clock] [--nbform straight|corner] [--trade noodles|soup|dumplings|skewers|kebab|burgers|fried|curry|tacos|crepes|roast|drinks] [--nograss] [--nowake] [--grass-cap N] [--grass-debug suit|density|species|clump|lod|cull|wind|wake|albedo] [--cave-debug normals|lit|lod] [--shot FILE.bmp]\n"
           "  --noshadows  no ray-traced shadows (nor the ray-query device features they need)\n"
           "  --shadowdebug  start in the shadow debug view (F10): only the city's lights on each surface\n"
           "  --city  start in this city (the first whose name begins so, any case; or its number), in its frame\n"
           "  --cam   start position (world metres, in the city's frame); a HEIGHT starts in fly mode at that altitude\n"
           "  --walk  hold W for this many seconds before a --shot\n"
           "  --turn  and turn this fast while walking (degrees a second, + to the right)\n"
           "  --walk-shot  take the --shot as the walk ends, not once streaming settles again (to catch the grass's wake)\n"
           "  --hours every street vendor open or shut whatever the hour (the board's A/B), or by the clock (the default)\n"
           "  --rain  fix the rain intensity (0 dry, 0.1 drizzle .. 1 downpour); otherwise it drifts\n"
           "  --time  hold the shaders' clock at this many seconds (flames, signs, steam: for captures)\n"
           "  --utc   start the world's clock (the sun, the moon, the stars) at this UTC instant, from which it runs at 1x;\n"
           "          otherwise it is the system's UTC. A --shot holds it, there or at " SKY_CAPTURE_UTC "\n"
           "  --ev    hold the exposure here, as a camera's EV at ISO 100 (higher is darker; %.2f is the one the night was\n"
           "          tuned at); otherwise the eye adapts to the scene (a --shot without it takes the eye's exposure for the view)\n"
           "  --preexpose  a check: write the scene this many stops brighter (darker, if negative) and take them back out\n"
           "               before the tone curve; the frame should not change, and any pass that does missed the pre-exposure\n"
           "  --hdr   show the frame on an HDR display (HDR10, else scRGB), if the window's display offers one (on Windows: its\n"
           "          HDR switch on); white paper at PAPER_NITS (default 203, BT.2408's), highlights up to PEAK_NITS (default\n"
           "          1000: the display's own). Otherwise SDR. A --shot of it saves FILE.hdr (linear, 1 = white paper) as well\n"
           "  --impostors  draw this share (default 0.4; 0 turns them off) of the office towers' floors behind curtain\n"
           "               walls and ribbon windows as baked interior impostors (docs/interior-impostors.md; F8 toggles)\n"
           "  --lockdoors  lock every lobby's automatic doors: sliding ones stay shut, revolving ones stand still\n"
           "  --kit  which street vendors set down the modelling kit's modules (NB2): none (the far shells only),\n"
           "         the noodle bars (slice, the default: bodies and cooking lines) or every vendor a line (the cost)\n"
           "  --vendor-debug  draw noodle bars' far shells magenta (shown until the near body is baked and resident, or\n"
           "               beyond 120 m) and log each vendor layer's build time; F7 toggles\n"
           "  --bars all  every vendor site a canopy noodle bar fits becomes one (NB8's gate: the worst views)\n"
           "  --nbform  build every canopy noodle bar straight or turning a corner, where it fits (NB4's captures)\n"
           "  --trade  every vendor one street-food trade (SF1's captures)\n"
           "  --nograss  draw no grass blades\n"
           "  --nowake  the grass doesn't part where the player walks\n"
           "  --grass-cap  give each of the grass's levels of detail room for at most N blades (the far ones give way)\n"
           "  --grass-debug  paint the grass's rules on the land, unlit: suit (suitability), density (blades per m^2 and\n"
           "                 the kept roots), species, clump; or colour the blades and the land by level of detail (lod)\n"
           "                 or by patch, the land by what the grass's culling made of its patch (cull); or both by\n"
           "                 the wind's strength (wind), or by the player's wake through the grass (wake)\n"
           "  --cave-debug  draw the caves unlit, to look at their meshes: their normals, or lit from the eye (lit), or that\n"
           "                tinted by level of detail (lod), through the ground (it isn't drawn), and fly mode goes under the ground.\n"
           "                Without it, fly mode goes under the ground only into a cave's air\n"
           "  --shot  wait for streaming to settle, save a screenshot and exit\n", EV100_REF);
}

/* Command-line options. */
typedef struct Options {
    bool        validation, shadows, shadowDebug, vendorDebug, lockDoors, noGrass;
    uint32_t    grassCap;        /* --grass-cap: each level of detail's room (0: its own) */
    const char *shotPath;
    const char *city;            /* --city: a name's beginning, or a number */
    bool        haveCam, camFly;
    double      cam[5];          /* x, z, yaw, pitch (degrees), height (world Y; with camFly, else walking) */
    float       walkSeconds, turnRate, fixedRain;   /* turnRate: degrees a second while walking */
    bool        walkShot;        /* --walk-shot: the shot as the walk ends */
    bool        noWake;          /* --nowake: no wake through the grass */
    double      fixedTime;       /* the shaders' clock held here (< 0: running) */
    bool        haveUtc;         /* --utc: the world's clock starts at utcJd */
    double      utcJd;
    bool        haveEv;          /* --ev: the exposure held at ev (EV100: src/core/light_units.h); otherwise the eye adapts */
    float       ev;
    float       preExpose;       /* --preexpose: the scene written this many stops off its exposure (a check) */
    float       hdrPaper, hdrPeak;   /* --hdr: white paper's nits and the display's peak (0: SDR) */
    float       impostors;       /* the share of eligible office floors drawn as impostors (0: none) */
    int         grassDebug;      /* --grass-debug: GRASS_DEBUG_* (src/grass/grass_rules.h) */
    int         caveDebug;       /* --cave-debug: renderer_set_cave_debug's view */
    int         kit;             /* --kit: KIT_OFF, KIT_SLICE, KIT_ALL (city_set_kit) */
} Options;

static bool parse_args(int argc, char **argv, Options *o) {
    memset(o, 0, sizeof(*o));
    o->shadows = true;
    o->fixedRain = -1.0f;
    o->fixedTime = -1.0;
    o->impostors = 0.4f;
    o->kit = KIT_SLICE;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--validation") == 0) o->validation = true;
        else if (strcmp(argv[i], "--noshadows") == 0) o->shadows = false;
        else if (strcmp(argv[i], "--shadowdebug") == 0) o->shadowDebug = true;
        else if (strcmp(argv[i], "--vendor-debug") == 0) o->vendorDebug = true;
        else if (strcmp(argv[i], "--hours") == 0 && i + 1 < argc) {
            const char *h = argv[++i];
            nb_hours_force(strcmp(h, "open") == 0 ? 1 : strcmp(h, "shut") == 0 ? 0 : -1);
        }
        else if (strcmp(argv[i], "--lockdoors") == 0) o->lockDoors = true;
        else if (strcmp(argv[i], "--nograss") == 0) o->noGrass = true;
        else if (strcmp(argv[i], "--kit") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "off") == 0) o->kit = KIT_OFF;
            else if (strcmp(v, "slice") == 0) o->kit = KIT_SLICE;
            else if (strcmp(v, "all") == 0) o->kit = KIT_ALL;
            else return false;
        }
        else if (strcmp(argv[i], "--tier-style") == 0 && i + 1 < argc) {   /* SF2: the tiered kiosk's upper tier */
            const int k = nb_upper_by_name(argv[++i]);
            if (k < 0) return false;
            nb_force_upper(k);
        }
        else if (strcmp(argv[i], "--kit-lod") == 0 && i + 1 < argc) city_set_kit_lod(atoi(argv[++i]));   /* SF2: 0 or 1 */
        else if (strcmp(argv[i], "--bars") == 0 && i + 1 < argc) {
            if (strcmp(argv[++i], "all") != 0) return false;
            city_force_bars(1);   /* NB8's gate: every site a canopy bar fits */
        }
        else if (strcmp(argv[i], "--trade") == 0 && i + 1 < argc) {
            const int t = vendor_trade_by_name(argv[++i]);
            if (t < 0) return false;
            vendor_force_trade(t);   /* SF1's captures: every vendor this trade */
        }
        else if (strcmp(argv[i], "--nbform") == 0 && i + 1 < argc) {
            /* NB4, SF2: every vendor this form where it fits (straight, corner, hatch, arcade, tiered) */
            const int f = nb_form_by_name(argv[++i]);
            if (f < 0) return false;
            nb_force_form(f);
        }
        else if (strcmp(argv[i], "--grass-cap") == 0 && i + 1 < argc) o->grassCap = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--grass-debug") == 0 && i + 1 < argc) {
            static const char *const VIEWS[] = { "suit", "density", "species", "clump", "lod", "cull", "wind", "wake", "albedo" };   /* GRASS_DEBUG_SUIT on */
            const char *v = argv[++i];
            for (int k = 0; k < (int)(sizeof(VIEWS) / sizeof(VIEWS[0])); ++k) if (strcmp(v, VIEWS[k]) == 0) o->grassDebug = GRASS_DEBUG_SUIT + k;
            if (o->grassDebug == GRASS_DEBUG_OFF) return false;
        }
        else if (strcmp(argv[i], "--cave-debug") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            o->caveDebug = strcmp(v, "normals") == 0 ? 1 : strcmp(v, "lit") == 0 ? 2 : strcmp(v, "lod") == 0 ? 3 : 0;
            if (!o->caveDebug) return false;
        }
        else if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) o->shotPath = argv[++i];
        else if (strcmp(argv[i], "--city") == 0 && i + 1 < argc) o->city = argv[++i];
        else if (strcmp(argv[i], "--walk") == 0 && i + 1 < argc) o->walkSeconds = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--turn") == 0 && i + 1 < argc) o->turnRate = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--walk-shot") == 0) o->walkShot = true;
        else if (strcmp(argv[i], "--nowake") == 0) o->noWake = true;
        else if (strcmp(argv[i], "--rain") == 0 && i + 1 < argc) o->fixedRain = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--time") == 0 && i + 1 < argc) o->fixedTime = atof(argv[++i]);
        else if (strcmp(argv[i], "--utc") == 0 && i + 1 < argc) {
            if (!sky_parse_utc(argv[++i], &o->utcJd)) { fprintf(stderr, "--utc %s: not a UTC instant (YYYY-MM-DDTHH:MM)\n", argv[i]); return false; }
            o->haveUtc = true;
        }
        else if (strcmp(argv[i], "--ev") == 0 && i + 1 < argc) { o->ev = (float)atof(argv[++i]); o->haveEv = true; }
        else if (strcmp(argv[i], "--preexpose") == 0 && i + 1 < argc) o->preExpose = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--hdr") == 0) {
            o->hdrPaper = 203.0f;
            o->hdrPeak = 1000.0f;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') o->hdrPaper = (float)atof(argv[++i]);
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') o->hdrPeak = (float)atof(argv[++i]);
            if (o->hdrPaper < 1.0f) return false;
        }
        else if (strcmp(argv[i], "--impostors") == 0) {
            o->impostors = 0.4f;
            if (i + 1 < argc && ((argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') || argv[i + 1][0] == '.')) o->impostors = (float)atof(argv[++i]);
        }
        else if (strcmp(argv[i], "--cam") == 0 && i + 4 < argc) {
            o->haveCam = true;
            for (int k = 0; k < 4; ++k) o->cam[k] = atof(argv[++i]);
            /* a height (fly mode), which may be below the world's zero: not an option's dash */
            if (i + 1 < argc && (argv[i + 1][0] != '-' || (argv[i + 1][1] >= '0' && argv[i + 1][1] <= '9'))) {
                o->cam[4] = atof(argv[++i]);
                o->camFly = true;
            }
        } else return false;
    }
    return true;
}

static GLFWwindow *create_window(void) {
    if (!glfwInit()) fatal("glfwInit failed");
    if (!glfwVulkanSupported()) fatal("Vulkan loader not found");
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow *win = glfwCreateWindow(1600, 900, "NeonCity", NULL, NULL);
    if (!win) fatal("could not create window");
    glfwSetFramebufferSizeCallback(win, on_resize);
    glfwSetCursorPosCallback(win, on_mouse);
    glfwSetKeyCallback(win, on_key);
    glfwSetMouseButtonCallback(win, on_click);
    set_capture(win, true);
    return win;
}

/* This frame's movement and look from the keyboard and mouse (nothing while
 * the mouse is released). Scripted captures ignore the mouse. */
static PlayerInput read_input(GLFWwindow *win, const Player *player, bool scripted) {
    PlayerInput in = { 0 };
    if (g_captured) {
        in.moveZ = (float)(glfwGetKey(win, GLFW_KEY_W) == GLFW_PRESS) - (float)(glfwGetKey(win, GLFW_KEY_S) == GLFW_PRESS);
        in.moveX = (float)(glfwGetKey(win, GLFW_KEY_D) == GLFW_PRESS) - (float)(glfwGetKey(win, GLFW_KEY_A) == GLFW_PRESS);
        in.sprint = glfwGetKey(win, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
        in.down = glfwGetKey(win, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS;
        if (!scripted) {   /* scripted captures must not pick up stray mouse movement */
            in.lookDX = g_mouseDX;
            in.lookDY = g_mouseDY;
        }
        in.toggleFly = g_flyPressed;
        /* Jump is an edge in walk mode, a held key in fly mode. */
        in.jump = player->fly ? glfwGetKey(win, GLFW_KEY_SPACE) == GLFW_PRESS : g_jumpPressed;
    }
    g_mouseDX = g_mouseDY = 0;
    g_flyPressed = g_jumpPressed = false;
    return in;
}

/* F7 vendor debug view, F8 interior impostors, F9 shadows, F10 shadow debug view, F12 screenshot. */
static void handle_toggles(Renderer *renderer, int *shotCount) {
    static float impostorShare = 0.4f;   /* what F8 turns them back on to */
    if (g_impostorPressed) {
        float now = renderer_impostors(renderer);
        if (now > 0.0f) impostorShare = now;
        renderer_set_impostors(renderer, now > 0.0f ? 0.0f : impostorShare);
        printf("Interior impostors: %s (%d modules)\n", now > 0.0f ? "off" : "on", renderer_impostor_modules(renderer));
        g_impostorPressed = false;
    }
    if (g_shadowPressed) {
        renderer_set_shadows(renderer, !renderer_shadows(renderer));
        g_shadowPressed = false;
    }
    if (g_debugPressed) {
        renderer_set_shadow_debug(renderer, !renderer_shadow_debug(renderer));
        g_debugPressed = false;
    }
    if (g_vendorDebugPressed) {
        renderer_set_vendor_debug(renderer, !renderer_vendor_debug(renderer));
        chunks_set_vendor_log(renderer_vendor_debug(renderer));
        printf("Vendor debug view: %s\n", renderer_vendor_debug(renderer) ? "on (far shells magenta; vendor layer build times logged)" : "off");
        g_vendorDebugPressed = false;
    }
    if (g_shotPressed) {
        char name[64];
        snprintf(name, sizeof(name), "neoncity_%03d.bmp", (*shotCount)++);
        renderer_request_screenshot(renderer, name);
        g_shotPressed = false;
    }
}

/* A scripted capture (--shot, --walk): walk once the initial view has
 * streamed in, then save the screenshot once streaming settles again (or
 * after a timeout, when the pool is too full for all of it to fit). */
typedef struct Capture {
    const char *path;
    float       walkSeconds, turnRate;
    int         settledFrames, framesAfterWalk;
    bool        walkStarted, shotTaken, atWalkEnd;   /* atWalkEnd: shoot as the walk ends (--walk-shot) */
    /* during the walk: in-view chunks near enough to miss (ChunkStats.inViewMissing), summed over frames, and the most */
    long        missingSum;
    int         missingFrames, missingMost;
} Capture;

static void capture_walk(Capture *c, PlayerInput *in, float dt, uint64_t totalFrames) {
    if (c->path && c->walkSeconds > 0.0f && (c->walkStarted || c->settledFrames > 30 || totalFrames > 900)) {
        c->walkStarted = true;
        in->moveZ = 1.0f;
        in->turn = c->turnRate * dt * PI_F / 180.0f;
        c->walkSeconds -= dt;
    }
}

static void capture_step(Capture *c, GLFWwindow *win, ChunkSystem *chunks, Renderer *renderer, const Player *player,
                         float emaDt, uint64_t totalFrames) {
    if (!c->path) return;
    ChunkStats s = chunks_stats(chunks);
    bool distant = renderer_proxies_settled(renderer) && renderer_terrain_settled(renderer);
    c->settledFrames = (s.queued + s.ready == 0 && distant && totalFrames > 10) ? c->settledFrames + 1 : 0;
    if (c->walkSeconds <= 0.0f) c->framesAfterWalk++;
    else if (c->walkStarted) {
        c->missingSum += s.inViewMissing;
        c->missingFrames++;
        if (s.inViewMissing > c->missingMost) c->missingMost = s.inViewMissing;
    }
    /* Capture once streaming has settled. The chunks may never settle when
     * their pool is full, so after 900 frames they are let off; the proxies
     * and the terrain still have to be ready (bar a last timeout), the
     * proxies bar the far tiles their own full pool turned away. */
    if (c->shotTaken) glfwSetWindowShouldClose(win, GLFW_TRUE);
    else if (c->walkSeconds <= 0.0f && ((c->atWalkEnd && c->walkStarted) || c->settledFrames > 60 || (c->framesAfterWalk > 900 && distant) || c->framesAfterWalk > 20000)) {
        renderer_request_screenshot(renderer, c->path);
        c->shotTaken = true;
        if (c->framesAfterWalk > 20000)
            printf("shot: timed out waiting for%s%s%s\n", s.queued + s.ready ? " chunks" : "",
                   renderer_proxies_settled(renderer) ? "" : " proxies", renderer_terrain_settled(renderer) ? "" : " terrain");
        printf("frame time %.2f ms (avg), drawn chunks %d, draws %d, resident %d, pool %d/%d pages\n",
               emaDt * 1000.0f, s.drawnChunks, s.drawCalls, s.resident, s.pagesUsed, s.pagesTotal);
        if (c->missingFrames > 0)
            printf("walk: %d frames, in-view chunks within 700 m not yet drawn: %.1f on average, %d at most\n",
                   c->missingFrames, (double)c->missingSum / c->missingFrames, c->missingMost);
        if (renderer_shadows(renderer))
            printf("shadows: %d bottom-level structures (%d MB, %d built so far), %d TLAS instances\n",
                   s.blas, s.blasMB, s.blasBuilds, s.instances);
        {
            KitStats ks = kit_stats(chunks_kit(chunks));
            printf("kit: library %d triangles (%.1f MB, built in %.0f ms); %d instances offered, drawn L0 %d L1 %d L2 %d "
                   "(%ld triangles, %d draws); %d TLAS instances over %d module BLASes\n",
                   ks.libTris, ks.libMB, ks.buildMs, ks.offered, ks.drawn[0], ks.drawn[1], ks.drawn[2], ks.tris, ks.draws,
                   ks.rtInstances, ks.blas);
        }
        char gpu[2048];
        renderer_gpu_times(renderer, gpu, sizeof(gpu));
        printf("%s\n", gpu);
        renderer_terrain_stats(renderer, gpu, sizeof(gpu));
        printf("%s\n", gpu);
        renderer_proxy_stats(renderer, gpu, sizeof(gpu));
        printf("%s\n", gpu);
        renderer_grass_stats(renderer, gpu, sizeof(gpu));
        printf("%s\n", gpu);
        layouts_stats(gpu, sizeof(gpu));
        printf("%s\n", gpu);
        printf("camera (%.1f, %.1f, %.1f) yaw %.1f pitch %.1f\n", player->eye[0], player->eye[1], player->eye[2],
               player->yaw * 180.0f / PI_F, player->pitch * 180.0f / PI_F);
        float target, metered, ev = renderer_exposure(renderer, &target, &metered);
        printf("exposure: EV100 %.2f (the eye's target %.2f; the scene meters %.2f)\n", ev, target, metered);
        float sun[4], moon[4], sky[4], level[3];
        renderer_air(renderer, sun, moon, sky, level);
        printf("air: sun %.4g %.4g %.4g (%.0f%% of its disc up), moon %.3g %.3g %.3g, all round %.3g %.3g %.3g, the sky on level ground %.3g %.3g %.3g (engine units)\n",
               sun[0], sun[1], sun[2], sun[3] * 100.0f, moon[0], moon[1], moon[2], sky[0], sky[1], sky[2], level[0], level[1], level[2]);
    }
}

/* The frame's camera, time and weather. Camera-relative rendering: the camera
 * sits at the origin and every chunk is offset by (chunk origin - camera)
 * computed in doubles. */
/* The height the haze thins above (hazeAmount in shaders/common.glsl): the
 * ground at the centre of the city whose frame is active, or out in the
 * land of the last one. */
static float haze_datum(void) {
    static float datum;
    static uint32_t known = UINT32_MAX;
    const TerrainFrame *f = terrain_frame();
    if (f->settlement >= 0 && f->id != known) {
        datum = city_settlement_ground(f->settlement);
        known = f->id;
    }
    return datum;
}

/* The sky in the camera's local frame (src/app/sky_clock.c): the sun, the
 * moon and the stars, worked out Earth-fixed for the clock's instant and
 * turned into the frame's axes at the eye, so each place on the planet has
 * its own hour. */
typedef struct LocalSky {
    TerrainDir ax, up, az;     /* the frame's axes at the eye, Earth-fixed */
    SkyState   s;
    double     moon[3];        /* towards the moon from the eye's place (its parallax), Earth-fixed */
} LocalSky;

static void local_sky(const Player *player, double jd, LocalSky *l) {
    terrain_frame_axes(terrain_frame(), player->eye[0], player->eye[2], &l->ax, &l->up, &l->az);
    sky_state(jd, &l->s);
    double up[3] = { l->up.x, l->up.y, l->up.z };
    sky_moon_topocentric(&l->s, up, l->moon);
}

static void to_local(const LocalSky *l, const double d[3], float out[3]) {
    out[0] = (float)(d[0] * l->ax.x + d[1] * l->ax.y + d[2] * l->ax.z);
    out[1] = (float)(d[0] * l->up.x + d[1] * l->up.y + d[2] * l->up.z);
    out[2] = (float)(d[0] * l->az.x + d[1] * l->az.y + d[2] * l->az.z);
}

/* frame.moon, frame.sun, frame.starsTurn (shaders/common.glsl). The moon's
 * light (moon.w, against a full moon's: MOON_FULL) follows its phase and
 * fades as it sinks into the horizon's thick air: none once it has set. */
static void sky_ubo(const LocalSky *l, FrameUBO *ubo) {
    to_local(l, l->moon, ubo->moon);
    to_local(l, l->s.sun, ubo->sun);
    float m = fminf(fmaxf((ubo->moon[1] + 0.02f) / 0.12f, 0.0f), 1.0f);   /* its altitude's sine over -0.02 .. 0.1 */
    ubo->moon[3] = (float)l->s.moonBright * m * m * (3.0f - 2.0f * m);
    ubo->sun[3] = ubo->sun[1];
    /* the stars' frame (equatorial, of date) from the local one: the frame's
     * axes, Earth-fixed, turned back by the sidereal angle */
    double cg = cos(l->s.gmst), sg = sin(l->s.gmst);
    const TerrainDir *axes[3] = { &l->ax, &l->up, &l->az };
    for (int c = 0; c < 3; ++c) {
        const TerrainDir *a = axes[c];
        ubo->starsTurn[c * 4 + 0] = (float)(a->x * cg - a->y * sg);
        ubo->starsTurn[c * 4 + 1] = (float)(a->x * sg + a->y * cg);
        ubo->starsTurn[c * 4 + 2] = (float)a->z;
        ubo->starsTurn[c * 4 + 3] = 0.0f;
    }
    ubo->starsTurn[15] = 1.0f;
}

/* One line on the console: the instant, the place, its local solar time, the
 * sun's and the moon's heights and bearings, the moon's phase. */
static void sky_report(const Player *player, double jd) {
    LocalSky l;
    local_sky(player, jd, &l);
    double up[3] = { l.up.x, l.up.y, l.up.z }, lat, lon, salt, saz, malt, maz;
    terrain_dir_latlon(l.up, &lat, &lon);
    sky_alt_az(l.s.sun, up, &salt, &saz);
    sky_alt_az(l.moon, up, &malt, &maz);
    char when[64];
    sky_format_utc(jd, when, sizeof(when));
    double h = sky_local_solar_hours(jd, lon), r2d = 180.0 / 3.14159265358979323846;
    static const char *const DAYS[7] = { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" };
    printf("sky: %s at %.3f %.3f, local solar time %s %02d:%02d; sun altitude %.1f azimuth %.1f; moon altitude %.1f azimuth %.1f, %.0f%% lit (%.2f of full)\n",
           when, lat, lon, DAYS[sky_local_weekday(jd, lon)], (int)h, (int)(h * 60.0) % 60, salt * r2d, saz * r2d, malt * r2d, maz * r2d, l.s.moonLit * 100.0, l.s.moonBright);
    fflush(stdout);
}

static void frame_ubo(const Player *player, const VkCore *vk, double t, double jd, const Weather *weather, float edge, FrameUBO *ubo) {
    float aspect = (float)vk->extent.width / (float)(vk->extent.height ? vk->extent.height : 1);
    mat4 proj = mat4_perspective_rz(player->fovY, aspect, NEAR_PLANE, FAR_PLANE);
    mat4 view = mat4_view_rot(player->right, player->up, player->fwd);
    memset(ubo, 0, sizeof(*ubo));
    ubo->viewProj = mat4_mul(proj, view);
    float ty = tanf(player->fovY * 0.5f), tx = ty * aspect;
    ubo->rayBasis = mat4_identity();
    ubo->rayBasis.m[0] = player->right.x * tx; ubo->rayBasis.m[1] = player->right.y * tx; ubo->rayBasis.m[2]  = player->right.z * tx;
    ubo->rayBasis.m[4] = player->up.x * ty;    ubo->rayBasis.m[5] = player->up.y * ty;    ubo->rayBasis.m[6]  = player->up.z * ty;
    ubo->rayBasis.m[8] = player->fwd.x;        ubo->rayBasis.m[9] = player->fwd.y;        ubo->rayBasis.m[10] = player->fwd.z;
    ubo->camPos[0] = (float)player->eye[0];
    ubo->camPos[1] = (float)player->eye[1];
    ubo->camPos[2] = (float)player->eye[2];
    ubo->camPos[3] = (float)t;
    ubo->camFwd[0] = player->fwd.x; ubo->camFwd[1] = player->fwd.y; ubo->camFwd[2] = player->fwd.z;
    ubo->camFwd[3] = VIEW_DISTANCE;
    ubo->weather[2] = haze_datum();
    ubo->weather[3] = edge;
    weather_frame(weather, player->eye, ubo);
    LocalSky sky;
    local_sky(player, jd, &sky);
    sky_ubo(&sky, ubo);
    /* the city's own clock (DN7): its local mean solar time, from the camera's
     * longitude (a settlement spans a few minutes of it), and the day there */
    double lat, lon;
    terrain_dir_latlon(sky.up, &lat, &lon);
    ubo->clock[0] = (float)sky_local_solar_hours(jd, lon);
    ubo->clock[1] = (float)sky_local_weekday(jd, lon);
    nb_hours_now(ubo->clock[0], (int)lrintf(ubo->clock[1]));   /* SF4: the vendors' hours (what the kit draws) */
    nb_hours_shown(ubo->vendorOpen);                              /* ...as shown: none changes in view (chunks.c) */
}

/* frame.exposure (shaders/common.glsl; src/core/light_units.h). The exposure
 * takes a luminance of 1.2 x 2^EV100 cd/m2 to 1: in the engine's units,
 * 2^(EV100_REF - EV100). The EV100 is --ev's, or else the eye's (auto
 * exposure: src/render/exposure_gpu.c) as last read back from the GPU, a
 * couple of frames late. The scene is written pre-exposed by it, and the
 * composite takes out any difference from the eye's exposure this frame;
 * --preexpose writes it off by that many stops more, for the composite to
 * take back out. A capture takes the eye's exposure for its view at once
 * (snap), but for a --walk's, which adapts as it goes. */
static void exposure_ubo(const Options *opt, Renderer *renderer, float dt, bool snap, FrameUBO *ubo) {
    renderer_set_exposure(renderer, opt->haveEv ? opt->ev : NAN, dt, snap);
    float ev = opt->haveEv ? opt->ev : renderer_exposure(renderer, NULL, NULL);
    float E = exp2f(EV100_REF - ev), P = E * exp2f(opt->preExpose);
    ubo->exposure[0] = P;
    ubo->exposure[1] = 1.0f / P;
    ubo->exposure[2] = E;
    ubo->exposure[3] = ev;
}

/* The player's wake for the grass's blades (shaders/grass/wake.glsl), in real
 * time: the shaders' clock may be held (--time), the trail's ages never are. */
static void wake_ubo(const GrassWake *wake, const double cam[3], double now, FrameUBO *ubo) {
    GrassWakeFrame f;
    grass_wake_frame(wake, cam, now, &f);
    memcpy(ubo->wake, f.p, sizeof(ubo->wake));
    for (int i = 0; i < GRASS_WAKE_POINTS; ++i) ubo->wakeW[i / 4][i % 4] = f.w[i];
    memcpy(ubo->wakeBox, f.box, sizeof(ubo->wakeBox));
    ubo->wakeInfo[0] = (float)f.n;
}

/* Where the chunks hand over to their proxies (edgeFog in shaders/common.glsl):
 * the view distance, or the chunk pool's reach while that is nearer, eased at
 * EDGE_SPEED so the band doesn't jump when the reach does. */
#define EDGE_SPEED 400.0f   /* m/s */
static float chunk_edge(ChunkSystem *chunks, float edge, float dt) {
    ChunkStats s = chunks_stats(chunks);
    float target = s.reach > 0.0f && s.reach < VIEW_DISTANCE ? s.reach : VIEW_DISTANCE;
    if (edge <= 0.0f) return target;
    float step = EDGE_SPEED * fminf(dt, 0.1f);
    return edge + fmaxf(-step, fminf(step, target - edge));
}

/* The window title: frame rate and the slowest frame (a hitch the average
 * hides), the --cam that reproduces the view, streaming state. */
static void update_title(GLFWwindow *win, ChunkSystem *chunks, Renderer *renderer, const Player *player, float fps, float worstMs) {
    ChunkStats s = chunks_stats(chunks);
    char reach[32] = "", city[96] = "";
    if (s.reach > 0.0f) snprintf(reach, sizeof(reach), " (to %.1f km)", s.reach / 1000.0f);
    const TerrainFrame *f = terrain_frame();
    if (f->settlement > 0) snprintf(city, sizeof(city), "--city %d (%s) ", f->settlement, plan_settlement_name(f->settlement));
    else if (f->settlement < 0) {
        double lat, lon;
        terrain_dir_latlon(terrain_world_dir(player->eye[0], player->eye[2]), &lat, &lon);
        snprintf(city, sizeof(city), "travelling at %.3f %.3f: ", lat, lon);
    }
    char title[480];
    snprintf(title, sizeof(title),
             "NeonCity | %.0f fps, worst %.0f ms | %s--cam %.1f %.1f %.0f %.0f | EV %.1f | chunk (%d,%d) | drawn %d (%d draws) | resident %d | queued %d | pool %d%%%s%s%s",
             fps, worstMs, city, player->eye[0], player->eye[2],
             player->yaw * 180.0f / PI_F, player->pitch * 180.0f / PI_F, renderer_exposure(renderer, NULL, NULL),
             (int)floor(player->feet[0] / CHUNK_SIZE), (int)floor(player->feet[2] / CHUNK_SIZE),
             s.drawnChunks, s.drawCalls, s.resident, s.queued + s.ready,
             s.pagesUsed * 100 / (s.pagesTotal ? s.pagesTotal : 1), reach, player->fly ? " | FLY" : "",
             renderer_shadows(renderer) ? "" : " | shadows off");
    glfwSetWindowTitle(win, title);
}

/* A frame over HITCH_SECONDS, on the console: the GPU's passes for the latest
 * frame it has finished (two frames behind: if they don't add up to the hitch,
 * the CPU took the time), and the streaming state. */
#define HITCH_SECONDS 0.1
static void report_hitch(ChunkSystem *chunks, Renderer *renderer, const Player *player, float dt, uint64_t frame) {
    ChunkStats s = chunks_stats(chunks);
    char gpu[400];
    renderer_gpu_last(renderer, gpu, sizeof(gpu));
    printf("hitch %.0f ms at frame %llu, --cam %.1f %.1f %.0f %.0f: drawn %d, resident %d, queued %d, pool %d%%, reach %.0f m; %s\n",
           dt * 1000.0f, (unsigned long long)frame, player->eye[0], player->eye[2], player->yaw * 180.0f / PI_F, player->pitch * 180.0f / PI_F,
           s.drawnChunks, s.resident, s.queued + s.ready, s.pagesUsed * 100 / (s.pagesTotal ? s.pagesTotal : 1), s.reach, gpu);
    fflush(stdout);
}

static void run(GLFWwindow *win, VkCore *vk, ChunkSystem *chunks, Renderer *renderer, Player *player, Frames *fr, const Options *opt) {
    double startTime = glfwGetTime(), last = startTime, titleTimer = 0, worstDt = 0;
    int frames = 0, shotCount = 0;
    uint64_t totalFrames = 0;
    float emaDt = 0.0f, edge = 0.0f;
    Capture cap = { .path = opt->shotPath, .walkSeconds = opt->walkSeconds, .turnRate = opt->turnRate, .atWalkEnd = opt->walkShot };
    Weather weather = { 0.0f, -1.0f };
    GrassWake wake;   /* the player's path through the grass (src/grass/grass_wake.h) */
    SkyClock clock;   /* the world's UTC: a capture holds it */
    sky_clock_init(&clock, opt->haveUtc ? opt->utcJd : 0.0, startTime, opt->shotPath != NULL);
    if (!opt->haveUtc) {
        if (opt->shotPath) sky_parse_utc(SKY_CAPTURE_UTC, &clock.jd0);
        else clock.jd0 = sky_jd_now();
    }
    grass_wake_reset(&wake);
    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        double now = glfwGetTime();
        float dt = (float)(now - last);
        last = now;
        emaDt = emaDt == 0.0f ? dt : emaDt * 0.95f + dt * 0.05f;
        if (dt > HITCH_SECONDS && totalFrames > 60) report_hitch(chunks, renderer, player, dt, totalFrames);

        PlayerInput in = read_input(win, player, opt->shotPath != NULL);
        capture_walk(&cap, &in, dt, totalFrames);
        player_update(player, &in, dt);
        if (!opt->noWake) grass_wake_step(&wake, player->feet, !player->fly, now - startTime);
        double shaderTime = opt->fixedTime >= 0.0 ? opt->fixedTime : now - startTime;
        lobby_doors_update(player, shaderTime, dt);
        double t0 = glfwGetTime();
        if (frames_update(fr, player)) {   /* into another frame (renderer_set_frame says what stands) */
            renderer_set_frame(renderer, fr->active, player->eye);
            char what[160];
            frames_describe(fr, what, sizeof(what));
            printf("frame: %s (the change took %.1f ms) at --cam %.0f %.0f\n", what, (glfwGetTime() - t0) * 1000.0, player->eye[0], player->eye[2]);
            fflush(stdout);
        }
        renderer_frame_ahead(renderer, fr->ahead, player->eye);
        handle_toggles(renderer, &shotCount);
        capture_step(&cap, win, chunks, renderer, player, emaDt, totalFrames);
        totalFrames++;

        weather_update(&weather, now - startTime, dt, opt->fixedRain);
        FrameUBO ubo;
        edge = chunk_edge(chunks, edge, dt);
        double jd = sky_clock_jd(&clock, now);
        if (totalFrames == 1) sky_report(player, jd);
        frame_ubo(player, vk, shaderTime, jd, &weather, edge, &ubo);
        uint32_t keys[DOOR_SLOTS];
        ubo.doorInfo[0] = (uint32_t)lobby_doors_frame(keys, ubo.doorState);
        for (uint32_t k = 0; k < ubo.doorInfo[0]; ++k) ubo.doorKey[k / 4][k % 4] = keys[k];
        wake_ubo(&wake, player->eye, now - startTime, &ubo);
        exposure_ubo(opt, renderer, dt, opt->shotPath && !cap.walkStarted, &ubo);
        bool resized = g_resized;
        g_resized = false;
        renderer_frame(renderer, &ubo, player->eye, resized);

        frames++;
        titleTimer += dt;
        if (dt > worstDt) worstDt = dt;
        if (titleTimer > 0.5) {
            update_title(win, chunks, renderer, player, (float)(frames / titleTimer), (float)(worstDt * 1000.0));
            titleTimer = 0;
            worstDt = 0;
            frames = 0;
        }
    }
}

/* --city's settlement: its number, or the first whose name begins with s
 * (any case). Exits when there is none. */
static int find_city(const char *s) {
    char *end;
    long k = strtol(s, &end, 10);
    if (*s && !*end) {
        if (k >= 0 && k < plan_settlement_count()) return (int)k;
    } else {
        size_t n = strlen(s);
        for (int q = 0; q < plan_settlement_count(); ++q)
            if (_strnicmp(plan_settlement_name(q), s, n) == 0) return q;
    }
    fprintf(stderr, "--city %s: no such city (world_test --list lists them)\n", s);
    exit(1);
}

int main(int argc, char **argv) {
    Options opt;
    if (!parse_args(argc, argv, &opt)) { usage(); return 1; }
    terrain_earth_load(NULL);       /* the Earth's bake, if built, before the plan reads the ground */
    plan_world_init(cpu_count());   /* the settlements and roads, before any worker asks for them */
    {   /* every settlement's ground, and the highest of it, which the deck hangs over (weather.c, mesh_emit.c) */
        const clock_t t0 = clock();
        city_settlement_ground(0);
        city_settlement_deck(0);
        int top = 0, risen = 0;
        for (int k = 0; k < plan_settlement_count(); ++k) risen += city_settlement_deck(k) - city_settlement_ground(k) > CLOUD_LOW;
        for (int k = 1; k < plan_settlement_count(); ++k)
            if (city_settlement_deck(k) - city_settlement_ground(k) > city_settlement_deck(top) - city_settlement_ground(top)) top = k;
        printf("decks: %d settlements' highest ground in %.0f ms, %d of them over CLOUD_LOW above their core, the most %.0f m (%s)\n",
               plan_settlement_count(), 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC, risen, city_settlement_deck(top) - city_settlement_ground(top), plan_settlement_name(top));
    }
    models_load(NULL);              /* the street furniture's and lobbies' models (assets/models), before any chunk */
    Frames frames;
    frames_init(&frames, opt.city ? find_city(opt.city) : 0);   /* the frame the camera starts in, before anything is built */
    GLFWwindow *win = create_window();

    VkCore vk;
    vk_init(&vk, win, opt.validation, opt.shadows, opt.hdrPaper, opt.hdrPeak);
    city_set_kit(opt.kit);         /* before any chunk */
    ChunkSystem *chunks = chunks_create(&vk);
    Renderer *renderer = renderer_create(&vk, chunks);
    vk_pipeline_cache_save(&vk);   /* the pipelines it made, for the next run */
    renderer_set_shadow_debug(renderer, opt.shadowDebug);
    renderer_set_vendor_debug(renderer, opt.vendorDebug);
    chunks_set_vendor_log(opt.vendorDebug);
    renderer_set_grass_debug(renderer, opt.grassDebug);
    renderer_set_cave_debug(renderer, opt.caveDebug);
    renderer_set_grass(renderer, !opt.noGrass);
    renderer_set_grass_cap(renderer, opt.grassCap);
    renderer_set_impostors(renderer, opt.impostors);
    lobby_doors_lock_all(opt.lockDoors);

    Player player;
    player_init(&player);
    if (opt.haveCam) {
        player.feet[0] = opt.cam[0];
        player.feet[2] = opt.cam[1];
        player.yaw = (float)(opt.cam[2] * PI_F / 180.0);
        player.pitch = (float)(opt.cam[3] * PI_F / 180.0);
        if (opt.camFly) { player.fly = true; player.feet[1] = opt.cam[4]; }
    }
    player.noclip = opt.caveDebug != 0;   /* the caves seen through the ground, from anywhere */

    run(win, &vk, chunks, renderer, &player, &frames, &opt);
    layouts_shutdown();

    vkDeviceWaitIdle(vk.device);
    /* The driver takes 12-15 s over vkDestroyDevice (RTX 4080 SUPER, 2026-11-12: with every object
     * already destroyed, ray tracing or not), where ending the process frees it all at once. The
     * screenshot and the pipeline cache are written by now. The full teardown is kept for
     * --validation, whose layer reports what is left undestroyed. */
    if (!opt.validation) {
        fflush(NULL);
        _exit(0);
    }
    renderer_destroy(renderer);
    chunks_destroy(chunks);
    frames_shutdown(&frames);
    vk_shutdown(&vk);
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
