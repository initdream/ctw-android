#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "so_util.h"
#include "egl_shim.h"
#include "jni_shim.h"
#include "imports.h"
#include "music_player.h"
#include "movie_player.h"

#define CXX_DONOR_SO "libc++_shared.so"
#define GAME_SO      "libGame.so"
#define HEAP_MB      256

so_module donor_mod;
so_module game_mod;

#define LOG_STEP(fmt, ...) do { fprintf(stderr, "[step] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)

static void crash_handler(int sig, siginfo_t *info, void *uc) {
    ucontext_t *u = (ucontext_t *)uc;
#if defined(__aarch64__)
    uintptr_t pc = u->uc_mcontext.pc;
#elif defined(__x86_64__)
    uintptr_t pc = u->uc_mcontext.gregs[REG_RIP];
#else
    uintptr_t pc = 0;
#endif
    fprintf(stderr, "\n=== CRASH sig=%d addr=%p PC=%p ===\n", sig, info->si_addr, (void *)pc);
    fflush(stderr);

    if (game_mod.load_virtbase) {
        uintptr_t g_start = (uintptr_t)game_mod.load_virtbase;
        uintptr_t g_end = g_start + game_mod.load_size;
        if (pc >= g_start && pc < g_end) {
            char *sym_name = so_find_nearest_symbol(&game_mod, pc);
            fprintf(stderr, "  Crash PC is inside libGame.so + 0x%lx (%s)\n", (unsigned long)(pc - g_start), sym_name ? sym_name : "?");
            fflush(stderr);
        }
        if ((uintptr_t)info->si_addr >= g_start && (uintptr_t)info->si_addr < g_end) {
            fprintf(stderr, "  Fault addr is inside libGame.so + 0x%lx\n", (unsigned long)((uintptr_t)info->si_addr - g_start));
            fflush(stderr);
        }
    }

    if (pc) {
        uint32_t *p = (uint32_t *)pc;
        fprintf(stderr, "  ARM64 Opcodes at PC: [0]=0x%08x [1]=0x%08x [2]=0x%08x [3]=0x%08x\n", p[0], p[1], p[2], p[3]);
        fflush(stderr);
    }

    void *bt[32];
    int n = backtrace(bt, 32);
    char **strings = backtrace_symbols(bt, n);
    if (strings) {
        for (int i = 0; i < n; i++) {
            fprintf(stderr, "  [%2d] %s\n", i, strings[i]);
        }
        free(strings);
    }
    fflush(stderr);
    _exit(128 + sig);
}

static void install_crash_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

static void preload_device_libs(void) {
    static const char *libs[] = {
        "libSDL2-2.0.so.0", "libGLESv2.so", "libEGL.so", "libopenal.so.1",
        "libmpg123.so.0", "libm.so.6", "libc.so.6", "libpthread.so.0", NULL,
    };
    for (int i = 0; libs[i]; i++) {
        void *h = dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL);
        fprintf(stderr, "preload: %s %s\n", libs[i], h ? "OK" : dlerror());
    }
}

static void check_data(void) {
    struct stat st;
    if (stat(GAME_SO, &st) < 0) {
        fprintf(stderr, "Missing %s\n", GAME_SO);
        exit(1);
    }
    if (stat(CXX_DONOR_SO, &st) < 0) {
        fprintf(stderr, "Missing %s (C++ runtime donor)\n", CXX_DONOR_SO);
        exit(1);
    }
    if (stat("assets/game.pak", &st) < 0) {
        fprintf(stderr, "Missing game data files!\n");
        exit(1);
    }
}

