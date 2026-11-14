#define _GNU_SOURCE
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "egl_shim.h"
#include "util.h"

#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x0040
#endif

static SDL_Window *g_sdl_win = NULL;
static SDL_GLContext g_glctx = NULL;
static int ctw_window = 0;
static int g_w = 1280, g_h = 720;
static int g_gles_major = 2;
static EGLDisplay g_dpy = (EGLDisplay)0x1001;
static EGLContext g_ctx = (EGLContext)0x3003;
static EGLSurface g_surf = (EGLSurface)0x2002;

int ctw_init_gl(void) {
  if (g_glctx)
    return 1;

  if (SDL_WasInit(SDL_INIT_VIDEO) == 0 &&
      SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "[sdl] InitVideo: %s\n", SDL_GetError());
    if (getenv("SDL_VIDEODRIVER")) {
      unsetenv("SDL_VIDEODRIVER");
      if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
        fprintf(stderr, "[sdl] InitVideo (auto): %s\n", SDL_GetError());
      else
        fprintf(stderr, "[sdl] InitVideo OK driver auto='%s'\n",
                SDL_GetCurrentVideoDriver());
    }
  }

  SDL_DisplayMode dm;
  if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
    g_w = dm.w; g_h = dm.h;
  }
  const char *wenv = getenv("CTW_W");
  const char *henv = getenv("CTW_H");
  int windowed = (wenv && henv) ? 1 : 0;
  if (wenv && atoi(wenv) > 0) g_w = atoi(wenv);
  if (henv && atoi(henv) > 0) g_h = atoi(henv);

  int msaa = 0;
  { const char *e = getenv("CTW_MSAA"); if (e) msaa = atoi(e); }
  static const int alpha_try[] = {8, 0};
  int msaa_try[2] = {0, 0}, nmsaa = 1;
  if (msaa > 0) { msaa_try[0] = msaa; msaa_try[1] = 0; nmsaa = 2; }

  for (int pass = 0; pass < 2 && !g_glctx; pass++) {
    int es_major = pass == 0 ? 3 : 2;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, es_major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    for (int j = 0; j < nmsaa && !g_sdl_win; j++)
      for (int i = 0; i < 2 && !g_sdl_win; i++) {
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, alpha_try[i]);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, msaa_try[j] ? 1 : 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, msaa_try[j]);
        Uint32 flags = SDL_WINDOW_OPENGL | (windowed ? SDL_WINDOW_RESIZABLE
                                                     : SDL_WINDOW_FULLSCREEN_DESKTOP);
        g_sdl_win = SDL_CreateWindow("GTA: Chinatown Wars",
                                     SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                     g_w, g_h, flags);
        if (!g_sdl_win)
          fprintf(stderr, "[sdl] CreateWindow alpha=%d msaa=%d %s: %s\n",
                  alpha_try[i], msaa_try[j], windowed ? "win" : "fs",
                  SDL_GetError());
      }
    if (!g_sdl_win) {
      if (pass == 0) {
        fprintf(stderr,
                "[gl] ES3 window unavailable, falling back to GLES2\n");
        continue;
      }
      return 0;
    }

    const char *drv = SDL_GetCurrentVideoDriver();
    ctw_window = 1;
    fprintf(stderr, "[gl] %s %dx%d video='%s'\n",
            windowed ? "windowed" : "fullscreen-desktop", g_w, g_h, drv ? drv : "?");

    static const int ladder[][2] = {{3, 2}, {3, 1}, {3, 0}, {2, 0}};
    int nvers = pass == 0 ? 3 : 1;
    int vbase = pass == 0 ? 0 : 3;
    for (int k = 0; k < nvers && !g_glctx; k++) {
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, ladder[vbase + k][0]);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, ladder[vbase + k][1]);
      g_glctx = SDL_GL_CreateContext(g_sdl_win);
      if (g_glctx)
        break;
      fprintf(stderr, "[gl] GLES %d.%d context unavailable: %s\n",
              ladder[vbase + k][0], ladder[vbase + k][1], SDL_GetError());
    }
    if (!g_glctx) {
      if (pass == 0) {
        fprintf(stderr,
                "[gl] GLES3 family unavailable, falling back to GLES2\n");
        SDL_DestroyWindow(g_sdl_win);
        g_sdl_win = NULL;
        ctw_window = 0;
        continue;
      }
      fprintf(stderr, "[sdl] GL_CreateContext: %s\n", SDL_GetError());
      return 0;
    }
  }

  SDL_GL_MakeCurrent(g_sdl_win, g_glctx);
  const GLubyte *glver = glGetString(GL_VERSION);
  const GLubyte *glren = glGetString(GL_RENDERER);
  if (glver) {
    const char *es = strstr((const char *)glver, "OpenGL ES");
    int mj = es ? atoi(es + 9) : 0;
    if (mj >= 2)
      g_gles_major = mj;
  }
  fprintf(stderr, "[gl] GLES ctx '%s' renderer='%s'\n",
          glver ? (const char *)glver : "?",
          glren ? (const char *)glren : "?");
  return 1;
}

