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
#include "so_util_x64.h"
#include "egl_shim.h"
#include "jni_shim.h"
#include "imports.h"
#include "util.h"
#include "error.h"
#include "music_player.h"

#define CXX_SO     "libc++_shared.so"
#define GAME_SO    "libGame.so"
#define CXX_HEAP_MB  48
#define GAME_HEAP_MB 128

static void crash_handler(int sig, siginfo_t *info, void *uc) {
  uintptr_t fault = (uintptr_t)info->si_addr;
  uintptr_t tb = (uintptr_t)text_base;
  fprintf(stderr, "\n=== CRASH sig=%d addr=%p ===\n", sig, info->si_addr);

  Dl_info dlinfo;
  if (dladdr((void *)fault, &dlinfo) && dlinfo.dli_fname) {
    fprintf(stderr, "  fault address in: %s (offset 0x%lx)\n", dlinfo.dli_fname,
            (unsigned long)(fault - (uintptr_t)dlinfo.dli_fbase));
    if (dlinfo.dli_sname)
      fprintf(stderr, "  symbol: %s\n", dlinfo.dli_sname);
  }
  if (tb && fault >= tb && fault < tb + text_size)
    fprintf(stderr, "  libGame+0x%lx (text_base=%p)\n",
            (unsigned long)(fault - tb), text_base);

    #if defined(__x86_64__)
    ucontext_t *u = (ucontext_t *)uc;
  uintptr_t pc = u->uc_mcontext.gregs[REG_RIP];
  fprintf(stderr, "  RIP=%p", (void *)pc);
  if (tb && pc >= tb && pc < tb + text_size)
    fprintf(stderr, " = libGame+0x%lx", (unsigned long)(pc - tb));
  fprintf(stderr, "\n");
  static const struct { int idx; const char *n; } regs[] = {
    {REG_RAX,"rax"},{REG_RBX,"rbx"},{REG_RCX,"rcx"},{REG_RDX,"rdx"},
    {REG_RSI,"rsi"},{REG_RDI,"rdi"},{REG_RBP,"rbp"},{REG_RSP,"rsp"},
    {REG_R8 ,"r8" },{REG_R9 ,"r9" },{REG_R10,"r10"},{REG_R11,"r11"},
    {REG_R12,"r12"},{REG_R13,"r13"},{REG_R14,"r14"},{REG_R15,"r15"},
  };
  fprintf(stderr, "  regs:");
  for (int i = 0; i < (int)(sizeof(regs)/sizeof(regs[0])); i++)
    fprintf(stderr, " %s=%#lx", regs[i].n,
            (unsigned long)u->uc_mcontext.gregs[regs[i].idx]);
    fprintf(stderr, "\n");
  if (pc) {
    unsigned char *p = (unsigned char *)pc;
    fprintf(stderr, "  insn:");
    for (int i = 0; i < 16; i++)
      fprintf(stderr, " %02x", p[i]);
    fprintf(stderr, "\n");
  }
  #endif

  void *bt[32];
  int n = backtrace(bt, 32);
  fprintf(stderr, "  Backtrace (%d frames):\n", n);
  char **strings = backtrace_symbols(bt, n);
  if (strings) {
    for (int i = 0; i < n; i++) {
      uintptr_t a = (uintptr_t)bt[i];
      if (tb && a >= tb && a < tb + text_size)
        fprintf(stderr, "    [%2d] libGame+0x%lx  %s\n", i,
                (unsigned long)(a - tb), strings[i]);
        else
          fprintf(stderr, "    [%2d] %s\n", i, strings[i]);
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

static void load_module(const char *name, int heap_mb, DynLibFunction *tbl, int n) {
  size_t hs = (size_t)heap_mb * 1024 * 1024;
  void *heap = mmap(NULL, hs, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED)
    fatal_error("mmap %d MB failed for %s", heap_mb, name);
  fprintf(stderr, "== loading %s (heap %p, %d MB) ==\n", name, heap, heap_mb);
  if (so_load(name, heap, hs) < 0)
    fatal_error("Could not load %s.", name);
  if (so_relocate() < 0)
    fatal_error("Could not relocate %s", name);
  so_resolve(tbl, n, 0);
  so_finalize();
  so_flush_caches();
  so_execute_init_array();
}

static void check_data(void) {
  struct stat st;
  if (stat(GAME_SO, &st) < 0)
    fatal_error("Missing %s", GAME_SO);
  if (stat(CXX_SO, &st) < 0)
    fatal_error("Missing %s", CXX_SO);

  const char *need[] = { "assets/game.pak", "assets/dxt.bin", NULL };
  int missing = 0;
  for (int i = 0; need[i]; i++) {
    if (stat(need[i], &st) < 0) {
      fprintf(stderr, "ERROR: Required data file '%s' not found.\n", need[i]);
      missing = 1;
    }
  }
  if (missing) {
    fatal_error("Missing game data files!\nPlease ensure game.pak and dxt.bin are located in the 'assets/' directory.");
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
static void (* implOnLowMemory)(void *env, void *cls);
static void (* implOnBackButtonPressed)(void *env, void *cls);
static void (* implOnDrawFrame)(void *env, void *cls, float dt);
static void (* implOnGamepadConnected)(void *env, void *cls, int id);
static void (* implOnGamepadDisconnected)(void *env, void *cls, int id);
static void (* implOnGamepadResume)(void *env, void *cls, void *ids);
static void (* implOnGamepadButtonDown)(void *env, void *cls, int id, int keycode);
static void (* implOnGamepadButtonUp)(void *env, void *cls, int id, int keycode);
static void (* implOnGamepadAxesChanged)(void *env, void *cls, int id, float lx, float ly, float rx, float ry, float lt, float rt);
static void (* implOnAccelerometerChanged)(void *env, void *cls, float x, float y, float z);
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
  #define ENTRY(var, sym)                                                     \
  do {                                                                      \
    var = (void *)so_find_addr_safe("Java_com_rockstargames_oswrapper_"     \
    "GameNative_" sym);                     \
    if (!var)                                                               \
      fprintf(stderr, "WARN: native entry point %s not found\n", sym);      \
  } while (0)
  ENTRY(implOnActivityCreated, "implOnActivityCreated");
  ENTRY(implOnActivityDestroyed, "implOnActivityDestroyed");
  ENTRY(implOnInitialSetup, "implOnInitialSetup");
  ENTRY(implOnSurfaceCreated, "implOnSurfaceCreated");
  ENTRY(implOnSurfaceChanged, "implOnSurfaceChanged");
  ENTRY(implOnSurfaceDestroyed, "implOnSurfaceDestroyed");
  ENTRY(implOnGameResume, "implOnGameResume");
  ENTRY(implOnGamePause, "implOnGamePause");
  ENTRY(implOnLowMemory, "implOnLowMemory");
  ENTRY(implOnBackButtonPressed, "implOnBackButtonPressed");
  ENTRY(implOnDrawFrame, "implOnDrawFrame");
  ENTRY(implOnGamepadConnected, "implOnGamepadConnected");
  ENTRY(implOnGamepadDisconnected, "implOnGamepadDisconnected");
  ENTRY(implOnGamepadResume, "implOnGamepadResume");
  ENTRY(implOnGamepadButtonDown, "implOnGamepadButtonDown");
  ENTRY(implOnGamepadButtonUp, "implOnGamepadButtonUp");
  ENTRY(implOnGamepadAxesChanged, "implOnGamepadAxesChanged");
  ENTRY(implOnAccelerometerChanged, "implOnAccelerometerChanged");
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
  (void)implOnLowMemory; (void)implOnAccelerometerChanged;
  (void)implOnGamepadDisconnected; (void)implOnBackButtonPressed;
}

static void patch_game(void) {
  uintptr_t a;
  if ((a = so_find_addr_safe("_Z24NVThreadGetCurrentJNIEnvv")))
    hook_x64(a, (uintptr_t)NVThreadGetCurrentJNIEnv);
  if ((a = so_find_addr_safe("_Z22NVThreadSpawnJNIThreadPlPK14pthread_attr_tPKcPFPvS5_ES5_")))
    hook_x64(a, (uintptr_t)NVThreadSpawnJNIThread);
  if ((a = so_find_addr_safe("_Z15OS_ThreadLaunchPFjPvES_jPKci16OSThreadPriority")))
    hook_x64(a, (uintptr_t)OS_ThreadLaunch);
  if ((a = so_find_addr_safe("_Z13OS_ThreadWaitPv")))
    hook_x64(a, (uintptr_t)OS_ThreadWait);
  if ((a = so_find_addr_safe("_Z14OS_ThreadClosePv")))
    hook_x64(a, (uintptr_t)OS_ThreadClose);
  if ((a = so_find_addr_safe("_Z18OS_ThreadIsRunningPv")))
    hook_x64(a, (uintptr_t)OS_ThreadIsRunning);
  if ((a = so_find_addr_safe("_Z17OS_ScreenGetWidthv")))
    hook_x64(a, (uintptr_t)OS_ScreenGetWidth);
  if ((a = so_find_addr_safe("_Z18OS_ScreenGetHeightv")))
    hook_x64(a, (uintptr_t)OS_ScreenGetHeight);
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
  { SDL_CONTROLLER_BUTTON_A,            GPAD_A },
  { SDL_CONTROLLER_BUTTON_B,            GPAD_B },
  { SDL_CONTROLLER_BUTTON_X,            GPAD_X },
  { SDL_CONTROLLER_BUTTON_Y,            GPAD_Y },
  { SDL_CONTROLLER_BUTTON_BACK,         GPAD_BACK },
  { SDL_CONTROLLER_BUTTON_GUIDE,        GPAD_MENU },
  { SDL_CONTROLLER_BUTTON_START,        GPAD_START },
  { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, GPAD_L1 },
  { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,GPAD_R1 },
  { SDL_CONTROLLER_BUTTON_DPAD_UP,      GPAD_DPAD_UP },
  { SDL_CONTROLLER_BUTTON_DPAD_DOWN,    GPAD_DPAD_DOWN },
  { SDL_CONTROLLER_BUTTON_DPAD_LEFT,    GPAD_DPAD_LEFT },
  { SDL_CONTROLLER_BUTTON_DPAD_RIGHT,   GPAD_DPAD_RIGHT },
  { SDL_CONTROLLER_BUTTON_LEFTSTICK,    GPAD_THUMBL },
  { SDL_CONTROLLER_BUTTON_RIGHTSTICK,   GPAD_THUMBR },
};
#define PAD_MAP_N (sizeof(pad_map) / sizeof(pad_map[0]))

static SDL_GameController *g_pad = NULL;
static Uint16 g_prev_buttons = 0;
static int g_mouse_touch_down = 0;

static void update_gamepad(void) {
  if (!g_pad || !SDL_GameControllerGetAttached(g_pad))
    return;

  Uint16 now = 0;
  for (size_t i = 0; i < PAD_MAP_N; i++) {
    if (SDL_GameControllerGetButton(g_pad, pad_map[i].sdl))
      now |= (Uint16)(1u << pad_map[i].code);
  }

  for (size_t i = 0; i < PAD_MAP_N; i++) {
    const unsigned b = 1u << pad_map[i].code;
    if ((now & b) && !(g_prev_buttons & b)) {
      if (implOnGamepadButtonDown)
        implOnGamepadButtonDown(fake_env, NULL, 0, pad_map[i].code);
    }
    if (!(now & b) && (g_prev_buttons & b)) {
      if (implOnGamepadButtonUp)
        implOnGamepadButtonUp(fake_env, NULL, 0, pad_map[i].code);
    }
  }
  g_prev_buttons = now;

  if ((g_prev_buttons & (1u << GPAD_BACK)) &&
    (g_prev_buttons & (1u << GPAD_MENU))) {
    debugPrintf("JNI: BACK+MENU -> quit\n");
  jni_quit_requested = 1;
    }

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

    /* Deadzone handling (15%) */
    for (int i = 0; i < 4; i++) {
      if (fabsf(axes[i]) < 0.15f) axes[i] = 0.0f;
    }
    if (axes[4] < 0.15f) axes[4] = 0.0f;
    if (axes[5] < 0.15f) axes[5] = 0.0f;

    static float prev_axes[6];
  if (memcmp(axes, prev_axes, sizeof(axes)) != 0) {
    memcpy(prev_axes, axes, sizeof(axes));
    if (implOnGamepadAxesChanged) {
      implOnGamepadAxesChanged(fake_env, NULL, 0,
                               axes[0], axes[1], axes[2], axes[3], axes[4], axes[5]);
    }
  }
}

static void handle_keyboard(const SDL_KeyboardEvent *ke) {
  int down = (ke->type == SDL_KEYDOWN) ? 1 : 0;
  int code = -1;
  switch (ke->keysym.sym) {
    case SDLK_UP:    code = GPAD_DPAD_UP; break;
    case SDLK_DOWN:  code = GPAD_DPAD_DOWN; break;
    case SDLK_LEFT:  code = GPAD_DPAD_LEFT; break;
    case SDLK_RIGHT: code = GPAD_DPAD_RIGHT; break;
    case SDLK_RETURN: code = GPAD_A; break;
    case SDLK_BACKSPACE: code = GPAD_B; break;
    case SDLK_ESCAPE: jni_quit_requested = 1; return;
    case SDLK_SPACE: code = GPAD_X; break;
    case SDLK_LSHIFT: code = GPAD_Y; break;
    case SDLK_TAB: code = GPAD_START; break;
    default: break;
  }
  if (code >= 0) {
    if (down)
      implOnGamepadButtonDown(fake_env, NULL, 0, code);
    else
      implOnGamepadButtonUp(fake_env, NULL, 0, code);
  }
}

static void dispatch_rockstar_events(void) {
  JniRockstarEvent event;
  int dispatched = 0;
  while (dispatched < 32 && jni_pop_pending_rockstar(&event)) {
    dispatched++;
    switch (event.type) {
      case JNI_RS_INITIAL_COMPLETE:
        implOnRockstarInitialComplete(fake_env, NULL); break;
      case JNI_RS_SIGN_IN_COMPLETE:
        implOnRockstarSignInComplete(fake_env, NULL); break;
      case JNI_RS_SIGN_OUT_COMPLETE:
        implOnRockstarSignOutComplete(fake_env, NULL); break;
      case JNI_RS_CLOUD_DISABLED_COMPLETE:
        implOnRockstarCloudDisabledComplete(fake_env, NULL); break;
      case JNI_RS_GATE_COMPLETE:
        implOnRockstarGateComplete(fake_env, NULL, event.accepted); break;
      case JNI_RS_STATE_CHANGED:
        implOnRockstarStateChanged(fake_env, NULL, event.signed_in); break;
      case JNI_RS_ID_CHANGED:
        implOnRockstarIdChanged(fake_env, NULL,
                                event.value ? event.value : jni_make_string("")); break;
      case JNI_RS_TICKET_CHANGED:
        implOnRockstarTicketChanged(fake_env, NULL,
                                    event.value ? event.value : jni_make_string("")); break;
      case JNI_RS_SETUP:
        implOnRockstarSetup(fake_env, NULL, event.environment, event.title_id);
        break;
      default:
        debugPrintf("JNI: unknown rockstar event %d\n", event.type); break;
    }
  }
}

static void dispatch_playlist_events(void) {
  JniPlaylistEvent event;
  while (jni_pop_pending_playlist(&event)) {
    debugPrintf("JNI: dispatch playlistOpenComplete(%d, %d)\n",
                event.success, event.count);
    implOnPlaylistOpenComplete(fake_env, NULL, event.success, event.count);
  }
}

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  install_crash_handler();
  fprintf(stderr, "=== GTA: Chinatown Wars (CTW 4.4.243) so-loader / x86_64 ===\n");

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
    fprintf(stderr, "WARN: SDL_Init failed: %s\n", SDL_GetError());
  }

  preload_device_libs();
  check_data();

  load_module(CXX_SO, CXX_HEAP_MB, dynlib_functions, dynlib_functions_count);
  int cxx_n = 0;
  DynLibFunction *cxx_tbl = so_snapshot_symbols(&cxx_n);

  int comb_n = dynlib_functions_count + cxx_n;
  DynLibFunction *comb = malloc(sizeof(DynLibFunction) * comb_n);
  if (!comb)
    fatal_error("out of memory building combined import table");
  memcpy(comb, dynlib_functions, sizeof(DynLibFunction) * dynlib_functions_count);
  memcpy(comb + dynlib_functions_count, cxx_tbl, sizeof(DynLibFunction) * cxx_n);

  load_module(GAME_SO, GAME_HEAP_MB, comb, comb_n);
  free(comb);

  {
    ImportOverride egl_ovr[] = {
      {"eglChooseConfig",       (uintptr_t)eglChooseConfig_shim, NULL},
      {"eglCreateContext",      (uintptr_t)eglCreateContext_shim, NULL},
      {"eglCreateWindowSurface",(uintptr_t)eglCreateWindowSurface_shim, NULL},
      {"eglDestroySurface",     (uintptr_t)eglDestroySurface_shim, NULL},
      {"eglGetConfigAttrib",    (uintptr_t)eglGetConfigAttrib_shim, NULL},
      {"eglGetDisplay",         (uintptr_t)eglGetDisplay_shim, NULL},
      {"eglGetProcAddress",     (uintptr_t)eglGetProcAddress_shim, NULL},
      {"eglInitialize",         (uintptr_t)eglInitialize_shim, NULL},
      {"eglMakeCurrent",        (uintptr_t)eglMakeCurrent_shim, NULL},
      {"eglQueryString",        (uintptr_t)eglQueryString_shim, NULL},
      {"eglSwapBuffers",        (uintptr_t)eglSwapBuffers_shim, NULL},
      {"eglSwapInterval",       (uintptr_t)eglSwapInterval_shim, NULL},
    };
    so_override_imports(egl_ovr, sizeof(egl_ovr) / sizeof(egl_ovr[0]));
  }

  if (getenv("CTW_MOVAPS_PATCH") && getenv("CTW_MOVAPS_PATCH")[0] != '0')
    so_patch_all_movaps();

  jni_init();
  int (* JNI_OnLoad)(void *, void *) = (void *)so_find_addr_safe("JNI_OnLoad");
  void (* NVThreadInit)(void *) = (void *)so_find_addr_safe("_Z12NVThreadInitP7_JavaVM");

  patch_game();
  resolve_entry_points();

  /* Boot order: NVThreadInit MUST run before JNI_OnLoad */
  if (NVThreadInit)   NVThreadInit(fake_vm);
  if (JNI_OnLoad)     JNI_OnLoad(fake_vm, NULL);

  void *services   = jni_make_object("GamePlatformServices");
  void *device_info = jni_make_object("DeviceInfo");
  void *asset_mgr  = jni_make_object("AssetManager");
  void *surface    = jni_make_object("Surface");
  const char *path_strs[] = { ".", "." };
  void *paths = jni_make_string_array(2, path_strs);
  const char *arg_strs[]  = { "", "" };
  void *args  = jni_make_string_array(2, arg_strs);

  debugPrintf("implOnActivityCreated\n");
  if (implOnActivityCreated)
    implOnActivityCreated(fake_env, NULL, services, 0);

  debugPrintf("implOnInitialSetup\n");
  if (implOnInitialSetup)
    implOnInitialSetup(fake_env, NULL, device_info, asset_mgr, paths, args);

  if (!ctw_init_gl())
    fprintf(stderr, "WARN: ctw_init_gl failed (continuing anyway)\n");

  ctw_audio_prewarm();

  debugPrintf("implOnSurfaceCreated\n");
  if (implOnSurfaceCreated)
    implOnSurfaceCreated(fake_env, NULL);

  debugPrintf("implOnSurfaceChanged %dx%d\n", ctw_screen_w(), ctw_screen_h());
  if (implOnSurfaceChanged)
    implOnSurfaceChanged(fake_env, NULL, surface, ctw_screen_w(), ctw_screen_h());

  debugPrintf("implOnNetworkChanged offline\n");
  if (implOnNetworkChanged)
    implOnNetworkChanged(fake_env, NULL, 0);

  g_pad = SDL_GameControllerOpen(0);
  if (g_pad) {
    fprintf(stderr, "controller connected: %s\n",
            SDL_GameControllerName(g_pad) ? SDL_GameControllerName(g_pad) : "?");
    if (implOnGamepadConnected)
      implOnGamepadConnected(fake_env, NULL, 0);
    const int pad_ids[] = { 0 };
    if (implOnGamepadResume)
      implOnGamepadResume(fake_env, NULL, jni_make_int_array(1, pad_ids));
  }

  debugPrintf("implOnGameResume\n");
  if (implOnGameResume)
    implOnGameResume(fake_env, NULL);
  if (implIsInitialized)
    fprintf(stderr, "=== implIsInitialized = %d ===\n",
            implIsInitialized(fake_env, NULL));

    Uint32 boot_start = SDL_GetTicks();
  int rk_fired = 0;
  Uint32 last = SDL_GetTicks();

  while (!jni_quit_requested) {
    dispatch_rockstar_events();
    dispatch_playlist_events();

    music_tick();

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) {
        jni_quit_requested = 1;
      } else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
        handle_keyboard(&e.key);

      } else if (e.type == SDL_CONTROLLERDEVICEADDED) {
        if (!g_pad) {
          g_pad = SDL_GameControllerOpen(e.cdevice.which);
          if (g_pad) {
            fprintf(stderr, "controller connected: %s\n", SDL_GameControllerName(g_pad));
            if (implOnGamepadConnected)
              implOnGamepadConnected(fake_env, NULL, 0);
            const int pad_ids[] = { 0 };
            if (implOnGamepadResume)
              implOnGamepadResume(fake_env, NULL, jni_make_int_array(1, pad_ids));
          }
        }
      } else if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
        if (g_pad) {
          SDL_GameControllerClose(g_pad);
          g_pad = NULL;
          if (implOnGamepadDisconnected)
            implOnGamepadDisconnected(fake_env, NULL, 0);
        }

      } else if (e.type == SDL_MOUSEBUTTONDOWN) {
        if (e.button.button == SDL_BUTTON_LEFT) {
          g_mouse_touch_down = 1;
          if (implOnTouchStart)
            implOnTouchStart(fake_env, NULL, 0, (float)e.button.x, (float)e.button.y);
        }
      } else if (e.type == SDL_MOUSEBUTTONUP) {
        if (e.button.button == SDL_BUTTON_LEFT) {
          g_mouse_touch_down = 0;
          if (implOnTouchEnd)
            implOnTouchEnd(fake_env, NULL, 0, (float)e.button.x, (float)e.button.y);
        }
      } else if (e.type == SDL_MOUSEMOTION) {
        if (g_mouse_touch_down) {
          if (implOnTouchMove)
            implOnTouchMove(fake_env, NULL, 0, (float)e.motion.x, (float)e.motion.y);
        }

      } else if (e.type == SDL_FINGERDOWN) {
        int slot = (int)(e.tfinger.fingerId % 4);
        float x = e.tfinger.x * (float)ctw_screen_w();
        float y = e.tfinger.y * (float)ctw_screen_h();
        if (implOnTouchStart)
          implOnTouchStart(fake_env, NULL, slot, x, y);

      } else if (e.type == SDL_FINGERUP) {
        int slot = (int)(e.tfinger.fingerId % 4);
        float x = e.tfinger.x * (float)ctw_screen_w();
        float y = e.tfinger.y * (float)ctw_screen_h();
        if (implOnTouchEnd)
          implOnTouchEnd(fake_env, NULL, slot, x, y);

      } else if (e.type == SDL_FINGERMOTION) {
        int slot = (int)(e.tfinger.fingerId % 4);
        float x = e.tfinger.x * (float)ctw_screen_w();
        float y = e.tfinger.y * (float)ctw_screen_h();
        if (implOnTouchMove)
          implOnTouchMove(fake_env, NULL, slot, x, y);
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
    if (dt <= 0.0f || dt > 0.25f)
      dt = 1.0f / 60.0f;

    if (implOnDrawFrame)
      implOnDrawFrame(fake_env, NULL, dt);
  }

  debugPrintf("shutting down\n");
  if (implOnGamePause)          implOnGamePause(fake_env, NULL);
  if (implOnSurfaceDestroyed)   implOnSurfaceDestroyed(fake_env, NULL);
  if (implOnActivityDestroyed)  implOnActivityDestroyed(fake_env, NULL);
  deinit_openal();
  if (g_pad) SDL_GameControllerClose(g_pad);
  SDL_Quit();
  return 0;
}