static void (* implOnActivityCreated)(void *env, void *cls, void *services, int isFirstRun);
static void (* implOnActivityDestroyed)(void *env, void *cls);
static void (* implOnInitialSetup)(void *env, void *cls, void *deviceInfo, void *assetMgr, void *paths, void *args);
static void (* implOnSurfaceCreated)(void *env, void *cls);
static void (* implOnSurfaceChanged)(void *env, void *cls, void *surface, int w, int h);
static void (* implOnSurfaceDestroyed)(void *env, void *cls);
static void (* implOnGameResume)(void *env, void *cls);
static void (* implOnGamePause)(void *env, void *cls);
static void (* implOnDrawFrame)(void *env, void *cls, float dt);
static void (* implOnGamepadConnected)(void *env, void *cls, int id);
static void (* implOnGamepadDisconnected)(void *env, void *cls, int id);
static void (* implOnGamepadResume)(void *env, void *cls, void *ids);
static void (* implOnGamepadButtonDown)(void *env, void *cls, int id, int keycode);
static void (* implOnGamepadButtonUp)(void *env, void *cls, int id, int keycode);
static void (* implOnGamepadAxesChanged)(void *env, void *cls, int id, float lx, float ly, float rx, float ry, float lt, float rt);
static void (* implOnTouchStart)(void *env, void *cls, int id, float x, float y);
static void (* implOnTouchMove)(void *env, void *cls, int id, float x, float y);
static void (* implOnTouchEnd)(void *env, void *cls, int id, float x, float y);
static void (* implOnNetworkChanged)(void *env, void *cls, int type);
static void (* implOnPlaylistOpenComplete)(void *env, void *cls, int success, int count);
static void (* implOnRockstarCloudDisabledComplete)(void *env, void *cls);
static void (* implOnRockstarGateComplete)(void *env, void *cls, int accepted);
static void (* implOnRockstarIdChanged)(void *env, void *cls, void *id);
static void (* implOnRockstarInitialComplete)(void *env, void *cls);
static void (* implOnRockstarSetup)(void *env, void *cls, void *environment, void *title_id);
static void (* implOnRockstarSignInComplete)(void *env, void *cls);
static void (* implOnRockstarSignOutComplete)(void *env, void *cls);
static void (* implOnRockstarStateChanged)(void *env, void *cls, int signed_in);
static void (* implOnRockstarTicketChanged)(void *env, void *cls, void *ticket);
static int  (* implIsInitialized)(void *env, void *cls);

static void resolve_entry_points(void) {
    #define ENTRY(var, sym) var = (void *)so_find_addr_rx(&game_mod, "Java_com_rockstargames_oswrapper_GameNative_" sym)
    ENTRY(implOnActivityCreated, "implOnActivityCreated");
    ENTRY(implOnActivityDestroyed, "implOnActivityDestroyed");
    ENTRY(implOnInitialSetup, "implOnInitialSetup");
    ENTRY(implOnSurfaceCreated, "implOnSurfaceCreated");
    ENTRY(implOnSurfaceChanged, "implOnSurfaceChanged");
    ENTRY(implOnSurfaceDestroyed, "implOnSurfaceDestroyed");
    ENTRY(implOnGameResume, "implOnGameResume");
    ENTRY(implOnGamePause, "implOnGamePause");
    ENTRY(implOnDrawFrame, "implOnDrawFrame");
    ENTRY(implOnGamepadConnected, "implOnGamepadConnected");
    ENTRY(implOnGamepadDisconnected, "implOnGamepadDisconnected");
    ENTRY(implOnGamepadResume, "implOnGamepadResume");
    ENTRY(implOnGamepadButtonDown, "implOnGamepadButtonDown");
    ENTRY(implOnGamepadButtonUp, "implOnGamepadButtonUp");
    ENTRY(implOnGamepadAxesChanged, "implOnGamepadAxesChanged");
    ENTRY(implOnTouchStart, "implOnTouchStart");
    ENTRY(implOnTouchMove, "implOnTouchMove");
    ENTRY(implOnTouchEnd, "implOnTouchEnd");
    ENTRY(implOnNetworkChanged, "implOnNetworkChanged");
    ENTRY(implOnPlaylistOpenComplete, "implOnPlaylistOpenComplete");
    ENTRY(implOnRockstarCloudDisabledComplete, "implOnRockstarCloudDisabledComplete");
    ENTRY(implOnRockstarGateComplete, "implOnRockstarGateComplete");
    ENTRY(implOnRockstarIdChanged, "implOnRockstarIdChanged");
    ENTRY(implOnRockstarInitialComplete, "implOnRockstarInitialComplete");
    ENTRY(implOnRockstarSetup, "implOnRockstarSetup");
    ENTRY(implOnRockstarSignInComplete, "implOnRockstarSignInComplete");
    ENTRY(implOnRockstarSignOutComplete, "implOnRockstarSignOutComplete");
    ENTRY(implOnRockstarStateChanged, "implOnRockstarStateChanged");
    ENTRY(implOnRockstarTicketChanged, "implOnRockstarTicketChanged");
    ENTRY(implIsInitialized, "implIsInitialized");
    #undef ENTRY
}