void ctw_swap_buffers(void) {
  if (g_sdl_win)
    SDL_GL_SwapWindow(g_sdl_win);
}

int ctw_make_current(void) {
  if (g_sdl_win && g_glctx) {
    SDL_GL_MakeCurrent(g_sdl_win, g_glctx);
    return 1;
  }
  return 0;
}

void ctw_release_current(void) {
  if (g_sdl_win)
    SDL_GL_MakeCurrent(g_sdl_win, NULL);
}

void ctw_egl_objects(uintptr_t *display, uintptr_t *surface,
                     uintptr_t *context) {
  if (display)
    *display = (uintptr_t)g_dpy;
  if (surface)
    *surface = (uintptr_t)g_surf;
  if (context)
    *context = (uintptr_t)g_ctx;
}

int ctw_screen_w(void) { return g_w; }
int ctw_screen_h(void) { return g_h; }
int ctw_is_kmsdrm(void) { return 0; }

static void *egl_real(const char *name) { return dlsym(RTLD_DEFAULT, name); }

EGLDisplay eglGetDisplay_shim(EGLNativeDisplayType native_display) {
  (void)native_display;
  return (EGLDisplay)0x1001;
}

EGLBoolean eglInitialize_shim(EGLDisplay dpy, EGLint *major, EGLint *minor) {
  (void)dpy;
  if (major)
    *major = 1;
  if (minor)
    *minor = 4;
  return EGL_TRUE;
}

EGLBoolean eglGetConfigAttrib_shim(EGLDisplay dpy, EGLConfig config,
                                   EGLint attribute, EGLint *value) {
  (void)dpy;
  (void)config;
  switch (attribute) {
  case EGL_RED_SIZE:
    if (value)
      *value = 8;
    break;
  case EGL_GREEN_SIZE:
    if (value)
      *value = 8;
    break;
  case EGL_BLUE_SIZE:
    if (value)
      *value = 8;
    break;
  case EGL_ALPHA_SIZE:
    if (value)
      *value = 8;
    break;
  case EGL_DEPTH_SIZE:
    if (value)
      *value = 24;
    break;
  case EGL_STENCIL_SIZE:
    if (value)
      *value = 8;
    break;
  case EGL_SURFACE_TYPE:
    if (value)
      *value = EGL_WINDOW_BIT;
    break;
  case EGL_RENDERABLE_TYPE:
    if (value)
      *value = g_gles_major >= 3
                   ? (EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT)
                   : EGL_OPENGL_ES2_BIT;
    break;
  case EGL_COLOR_BUFFER_TYPE:
    if (value)
      *value = EGL_RGB_BUFFER;
    break;
  case EGL_CONFORMANT:
    if (value)
      *value = g_gles_major >= 3
                   ? (EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT)
                   : EGL_OPENGL_ES2_BIT;
    break;
  default:
    if (value)
      *value = 0;
    break;
  }
  return EGL_TRUE;
}

