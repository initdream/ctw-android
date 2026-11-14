#ifndef EGL_SHIM_H
#define EGL_SHIM_H

#include <stdint.h>
#include <EGL/egl.h>

int  ctw_init_gl(void);
void ctw_swap_buffers(void);
int  ctw_make_current(void);
void ctw_release_current(void);
void ctw_egl_objects(uintptr_t *display, uintptr_t *surface, uintptr_t *context);
int  ctw_screen_w(void);
int  ctw_screen_h(void);
int  ctw_is_kmsdrm(void);

/* EGL import shims (defined in egl_shim.c, referenced by imports.c) */
EGLBoolean eglChooseConfig_shim(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
EGLContext eglCreateContext_shim(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
EGLSurface eglCreateWindowSurface_shim(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
EGLBoolean eglDestroySurface_shim(EGLDisplay, EGLSurface);
EGLBoolean eglGetConfigAttrib_shim(EGLDisplay, EGLConfig, EGLint, EGLint *);
EGLDisplay eglGetDisplay_shim(EGLNativeDisplayType);
__eglMustCastToProperFunctionPointerType eglGetProcAddress_shim(const char *);
EGLBoolean eglInitialize_shim(EGLDisplay, EGLint *, EGLint *);
EGLBoolean eglMakeCurrent_shim(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
const char *eglQueryString_shim(EGLDisplay, EGLint);
EGLBoolean eglSwapBuffers_shim(EGLDisplay, EGLSurface);
EGLBoolean eglSwapInterval_shim(EGLDisplay, EGLint);

#endif