extern void *get_fallback_decodeStringRef(void *thiz, unsigned int ref);

static void patch_game(void) {
    uintptr_t a;
    #define HOOK(sym, dst) \
        if ((a = so_find_addr_safe(&game_mod, sym))) { \
            fprintf(stderr, "[patch] Hooking %s at %p -> %p\n", sym, (void *)a, (void *)(dst)); \
            fflush(stderr); \
            hook_arm64(a, (uintptr_t)(dst)); \
        } else { \
            fprintf(stderr, "[patch] WARN: symbol %s NOT FOUND!\n", sym); \
            fflush(stderr); \
        }

    HOOK("_Z24NVThreadGetCurrentJNIEnvv", NVThreadGetCurrentJNIEnv);
    HOOK("_Z22NVThreadSpawnJNIThreadPlPK14pthread_attr_tPKcPFPvS5_ES5_", NVThreadSpawnJNIThread);
    HOOK("_Z15OS_ThreadLaunchPFjPvES_jPKci16OSThreadPriority", OS_ThreadLaunch);
    HOOK("_Z13OS_ThreadWaitPv", OS_ThreadWait);
    HOOK("_Z14OS_ThreadClosePv", OS_ThreadClose);
    HOOK("_Z18OS_ThreadIsRunningPv", OS_ThreadIsRunning);
    HOOK("_Z17OS_ScreenGetWidthv", OS_ScreenGetWidth);
    HOOK("_Z18OS_ScreenGetHeightv", OS_ScreenGetHeight);
    HOOK("_ZNK14actionTreeType15decodeStringRefEj", get_fallback_decodeStringRef);
    #undef HOOK
}

#define GPAD_A 0
#define GPAD_B 1
#define GPAD_X 2
#define GPAD_Y 3
#define GPAD_START 4
#define GPAD_SELECT 5
#define GPAD_L1 6
#define GPAD_R1 7
#define GPAD_DPAD_UP 8
#define GPAD_DPAD_DOWN 9
#define GPAD_DPAD_LEFT 10
#define GPAD_DPAD_RIGHT 11
#define GPAD_THUMBL 12
#define GPAD_THUMBR 13
#define GPAD_BACK 14
#define GPAD_MENU 15

typedef struct { int sdl; int code; } PadMap;
static const PadMap pad_map[] = {
    { SDL_CONTROLLER_BUTTON_A, GPAD_A }, { SDL_CONTROLLER_BUTTON_B, GPAD_B },
    { SDL_CONTROLLER_BUTTON_X, GPAD_X }, { SDL_CONTROLLER_BUTTON_Y, GPAD_Y },
    { SDL_CONTROLLER_BUTTON_BACK, GPAD_BACK }, { SDL_CONTROLLER_BUTTON_GUIDE, GPAD_MENU },
    { SDL_CONTROLLER_BUTTON_START, GPAD_START }, { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, GPAD_L1 },
    { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, GPAD_R1 }, { SDL_CONTROLLER_BUTTON_DPAD_UP, GPAD_DPAD_UP },
    { SDL_CONTROLLER_BUTTON_DPAD_DOWN, GPAD_DPAD_DOWN }, { SDL_CONTROLLER_BUTTON_DPAD_LEFT, GPAD_DPAD_LEFT },
    { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, GPAD_DPAD_RIGHT }, { SDL_CONTROLLER_BUTTON_LEFTSTICK, GPAD_THUMBL },
    { SDL_CONTROLLER_BUTTON_RIGHTSTICK, GPAD_THUMBR },
};
#define PAD_MAP_N (sizeof(pad_map) / sizeof(pad_map[0]))