EGLBoolean eglChooseConfig_shim(EGLDisplay dpy, const EGLint *attrib_list,
                                EGLConfig *configs, EGLint config_size,
                                EGLint *num_config) {
  (void)dpy;
  (void)attrib_list;
  (void)config_size;
  if (configs && config_size > 0)
    configs[0] = (EGLConfig)0x2001;
  if (num_config)
    *num_config = 1;
  return EGL_TRUE;
}

EGLConfig eglCreateContext_shim(EGLDisplay dpy, EGLConfig config,
                                EGLContext share_list,
                                const EGLint *attrib_list) {
  (void)dpy;
  (void)config;
  (void)share_list;
  (void)attrib_list;
  return (EGLConfig)0x3003;
}

EGLSurface eglCreateWindowSurface_shim(EGLDisplay dpy, EGLConfig config,
                                       EGLNativeWindowType win,
                                       const EGLint *attrib_list) {
  (void)dpy;
  (void)config;
  (void)win;
  (void)attrib_list;
  return (EGLSurface)0x2002;
}

EGLBoolean eglDestroySurface_shim(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy;
  (void)surface;
  return EGL_TRUE;
}

EGLBoolean eglMakeCurrent_shim(EGLDisplay dpy, EGLSurface draw, EGLSurface read,
                               EGLContext ctx) {
  (void)dpy;
  (void)draw;
  (void)read;
  if (ctx == (EGLContext)EGL_NO_CONTEXT || ctx == NULL) {
    ctw_release_current();
    return EGL_TRUE;
  }
  return ctw_make_current() ? EGL_TRUE : EGL_FALSE;
}

EGLBoolean eglSwapBuffers_shim(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy;
  (void)surface;
  ctw_swap_buffers();
  return EGL_TRUE;
}

EGLBoolean eglSwapInterval_shim(EGLDisplay dpy, EGLint interval) {
  (void)dpy;
  if (ctw_window)
    SDL_GL_SetSwapInterval(interval ? 1 : 0);
  return EGL_TRUE;
}

const char *eglQueryString_shim(EGLDisplay dpy, EGLint name) {
  (void)dpy;
  switch (name) {
  case EGL_VENDOR:
    return "SDL2 (GTA Chinatown Wars shim)";
  case EGL_VERSION:
    return "1.4 SDL2 EGL shim";
  case EGL_EXTENSIONS:
    return "";
  default:
    return "";
  }
}

__eglMustCastToProperFunctionPointerType
eglGetProcAddress_shim(const char *procname) {
  if (!procname)
    return NULL;

  static const struct {
    const char *n;
    void *fn;
  } map[] = {
      {"eglGetDisplay", (void *)eglGetDisplay_shim},
      {"eglInitialize", (void *)eglInitialize_shim},
      {"eglChooseConfig", (void *)eglChooseConfig_shim},
      {"eglGetConfigAttrib", (void *)eglGetConfigAttrib_shim},
      {"eglCreateContext", (void *)eglCreateContext_shim},
      {"eglCreateWindowSurface", (void *)eglCreateWindowSurface_shim},
      {"eglDestroySurface", (void *)eglDestroySurface_shim},
      {"eglMakeCurrent", (void *)eglMakeCurrent_shim},
      {"eglSwapBuffers", (void *)eglSwapBuffers_shim},
      {"eglSwapInterval", (void *)eglSwapInterval_shim},
      {"eglQueryString", (void *)eglQueryString_shim},
      {"eglGetProcAddress", (void *)eglGetProcAddress_shim},
  };
  for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
    if (strcmp(procname, map[i].n) == 0)
      return (__eglMustCastToProperFunctionPointerType)map[i].fn;
  }

  static __eglMustCastToProperFunctionPointerType (*real)(const char *) = NULL;
  if (!real)
    real = (__eglMustCastToProperFunctionPointerType (*)(const char *))egl_real(
        "eglGetProcAddress");
  __eglMustCastToProperFunctionPointerType p = real ? real(procname) : NULL;
  if (!p && procname)
    p = (__eglMustCastToProperFunctionPointerType)dlsym(RTLD_DEFAULT, procname);
  return p;
}
