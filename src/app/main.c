/* NeonCity - a vast, deterministic, cyberpunk city walk in C + Vulkan.
 *
 * Controls: mouse look, WASD move, Shift sprint, Space jump,
 *           F toggle fly (debug: Space/Ctrl up/down), F9 toggle shadows, F10 shadow debug view,
 *           F11 fullscreen, Esc release mouse (Esc again to quit), click to recapture. */
#include "vkcore.h"
#include "renderer.h"
#include "chunks.h"
#include "player.h"
#include "city.h"

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

/* Weather: steady rain whose intensity drifts between drizzle and downpour
 * over minutes, never quite stopping. The streets wet quickly as it
 * strengthens and dry slowly as it eases. fixedRain >= 0 pins the intensity. */
typedef struct Weather { float rain, wet; } Weather;

static void weather_update(Weather *w, double t, float dt, float fixedRain) {
    float rain = fixedRain >= 0.0f ? fixedRain
               : (float)(0.6 + 0.25 * sin(t / 53.0) + 0.15 * sin(t / 31.0 + 1.7));
    float target = 0.55f + 0.45f * rain;
    if (w->wet < 0.0f) w->wet = target;   /* first frame: it has already been raining for a while */
    float tau = target > w->wet ? 15.0f : 120.0f;
    w->wet += (target - w->wet) * (1.0f - expf(-dt / tau));
    w->rain = rain;
}

static void usage(void) {
    printf("NeonCity [--validation] [--noshadows] [--cam X Z YAW_DEG PITCH_DEG [HEIGHT]] [--walk SECONDS] [--rain 0..1] [--shot FILE.bmp]\n"
           "  --noshadows  no ray-traced shadows (nor the ray-query device features they need)\n"
           "  --shadowdebug  start in the shadow debug view (F10): only the city's lights on each surface\n"
           "  --cam   start position (world metres); a HEIGHT starts in fly mode at that altitude\n"
           "  --walk  hold W for this many seconds before a --shot\n"
           "  --rain  fix the rain intensity (0 drizzle .. 1 downpour); otherwise it drifts\n"
           "  --shot  wait for streaming to settle, save a screenshot and exit\n");
}

/* Command-line options. */
typedef struct Options {
    bool        validation, shadows, shadowDebug;
    const char *shotPath;
    bool        haveCam;
    double      cam[5];          /* x, z, yaw, pitch (degrees), height (< 0: walking) */
    float       walkSeconds, fixedRain;
} Options;

static bool parse_args(int argc, char **argv, Options *o) {
    memset(o, 0, sizeof(*o));
    o->shadows = true;
    o->cam[4] = -1;
    o->fixedRain = -1.0f;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--validation") == 0) o->validation = true;
        else if (strcmp(argv[i], "--noshadows") == 0) o->shadows = false;
        else if (strcmp(argv[i], "--shadowdebug") == 0) o->shadowDebug = true;
        else if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) o->shotPath = argv[++i];
        else if (strcmp(argv[i], "--walk") == 0 && i + 1 < argc) o->walkSeconds = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--rain") == 0 && i + 1 < argc) o->fixedRain = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--cam") == 0 && i + 4 < argc) {
            o->haveCam = true;
            for (int k = 0; k < 4; ++k) o->cam[k] = atof(argv[++i]);
            if (i + 1 < argc && argv[i + 1][0] != '-') o->cam[4] = atof(argv[++i]);
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
    float       walkSeconds;
    int         settledFrames, framesAfterWalk;
    bool        walkStarted, shotTaken;
} Capture;

static void capture_walk(Capture *c, PlayerInput *in, float dt, uint64_t totalFrames) {
    if (c->path && c->walkSeconds > 0.0f && (c->walkStarted || c->settledFrames > 30 || totalFrames > 900)) {
        c->walkStarted = true;
        in->moveZ = 1.0f;
        c->walkSeconds -= dt;
    }
}

