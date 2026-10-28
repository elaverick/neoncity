/* NeonCity - a vast, deterministic, cyberpunk city walk in C + Vulkan.
 *
 * Controls: mouse look, WASD move, Shift sprint, Space jump,
 *           F toggle fly (debug: Space/Ctrl up/down), F9 toggle shadows, F10 shadow debug view,
 *           F11 fullscreen, Esc release mouse (Esc again to quit), click to recapture. */
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

static bool   g_resized;
static bool   g_captured = true;
static double g_lastMX, g_lastMY;
static bool   g_haveMouse;
static float  g_mouseDX, g_mouseDY;
static bool   g_flyPressed, g_jumpPressed, g_shotPressed, g_shadowPressed, g_debugPressed;
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
    if (key == GLFW_KEY_F9) g_shadowPressed = true;
    if (key == GLFW_KEY_F10) g_debugPressed = true;
    if (key == GLFW_KEY_F11) toggle_fullscreen(w);
    if (key == GLFW_KEY_F12) g_shotPressed = true;
}

static void on_click(GLFWwindow *w, int button, int action, int mods) {
    if (action == GLFW_PRESS && !g_captured) set_capture(w, true);
}

static void usage(void) {
    printf("NeonCity [--validation] [--noshadows] [--cam X Z YAW_DEG PITCH_DEG [HEIGHT]] [--walk SECONDS [--turn DEG_PER_S]] [--rain 0..1] [--time SECONDS] [--shot FILE.bmp]\n"
           "  --noshadows  no ray-traced shadows (nor the ray-query device features they need)\n"
           "  --shadowdebug  start in the shadow debug view (F10): only the city's lights on each surface\n"
           "  --cam   start position (world metres); a HEIGHT starts in fly mode at that altitude\n"
           "  --walk  hold W for this many seconds before a --shot\n"
           "  --turn  and turn this fast while walking (degrees a second, + to the right)\n"
           "  --rain  fix the rain intensity (0 drizzle .. 1 downpour); otherwise it drifts\n"
           "  --time  hold the shaders' clock at this many seconds (flames, signs, steam: for captures)\n"
           "  --shot  wait for streaming to settle, save a screenshot and exit\n");
}

/* Command-line options. */
typedef struct Options {
    bool        validation, shadows, shadowDebug;
    const char *shotPath;
    bool        haveCam, camFly;
    double      cam[5];          /* x, z, yaw, pitch (degrees), height (world Y; with camFly, else walking) */
    float       walkSeconds, turnRate, fixedRain;   /* turnRate: degrees a second while walking */
    double      fixedTime;       /* the shaders' clock held here (< 0: running) */
} Options;

static bool parse_args(int argc, char **argv, Options *o) {
    memset(o, 0, sizeof(*o));
    o->shadows = true;
    o->fixedRain = -1.0f;
    o->fixedTime = -1.0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--validation") == 0) o->validation = true;
        else if (strcmp(argv[i], "--noshadows") == 0) o->shadows = false;
        else if (strcmp(argv[i], "--shadowdebug") == 0) o->shadowDebug = true;
        else if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) o->shotPath = argv[++i];
        else if (strcmp(argv[i], "--walk") == 0 && i + 1 < argc) o->walkSeconds = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--turn") == 0 && i + 1 < argc) o->turnRate = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--rain") == 0 && i + 1 < argc) o->fixedRain = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--time") == 0 && i + 1 < argc) o->fixedTime = atof(argv[++i]);
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