static SDL_GameController *g_pad = NULL;
static Uint16 g_prev_buttons = 0;
static int g_mouse_touch_down = 0;

static void update_gamepad(void) {
    if (!g_pad || !SDL_GameControllerGetAttached(g_pad)) return;

    Uint16 now = 0;
    for (size_t i = 0; i < PAD_MAP_N; i++) {
        if (SDL_GameControllerGetButton(g_pad, pad_map[i].sdl))
            now |= (Uint16)(1u << pad_map[i].code);
    }
    for (size_t i = 0; i < PAD_MAP_N; i++) {
        const unsigned b = 1u << pad_map[i].code;
        if ((now & b) && !(g_prev_buttons & b)) {
            if (implOnGamepadButtonDown) implOnGamepadButtonDown(fake_env, NULL, 0, pad_map[i].code);
            movie_skip();
        }
        if (!(now & b) && (g_prev_buttons & b)) {
            if (implOnGamepadButtonUp) implOnGamepadButtonUp(fake_env, NULL, 0, pad_map[i].code);
        }
    }
    g_prev_buttons = now;
    if ((now & (1u << GPAD_BACK)) && (now & (1u << GPAD_MENU))) jni_quit_requested = 1;

    const Sint16 ax = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX);
    const Sint16 ay = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY);
    const Sint16 bx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX);
    const Sint16 by = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY);
    const Sint16 at = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    const Sint16 bt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);

    float axes[6] = {
        (float)ax / 32767.0f, (float)ay / 32767.0f,
        (float)bx / 32767.0f, (float)by / 32767.0f,
        (float)at / 32767.0f, (float)bt / 32767.0f,
    };
    for (int i = 0; i < 4; i++) if (fabsf(axes[i]) < 0.15f) axes[i] = 0.0f;
    if (axes[4] < 0.15f) axes[4] = 0.0f;
    if (axes[5] < 0.15f) axes[5] = 0.0f;

    static float prev_axes[6];
    if (memcmp(axes, prev_axes, sizeof(axes)) != 0) {
        memcpy(prev_axes, axes, sizeof(axes));
        if (implOnGamepadAxesChanged)
            implOnGamepadAxesChanged(fake_env, NULL, 0, axes[0], axes[1], axes[2], axes[3], axes[4], axes[5]);
    }
}

static void dispatch_rockstar_events(void) {
    JniRockstarEvent event;
    int dispatched = 0;
    while (dispatched < 32 && jni_pop_pending_rockstar(&event)) {
        dispatched++;
        switch (event.type) {
            case JNI_RS_INITIAL_COMPLETE: implOnRockstarInitialComplete(fake_env, NULL); break;
            case JNI_RS_SIGN_IN_COMPLETE: implOnRockstarSignInComplete(fake_env, NULL); break;
            case JNI_RS_SIGN_OUT_COMPLETE: implOnRockstarSignOutComplete(fake_env, NULL); break;
            case JNI_RS_CLOUD_DISABLED_COMPLETE: implOnRockstarCloudDisabledComplete(fake_env, NULL); break;
            case JNI_RS_GATE_COMPLETE: implOnRockstarGateComplete(fake_env, NULL, event.accepted); break;
            case JNI_RS_STATE_CHANGED: implOnRockstarStateChanged(fake_env, NULL, event.signed_in); break;
            case JNI_RS_ID_CHANGED: implOnRockstarIdChanged(fake_env, NULL, event.value ? event.value : jni_make_string("")); break;
            case JNI_RS_TICKET_CHANGED: implOnRockstarTicketChanged(fake_env, NULL, event.value ? event.value : jni_make_string("")); break;
            case JNI_RS_SETUP: implOnRockstarSetup(fake_env, NULL, event.environment, event.title_id); break;
        }
    }
}