static void capture_step(Capture *c, GLFWwindow *win, ChunkSystem *chunks, Renderer *renderer, const Player *player,
                         float emaDt, uint64_t totalFrames) {
    if (!c->path) return;
    ChunkStats s = chunks_stats(chunks);
    c->settledFrames = (s.queued + s.ready == 0 && renderer_proxies_ready(renderer) && totalFrames > 10) ? c->settledFrames + 1 : 0;
    if (c->walkSeconds <= 0.0f) c->framesAfterWalk++;
    /* Capture once streaming has settled (or after a timeout under memory pressure). */
    if (c->shotTaken) glfwSetWindowShouldClose(win, GLFW_TRUE);
    else if (c->walkSeconds <= 0.0f && (c->settledFrames > 60 || c->framesAfterWalk > 900)) {
        renderer_request_screenshot(renderer, c->path);
        c->shotTaken = true;
        printf("frame time %.2f ms (avg), drawn chunks %d, draws %d, resident %d, pool %d/%d pages\n",
               emaDt * 1000.0f, s.drawnChunks, s.drawCalls, s.resident, s.pagesUsed, s.pagesTotal);
        if (renderer_shadows(renderer))
            printf("shadows: %d bottom-level structures (%d MB, %d built so far), %d TLAS instances\n",
                   s.blas, s.blasMB, s.blasBuilds, s.instances);
        char gpu[256];
        renderer_gpu_times(renderer, gpu, sizeof(gpu));
        printf("%s\n", gpu);
        renderer_terrain_stats(renderer, gpu, sizeof(gpu));
        printf("%s\n", gpu);
        renderer_proxy_stats(renderer, gpu, sizeof(gpu));
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

static void frame_ubo(const Player *player, const VkCore *vk, double t, const Weather *weather, FrameUBO *ubo) {
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
    ubo->weather[0] = weather->rain;
    ubo->weather[1] = weather->wet;
    ubo->weather[2] = haze_datum();
}

/* The window title: frame rate, the --cam that reproduces the view, streaming state. */
static void update_title(GLFWwindow *win, ChunkSystem *chunks, Renderer *renderer, const Player *player, float fps) {
    ChunkStats s = chunks_stats(chunks);
    char title[320];
    snprintf(title, sizeof(title),
             "NeonCity | %.0f fps | --cam %.1f %.1f %.0f %.0f | chunk (%d,%d) | drawn %d (%d draws) | resident %d | queued %d | pool %d%%%s%s",
             fps, player->eye[0], player->eye[2],
             player->yaw * 180.0f / PI_F, player->pitch * 180.0f / PI_F,
             (int)floor(player->feet[0] / CHUNK_SIZE), (int)floor(player->feet[2] / CHUNK_SIZE),
             s.drawnChunks, s.drawCalls, s.resident, s.queued + s.ready,
             s.pagesUsed * 100 / (s.pagesTotal ? s.pagesTotal : 1), player->fly ? " | FLY" : "",
             renderer_shadows(renderer) ? "" : " | shadows off");
    glfwSetWindowTitle(win, title);
}

static void run(GLFWwindow *win, VkCore *vk, ChunkSystem *chunks, Renderer *renderer, Player *player, const Options *opt) {
    double startTime = glfwGetTime(), last = startTime, titleTimer = 0;
    int frames = 0, shotCount = 0;
    uint64_t totalFrames = 0;
    float emaDt = 0.0f;
    Capture cap = { opt->shotPath, opt->walkSeconds, 0, 0, false, false };
    Weather weather = { 0.0f, -1.0f };
    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        double now = glfwGetTime();
        float dt = (float)(now - last);
        last = now;
        emaDt = emaDt == 0.0f ? dt : emaDt * 0.95f + dt * 0.05f;

        PlayerInput in = read_input(win, player, opt->shotPath != NULL);
        capture_walk(&cap, &in, dt, totalFrames);
        player_update(player, &in, dt);
        handle_toggles(renderer, &shotCount);
        capture_step(&cap, win, chunks, renderer, player, emaDt, totalFrames);
        totalFrames++;

        weather_update(&weather, now - startTime, dt, opt->fixedRain);
        FrameUBO ubo;
        frame_ubo(player, vk, now - startTime, &weather, &ubo);
        bool resized = g_resized;
        g_resized = false;
        renderer_frame(renderer, &ubo, player->eye, resized);

        frames++;
        titleTimer += dt;
        if (titleTimer > 0.5) {
            update_title(win, chunks, renderer, player, (float)(frames / titleTimer));
            titleTimer = 0;
            frames = 0;
        }
    }
}

int main(int argc, char **argv) {
    Options opt;
    if (!parse_args(argc, argv, &opt)) { usage(); return 1; }
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
        if (opt.cam[4] >= 0) { player.fly = true; player.feet[1] = opt.cam[4]; }
    }

    run(win, &vk, chunks, renderer, &player, &opt);

    vkDeviceWaitIdle(vk.device);
    renderer_destroy(renderer);
    chunks_destroy(chunks);
    vk_shutdown(&vk);
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