/* F9 shadows, F10 shadow debug view, F12 screenshot. */
static void handle_toggles(Renderer *renderer, int *shotCount) {
    if (g_shadowPressed) {
        renderer_set_shadows(renderer, !renderer_shadows(renderer));
        g_shadowPressed = false;
    }
    if (g_debugPressed) {
        renderer_set_shadow_debug(renderer, !renderer_shadow_debug(renderer));
        g_debugPressed = false;
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
    bool        walkStarted, shotTaken;
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
    bool distant = renderer_proxies_ready(renderer) && renderer_terrain_settled(renderer);
    c->settledFrames = (s.queued + s.ready == 0 && distant && totalFrames > 10) ? c->settledFrames + 1 : 0;
    if (c->walkSeconds <= 0.0f) c->framesAfterWalk++;
    else if (c->walkStarted) {
        c->missingSum += s.inViewMissing;
        c->missingFrames++;
        if (s.inViewMissing > c->missingMost) c->missingMost = s.inViewMissing;
    }
    /* Capture once streaming has settled. The chunks may never settle when
     * their pool is full, so after 900 frames they are let off; the proxies
     * and the terrain still have to be ready (bar a last timeout). */
    if (c->shotTaken) glfwSetWindowShouldClose(win, GLFW_TRUE);
    else if (c->walkSeconds <= 0.0f && (c->settledFrames > 60 || (c->framesAfterWalk > 900 && distant) || c->framesAfterWalk > 20000)) {
        renderer_request_screenshot(renderer, c->path);
        c->shotTaken = true;
        printf("frame time %.2f ms (avg), drawn chunks %d, draws %d, resident %d, pool %d/%d pages\n",
               emaDt * 1000.0f, s.drawnChunks, s.drawCalls, s.resident, s.pagesUsed, s.pagesTotal);
        if (c->missingFrames > 0)
            printf("walk: %d frames, in-view chunks within 700 m not yet drawn: %.1f on average, %d at most\n",
                   c->missingFrames, (double)c->missingSum / c->missingFrames, c->missingMost);
        if (renderer_shadows(renderer))
            printf("shadows: %d bottom-level structures (%d MB, %d built so far), %d TLAS instances\n",
                   s.blas, s.blasMB, s.blasBuilds, s.instances);
        char gpu[1024];
        renderer_gpu_times(renderer, gpu, sizeof(gpu));
        printf("%s\n", gpu);
        renderer_terrain_stats(renderer, gpu, sizeof(gpu));
        printf("%s\n", gpu);
        renderer_proxy_stats(renderer, gpu, sizeof(gpu));
        printf("%s\n", gpu);
        layouts_stats(gpu, sizeof(gpu));
        printf("%s\n", gpu);
        printf("camera (%.1f, %.1f, %.1f) yaw %.1f pitch %.1f\n", player->eye[0], player->eye[1], player->eye[2],
               player->yaw * 180.0f / PI_F, player->pitch * 180.0f / PI_F);
    }
}

/* The frame's camera, time and weather. Camera-relative rendering: the camera
 * sits at the origin and every chunk is offset by (chunk origin - camera)
 * computed in doubles. */
/* The height the haze thins above (hazeAmount in shaders/common.glsl): the
 * city's ground at its centre. There is one settlement for now; with more,
 * this becomes the ground of the one the camera is in or nearest. */
static float haze_datum(void) {
    static float datum;
    static bool known;
    if (!known) {
        DVec2 c = plan_town_centre(0, 0);
        datum = city_ground_y(c.x, c.z);
        known = true;
    }
    return datum;
}

static void frame_ubo(const Player *player, const VkCore *vk, double t, const Weather *weather, float edge, FrameUBO *ubo) {
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
    char reach[32] = "";
    if (s.reach > 0.0f) snprintf(reach, sizeof(reach), " (to %.1f km)", s.reach / 1000.0f);
    char title[360];
    snprintf(title, sizeof(title),
             "NeonCity | %.0f fps, worst %.0f ms | --cam %.1f %.1f %.0f %.0f | chunk (%d,%d) | drawn %d (%d draws) | resident %d | queued %d | pool %d%%%s%s%s",
             fps, worstMs, player->eye[0], player->eye[2],
             player->yaw * 180.0f / PI_F, player->pitch * 180.0f / PI_F,
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

static void run(GLFWwindow *win, VkCore *vk, ChunkSystem *chunks, Renderer *renderer, Player *player, const Options *opt) {
    double startTime = glfwGetTime(), last = startTime, titleTimer = 0, worstDt = 0;
    int frames = 0, shotCount = 0;
    uint64_t totalFrames = 0;
    float emaDt = 0.0f, edge = 0.0f;
    Capture cap = { .path = opt->shotPath, .walkSeconds = opt->walkSeconds, .turnRate = opt->turnRate };
    Weather weather = { 0.0f, -1.0f };
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
        handle_toggles(renderer, &shotCount);
        capture_step(&cap, win, chunks, renderer, player, emaDt, totalFrames);
        totalFrames++;

        weather_update(&weather, now - startTime, dt, opt->fixedRain);
        FrameUBO ubo;
        edge = chunk_edge(chunks, edge, dt);
        frame_ubo(player, vk, opt->fixedTime >= 0.0 ? opt->fixedTime : now - startTime, &weather, edge, &ubo);
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

int main(int argc, char **argv) {
    Options opt;
    if (!parse_args(argc, argv, &opt)) { usage(); return 1; }
    terrain_earth_load(NULL);       /* the Earth's bake, if built, before the plan reads the ground */
    plan_world_init(cpu_count());   /* the settlements and roads, before any worker asks for them */
    models_load(NULL);              /* the street furniture's and lobbies' models (assets/models), before any chunk */
    GLFWwindow *win = create_window();

    VkCore vk;
    vk_init(&vk, win, opt.validation, opt.shadows);
    ChunkSystem *chunks = chunks_create(&vk);
    Renderer *renderer = renderer_create(&vk, chunks);
    renderer_set_shadow_debug(renderer, opt.shadowDebug);

    Player player;
    player_init(&player);
    if (opt.haveCam) {
        player.feet[0] = opt.cam[0];
        player.feet[2] = opt.cam[1];
        player.yaw = (float)(opt.cam[2] * PI_F / 180.0);
        player.pitch = (float)(opt.cam[3] * PI_F / 180.0);
        if (opt.camFly) { player.fly = true; player.feet[1] = opt.cam[4]; }
    }

    run(win, &vk, chunks, renderer, &player, &opt);
    layouts_shutdown();

    vkDeviceWaitIdle(vk.device);
    renderer_destroy(renderer);
    chunks_destroy(chunks);
    vk_shutdown(&vk);
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