int main(int argc, char *argv[]) {
    install_crash_handler();
    LOG_STEP("=== GTA: Chinatown Wars Linux AArch64 ===");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    LOG_STEP("Preloading device libraries...");
    preload_device_libs();

    LOG_STEP("Checking data files...");
    check_data();

    LOG_STEP("Initializing JNI environment...");
    jni_init();

    LOG_STEP("Initializing OpenGL context...");
    if (!ctw_init_gl()) {
        fprintf(stderr, "ERROR: ctw_init_gl failed!\n");
        return 1;
    }

    LOG_STEP("Prewarming audio...");
    ctw_audio_prewarm();

    LOG_STEP("Allocating heap memory (%d MB)...", HEAP_MB);
    size_t hs = (size_t)HEAP_MB * 1024 * 1024;
    void *heap = mmap(NULL, hs, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (heap == MAP_FAILED) {
        fprintf(stderr, "mmap %d MB failed\n", HEAP_MB);
        return 1;
    }

    LOG_STEP("Loading donor SO (%s)...", CXX_DONOR_SO);
    if (so_load(&donor_mod, CXX_DONOR_SO, heap, hs) < 0) {
        fprintf(stderr, "Could not load %s\n", CXX_DONOR_SO);
        return 1;
    }

    void *game_base = (void *)ALIGN_MEM((uintptr_t)heap + donor_mod.load_size, 0x1000);
    size_t game_limit = hs - ((uintptr_t)game_base - (uintptr_t)heap);

    LOG_STEP("Loading game SO (%s)...", GAME_SO);
    if (so_load(&game_mod, GAME_SO, game_base, game_limit) < 0) {
        fprintf(stderr, "Could not load %s\n", GAME_SO);
        return 1;
    }

    /* Place the arm64 trampoline pool right after the game module (still inside
     * the RWX heap mmap, so only a few MB from every hook site).  This is what
     * keeps hook_arm64's 4-byte B branch (offset limited to ±128 MB) able to reach
     * its slot; without it the pool is mmap'd anonymously far from the game .text
     * and every hook silently fails to fire. */
    so_set_tramp_pool((void *)ALIGN_MEM((uintptr_t)game_base + game_mod.load_size, 16));

    LOG_STEP("Relocating modules...");
    so_relocate(&donor_mod);
    so_relocate(&game_mod);

    LOG_STEP("Resolving imports...");
    so_resolve(&donor_mod, dynlib_functions, dynlib_functions_count, 1);
    so_resolve(&game_mod, dynlib_functions, dynlib_functions_count, 1);

    LOG_STEP("Patching game & resolving entry points...");
    patch_game();
    resolve_entry_points();

    LOG_STEP("Finalizing memory permissions...");
    so_finalize(&donor_mod);
    so_finalize(&game_mod);
    so_flush_caches(&donor_mod);
    so_flush_caches(&game_mod);

    LOG_STEP("Executing init_array (constructors) for %s...", CXX_DONOR_SO);
    so_execute_init_array(&donor_mod);

    LOG_STEP("Executing init_array (constructors) for %s...", GAME_SO);
    so_execute_init_array(&game_mod);

    void (* NVThreadInit)(void *) = (void *)so_find_addr_rx(&game_mod, "_Z12NVThreadInitP7_JavaVM");
    int (* JNI_OnLoad)(void *, void *) = (void *)so_find_addr_rx(&game_mod, "JNI_OnLoad");

    if (NVThreadInit) {
        LOG_STEP("Calling NVThreadInit...");
        NVThreadInit(fake_vm);
    }
    if (JNI_OnLoad) {
        LOG_STEP("Calling JNI_OnLoad...");
        JNI_OnLoad(fake_vm, NULL);
    }

    void *services = jni_make_object("GamePlatformServices");
    void *device_info = jni_make_object("DeviceInfo");
    void *asset_mgr = jni_make_object("AssetManager");
    void *surface = jni_make_object("Surface");
    const char *path_strs[] = { ".", "." };
    void *paths = jni_make_string_array(2, path_strs);
    void *args = jni_make_string_array(2, path_strs);

    LOG_STEP("Calling implOnActivityCreated...");
    if (implOnActivityCreated) implOnActivityCreated(fake_env, NULL, services, 0);

    LOG_STEP("Calling implOnInitialSetup...");
    if (implOnInitialSetup) implOnInitialSetup(fake_env, NULL, device_info, asset_mgr, paths, args);

    LOG_STEP("Calling implOnSurfaceCreated & implOnSurfaceChanged...");
    if (implOnSurfaceCreated) implOnSurfaceCreated(fake_env, NULL);
    if (implOnSurfaceChanged) implOnSurfaceChanged(fake_env, NULL, surface, ctw_screen_w(), ctw_screen_h());
    if (implOnNetworkChanged) implOnNetworkChanged(fake_env, NULL, 0);

    g_pad = SDL_GameControllerOpen(0);
    if (g_pad && implOnGamepadConnected) {
        implOnGamepadConnected(fake_env, NULL, 0);
        int pad_ids[] = {0};
        if (implOnGamepadResume) implOnGamepadResume(fake_env, NULL, jni_make_int_array(1, pad_ids));
    }

    LOG_STEP("Calling implOnGameResume...");
    if (implOnGameResume) implOnGameResume(fake_env, NULL);

    LOG_STEP("Entering main game loop!");
    Uint32 boot_start = SDL_GetTicks();
    int rk_fired = 0;
    Uint32 last = SDL_GetTicks();

    while (!jni_quit_requested) {
        dispatch_rockstar_events();

        JniPlaylistEvent p_event;
        while (jni_pop_pending_playlist(&p_event)) {
            if (implOnPlaylistOpenComplete)
                implOnPlaylistOpenComplete(fake_env, NULL, p_event.success, p_event.count);
        }

        music_tick();

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                jni_quit_requested = 1;
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                g_mouse_touch_down = 1;
                if (implOnTouchStart) implOnTouchStart(fake_env, NULL, 0, (float)e.button.x, (float)e.button.y);
            } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
                g_mouse_touch_down = 0;
                if (implOnTouchEnd) implOnTouchEnd(fake_env, NULL, 0, (float)e.button.x, (float)e.button.y);
            } else if (e.type == SDL_MOUSEMOTION && g_mouse_touch_down) {
                if (implOnTouchMove) implOnTouchMove(fake_env, NULL, 0, (float)e.motion.x, (float)e.motion.y);
            }
        }

        update_gamepad();

        if (!rk_fired && SDL_GetTicks() - boot_start > 450) {
            rk_fired = 1;
            jni_push_rockstar(JNI_RS_SETUP, 0, 1, NULL, "prod", "pc");
            jni_push_rockstar(JNI_RS_INITIAL_COMPLETE, 0, 1, NULL, NULL, NULL);
            jni_push_rockstar(JNI_RS_GATE_COMPLETE, 1, 1, NULL, NULL, NULL);
            jni_push_rockstar(JNI_RS_CLOUD_DISABLED_COMPLETE, 0, 1, NULL, NULL, NULL);
            jni_push_rockstar(JNI_RS_STATE_CHANGED, 0, 1, NULL, NULL, NULL);
            jni_push_rockstar(JNI_RS_ID_CHANGED, 0, 1, "pc_player", NULL, NULL);
            jni_push_rockstar(JNI_RS_TICKET_CHANGED, 0, 1, "pc_ticket", NULL, NULL);
            jni_push_rockstar(JNI_RS_SIGN_IN_COMPLETE, 0, 1, NULL, NULL, NULL);
        }

        Uint32 now = SDL_GetTicks();
        float dt = (float)(now - last) / 1000.0f;
        last = now;
        if (dt <= 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;

        if (implOnDrawFrame) implOnDrawFrame(fake_env, NULL, dt);
        movie_main_loop_tick();
    }

    if (implOnGamePause) implOnGamePause(fake_env, NULL);
    if (implOnSurfaceDestroyed) implOnSurfaceDestroyed(fake_env, NULL);
    if (implOnActivityDestroyed) implOnActivityDestroyed(fake_env, NULL);
    movie_stop();
    deinit_openal();
    if (g_pad) SDL_GameControllerClose(g_pad);
    SDL_Quit();
    return 0;
}
