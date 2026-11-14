#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <AL/al.h>
#include <AL/alc.h>
#include "so_util.h"
#include "imports.h"
#include "egl_shim.h"
#include "pthread_bridge.h"

extern void __cxa_atexit(void (*)(void *), void *, void *);
extern void __cxa_finalize(void *);
extern int *__errno_location(void);

uint64_t __stack_chk_guard_fake = 0x4242424242424242;
void __stack_chk_fail_fake(void) { abort(); }

static uint8_t fake_sF[3][256];
static uint32_t SL_IID_fake[4] = {0};

static const char *resolve_android_path(const char *path) {
    static _Thread_local char alt_path[2048];
    static _Thread_local char asset_path[2048];

    static const char *app_data_prefix = "/data/data/com.rockstargames.gtactw/";
    static const char *app_data_prefix_2 = "/data/user/0/com.rockstargames.gtactw/";
    static const char *sdcard_prefix = "/storage/emulated/0/Android/data/com.rockstargames.gtactw/";

    if (!path || path[0] == '\0') return path;

    if (strncmp(path, app_data_prefix, strlen(app_data_prefix)) == 0) {
        snprintf(alt_path, sizeof(alt_path), "./%s", path + strlen(app_data_prefix));
        return alt_path;
    }
    if (strncmp(path, app_data_prefix_2, strlen(app_data_prefix_2)) == 0) {
        snprintf(alt_path, sizeof(alt_path), "./%s", path + strlen(app_data_prefix_2));
        return alt_path;
    }
    if (strncmp(path, sdcard_prefix, strlen(sdcard_prefix)) == 0) {
        snprintf(alt_path, sizeof(alt_path), "./%s", path + strlen(sdcard_prefix));
        return alt_path;
    }

    if (access(path, F_OK) == 0) return path;

    const char *basename = strrchr(path, '/');
    basename = basename ? basename + 1 : path;
    if (basename[0] != '\0') {
        snprintf(asset_path, sizeof(asset_path), "./assets/%s", basename);
        if (access(asset_path, F_OK) == 0) return asset_path;
    }

    if (snprintf(asset_path, sizeof(asset_path), "./assets/%s", path) < (int)sizeof(asset_path)) {
        if (access(asset_path, F_OK) == 0) return asset_path;
    }

    return path;
}

static FILE *ctw_fopen(const char *path, const char *mode) {
    if (!path || !path[0]) return NULL;
    const char *resolved = resolve_android_path(path);
    FILE *fp = fopen(resolved, mode);
    fprintf(stderr, "[fs] fopen('%s' -> '%s', '%s') -> %p\n", path, resolved, mode, (void *)fp);
    fflush(stderr);
    return fp;
}

static int ctw_open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap);
    }
    const char *resolved = resolve_android_path(path);
    int fd = open(resolved, flags, mode);
    fprintf(stderr, "[fs] open('%s' -> '%s') -> %d\n", path, resolved, fd);
    fflush(stderr);
    return fd;
}

static int ctw_access(const char *path, int mode) {
    const char *resolved = resolve_android_path(path);
    return access(resolved, mode);
}

/* --- C++ __cxa_guard stubs --- */
static int  s_cxa_guard_acquire(char *g) { return g && g[0] == 0; }
static void s_cxa_guard_release(char *g) { if (g) g[0] = 1; }
static void s_cxa_guard_abort(char *g)   { (void)g; }
static void s_cxa_pure_virtual(void)     { fprintf(stderr, "[ctw] __cxa_pure_virtual called\n"); }

static int is_fake_file(const void *f) {
    if (f == NULL) return 1;
    const uint8_t *p = f;
    const uint8_t *base = (const uint8_t *)fake_sF;
    return p >= base && p < base + sizeof(fake_sF);
}

size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f) {
    if (is_fake_file(f)) return n;
    return fwrite(ptr, size, n, f);
}

size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f) {
    if (is_fake_file(f)) return 0;
    return fread(ptr, size, n, f);
}

int fputc_fake(int c, FILE *f) {
    if (is_fake_file(f)) return c;
    return fputc(c, f);
}

int fputs_fake(const char *s, FILE *f) {
    if (is_fake_file(f)) return 0;
    return fputs(s, f);
}

int fflush_fake(FILE *f) {
    if (is_fake_file(f) || f == NULL) return 0;
    return fflush(f);
}

int fclose_fake(FILE *f) {
    if (is_fake_file(f)) return 0;
    return fclose(f);
}

int ferror_fake(FILE *f) {
    if (is_fake_file(f)) return 0;
    return ferror(f);
}

int fileno_fake(FILE *f) {
    if (f == NULL) return -1;
    if (is_fake_file(f)) return ((const uint8_t *)f - &fake_sF[0][0]) / sizeof(fake_sF[0]);
    return fileno(f);
}

int fprintf_fake(FILE *f, const char *fmt, ...) {
    if (is_fake_file(f)) return 0;
    va_list va; va_start(va, fmt);
    int ret = vfprintf(f, fmt, va);
    va_end(va);
    return ret;
}

int vfprintf_fake(FILE *f, const char *fmt, va_list va) {
    if (is_fake_file(f)) return 0;
    return vfprintf(f, fmt, va);
}

int fseek_fake(FILE *f, long off, int whence) {
    if (is_fake_file(f)) return -1;
    return fseek(f, off, whence);
}

int getc_fake(FILE *f) {
    if (is_fake_file(f)) return -1;
    return getc(f);
}

int ungetc_fake(int c, FILE *f) {
    if (is_fake_file(f)) return -1;
    return ungetc(c, f);
}

void setbuf_fake(FILE *f, char *buf) {
    if (is_fake_file(f)) return;
    setbuf(f, buf);
}

static int __system_property_get_fake(const char *name, char *value) {
    if (value) value[0] = '\0';
    return 0;
}

static void __assert2_fake(const char *file, int line, const char *func, const char *expr) {
    fprintf(stderr, "ASSERTION FAILED: %s:%d (%s): %s\n", file, line, func ? func : "", expr ? expr : "");
}

static unsigned int slCreateEngine_fake(void *pEngine, uint32_t numOptions, const void *pEngineOptions, uint32_t numInterfaces, const void *pInterfaceIds, const void *pInterfaceRequired) {
    return 0x0000000C; // SL_RESULT_FEATURE_UNSUPPORTED
}

void sincosf_fake(float x, float *s, float *c) {
    *s = sinf(x); *c = cosf(x);
}

void *__memcpy_chk(void *dst, const void *src, size_t n, size_t dst_len) { return memcpy(dst, src, n); }
char *__strcat_chk(char *dst, const char *src, size_t dst_len) { return strcat(dst, src); }
char *__strcpy_chk(char *dst, const char *src, size_t dst_len) { return strcpy(dst, src); }
size_t __strlen_chk(const char *s, size_t max_len) { return strlen(s); }
char *__strncpy_chk(char *dst, const char *src, size_t n, size_t dst_len) { return strncpy(dst, src, n); }
char *__strncpy_chk2(char *dst, const char *src, size_t n, size_t dst_len, size_t src_len) { return strncpy(dst, src, n); }
int __vsnprintf_chk(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list ap) { return vsnprintf(s, maxlen, fmt, ap); }
int __vsprintf_chk(char *s, int flag, size_t slen, const char *fmt, va_list ap) { return vsprintf(s, fmt, ap); }
ssize_t __read_chk(int fd, void *buf, size_t count, size_t buf_size) { return read(fd, buf, count); }
ssize_t __write_chk(int fd, const void *buf, size_t count, size_t buf_size) { (void)buf_size; return write(fd, buf, count); }

int __android_log_print(int prio, const char *tag, const char *fmt, ...) { return 0; }
int __android_log_write(int prio, const char *tag, const char *text) { return 0; }
void android_set_abort_message(const char *msg) { fprintf(stderr, "ABORT: %s\n", msg ? msg : "?"); }

struct bionic_timespec { int64_t tv_sec; int64_t tv_nsec; };
struct bionic_stat {
    uint64_t st_dev; uint64_t st_ino; uint32_t st_mode; uint32_t st_nlink; uint32_t st_uid;
    uint32_t st_gid; uint64_t st_rdev; uint64_t __pad1; int64_t st_size; int32_t st_blksize;
    int32_t __pad2; int64_t st_blocks; struct bionic_timespec st_atim; struct bionic_timespec st_mtim;
    struct bionic_timespec st_ctim; uint32_t __unused4; uint32_t __unused5;
};

static void convert_stat(const struct stat *in, struct bionic_stat *out) {
    memset(out, 0, sizeof(*out));
    out->st_dev = in->st_dev; out->st_ino = in->st_ino; out->st_mode = in->st_mode;
    out->st_nlink = in->st_nlink; out->st_uid = in->st_uid; out->st_gid = in->st_gid;
    out->st_rdev = in->st_rdev; out->st_size = in->st_size; out->st_blksize = in->st_blksize;
    out->st_blocks = in->st_blocks; out->st_atim.tv_sec = in->st_atim.tv_sec;
    out->st_mtim.tv_sec = in->st_mtim.tv_sec; out->st_ctim.tv_sec = in->st_ctim.tv_sec;
}

int stat_fake(const char *path, struct bionic_stat *st) {
    struct stat real;
    const char *resolved = resolve_android_path(path);
    int ret = stat(resolved, &real);
    if (ret == 0 && st) convert_stat(&real, st);
    return ret;
}

int fstat_fake(int fd, struct bionic_stat *st) {
    struct stat real; int ret = fstat(fd, &real);
    if (ret == 0 && st) convert_stat(&real, st);
    return ret;
}

int lstat_fake(const char *path, struct bionic_stat *st) {
    struct stat real;
    const char *resolved = resolve_android_path(path);
    int ret = lstat(resolved, &real);
    if (ret == 0 && st) convert_stat(&real, st);
    return ret;
}

void *am_fromJava(void *env, void *obj) { return (void *)0xA55E7; }
void *aw_fromSurface(void *env, void *surface) { return (void *)0xAA11; }
int aw_setBuffersGeometry(void *w, int x, int y, int f) { return 0; }
int aw_getWidth(void *w) { return ctw_screen_w(); }
int aw_getHeight(void *w) { return ctw_screen_h(); }
void aw_release(void *w) { }

typedef struct { FILE *fp; long len; } CtwAAsset;

void *aa_open(void *mgr, const char *path, int mode) {
    if (!path || !path[0]) return NULL;
    const char *resolved = resolve_android_path(path);
    FILE *fp = fopen(resolved, "rb");
    fprintf(stderr, "[asset] AAssetManager_open('%s' -> '%s') -> %p\n", path, resolved, (void *)fp);
    fflush(stderr);

    if (!fp) return NULL;
    CtwAAsset *a = calloc(1, sizeof(*a));
    a->fp = fp; fseek(fp, 0, SEEK_END); a->len = ftell(fp); fseek(fp, 0, SEEK_SET);
    return a;
}

int aa_read(void *h, void *buf, size_t n) { CtwAAsset *a = h; return a ? (int)fread(buf, 1, n, a->fp) : -1; }
long aa_seek64(void *h, long off, int wh) { CtwAAsset *a = h; if (!a) return -1; fseek(a->fp, off, wh); return ftell(a->fp); }
long aa_getLength64(void *h) { CtwAAsset *a = h; return a ? a->len : 0; }
long aa_getRemainingLength64(void *h) { CtwAAsset *a = h; return a ? a->len - ftell(a->fp) : 0; }
void aa_close(void *h) { CtwAAsset *a = h; if (a) { if (a->fp) fclose(a->fp); free(a); } }

ALCcontext *al_hook_ctx = NULL; ALCdevice *al_hook_dev = NULL;
ALCcontext *alcCreateContextHook(ALCdevice *dev, const ALCint *unused) {
    if (al_hook_ctx) return al_hook_ctx;
    const ALCint attr[] = {ALC_FREQUENCY, 44100, 0};
    return (al_hook_ctx = alcCreateContext(dev, attr));
}
ALCdevice *alcOpenDeviceHook(const char *name) {
    if (al_hook_dev) return al_hook_dev;
    return (al_hook_dev = alcOpenDevice(name));
}
void alcDestroyContextHook(ALCcontext *ctx) { if (ctx == al_hook_ctx) al_hook_ctx = NULL; alcDestroyContext(ctx); }
ALCboolean alcCloseDeviceHook(ALCdevice *dev) { if (dev == al_hook_dev) al_hook_dev = NULL; return alcCloseDevice(dev); }
void alSourcef_hook(ALuint source, ALenum param, ALfloat value) {
    if (param == AL_GAIN && value == 0.0f) value = 1.0f;
    alSourcef(source, param, value);
}
void ctw_audio_prewarm(void) {
    if (!al_hook_dev) al_hook_dev = alcOpenDevice(NULL);
    if (!al_hook_ctx && al_hook_dev) {
        const ALCint attr[] = {ALC_FREQUENCY, 44100, 0};
        al_hook_ctx = alcCreateContext(al_hook_dev, attr);
        alcMakeContextCurrent(al_hook_ctx);
    }
}
void deinit_openal(void) {
    if (al_hook_dev) {
        if (al_hook_ctx) { alcMakeContextCurrent(NULL); alcDestroyContext(al_hook_ctx); }
        alcCloseDevice(al_hook_dev);
    }
}
static int ret0(void) { return 0; }

DynLibFunction dynlib_functions[] = {
    {"fopen", (uintptr_t)&ctw_fopen},
    {"open", (uintptr_t)&ctw_open},
    {"access", (uintptr_t)&ctw_access},
    {"sigaction", (uintptr_t)&ret0},
    {"sigemptyset", (uintptr_t)&ret0},
    {"pthread_setname_np", (uintptr_t)&ret0},
    {"__cxa_guard_acquire", (uintptr_t)&s_cxa_guard_acquire},
    {"__cxa_guard_release", (uintptr_t)&s_cxa_guard_release},
    {"__cxa_guard_abort", (uintptr_t)&s_cxa_guard_abort},
    {"__cxa_pure_virtual", (uintptr_t)&s_cxa_pure_virtual},
    {"__sF", (uintptr_t)&fake_sF},
    {"stderr", (uintptr_t)&stderr},
    {"stdout", (uintptr_t)&stdout},
    {"stdin", (uintptr_t)&stdin},
    {"fwrite", (uintptr_t)&fwrite_fake},
    {"fread", (uintptr_t)&fread_fake},
    {"fputc", (uintptr_t)&fputc_fake},
    {"fputs", (uintptr_t)&fputs_fake},
    {"fflush", (uintptr_t)&fflush_fake},
    {"fclose", (uintptr_t)&fclose_fake},
    {"ferror", (uintptr_t)&ferror_fake},
    {"fileno", (uintptr_t)&fileno_fake},
    {"fprintf", (uintptr_t)&fprintf_fake},
    {"vfprintf", (uintptr_t)&vfprintf_fake},
    {"fseek", (uintptr_t)&fseek_fake},
    {"getc", (uintptr_t)&getc_fake},
    {"ungetc", (uintptr_t)&ungetc_fake},
    {"setbuf", (uintptr_t)&setbuf_fake},
    {"__assert2", (uintptr_t)&__assert2_fake},
    {"__system_property_get", (uintptr_t)&__system_property_get_fake},
    {"__write_chk", (uintptr_t)&__write_chk},
    {"slCreateEngine", (uintptr_t)&slCreateEngine_fake},
    {"SL_IID_ENGINE", (uintptr_t)&SL_IID_fake},
    {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&SL_IID_fake},
    {"SL_IID_ANDROIDCONFIGURATION", (uintptr_t)&SL_IID_fake},
    {"SL_IID_PLAY", (uintptr_t)&SL_IID_fake},
    {"SL_IID_RECORD", (uintptr_t)&SL_IID_fake},
    {"__errno", (uintptr_t)&__errno_location},
    {"AAssetManager_fromJava", (uintptr_t)&am_fromJava},
    {"AAssetManager_open", (uintptr_t)&aa_open},
    {"AAsset_close", (uintptr_t)&aa_close},
    {"AAsset_getLength64", (uintptr_t)&aa_getLength64},
    {"AAsset_getRemainingLength64", (uintptr_t)&aa_getRemainingLength64},
    {"AAsset_read", (uintptr_t)&aa_read},
    {"AAsset_seek64", (uintptr_t)&aa_seek64},
    {"ANativeWindow_fromSurface", (uintptr_t)&aw_fromSurface},
    {"ANativeWindow_getHeight", (uintptr_t)&aw_getHeight},
    {"ANativeWindow_getWidth", (uintptr_t)&aw_getWidth},
    {"ANativeWindow_release", (uintptr_t)&aw_release},
    {"ANativeWindow_setBuffersGeometry", (uintptr_t)&aw_setBuffersGeometry},
    {"android_set_abort_message", (uintptr_t)&android_set_abort_message},
    {"__android_log_print", (uintptr_t)&__android_log_print},
    {"__android_log_write", (uintptr_t)&__android_log_write},
    {"__cxa_atexit", (uintptr_t)&__cxa_atexit},
    {"__cxa_finalize", (uintptr_t)&__cxa_finalize},
    {"dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr},
    {"__memcpy_chk", (uintptr_t)&__memcpy_chk},
    {"__strcat_chk", (uintptr_t)&__strcat_chk},
    {"__strcpy_chk", (uintptr_t)&__strcpy_chk},
    {"__strlen_chk", (uintptr_t)&__strlen_chk},
    {"__strncpy_chk", (uintptr_t)&__strncpy_chk},
    {"__strncpy_chk2", (uintptr_t)&__strncpy_chk2},
    {"__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk},
    {"__vsprintf_chk", (uintptr_t)&__vsprintf_chk},
    {"__read_chk", (uintptr_t)&__read_chk},
    {"sincosf", (uintptr_t)&sincosf_fake},
    {"__stack_chk_fail", (uintptr_t)&__stack_chk_fail_fake},
    {"__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake},
    {"stat", (uintptr_t)&stat_fake},
    {"fstat", (uintptr_t)&fstat_fake},
    {"lstat", (uintptr_t)&lstat_fake},
    {"eglChooseConfig", (uintptr_t)&eglChooseConfig_shim},
    {"eglCreateContext", (uintptr_t)&eglCreateContext_shim},
    {"eglCreateWindowSurface", (uintptr_t)&eglCreateWindowSurface_shim},
    {"eglDestroySurface", (uintptr_t)&eglDestroySurface_shim},
    {"eglGetConfigAttrib", (uintptr_t)&eglGetConfigAttrib_shim},
    {"eglGetDisplay", (uintptr_t)&eglGetDisplay_shim},
    {"eglGetProcAddress", (uintptr_t)&eglGetProcAddress_shim},
    {"eglInitialize", (uintptr_t)&eglInitialize_shim},
    {"eglMakeCurrent", (uintptr_t)&eglMakeCurrent_shim},
    {"eglQueryString", (uintptr_t)&eglQueryString_shim},
    {"eglSwapBuffers", (uintptr_t)&eglSwapBuffers_shim},
    {"eglSwapInterval", (uintptr_t)&eglSwapInterval_shim},
    {"alSourcef", (uintptr_t)&alSourcef_hook},
    {"alcCloseDevice", (uintptr_t)&alcCloseDeviceHook},
    {"alcCreateContext", (uintptr_t)&alcCreateContextHook},
    {"alcDestroyContext", (uintptr_t)&alcDestroyContextHook},
    {"alcOpenDevice", (uintptr_t)&alcOpenDeviceHook},
    {"_ZTHN10ALCcontext13sLocalContextE", (uintptr_t)&ret0},
    {"pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy_fake},
    {"pthread_attr_getschedparam", (uintptr_t)&pthread_attr_getschedparam_fake},
    {"pthread_attr_getstacksize", (uintptr_t)&pthread_attr_getstacksize_fake},
    {"pthread_attr_init", (uintptr_t)&pthread_attr_init_fake},
    {"pthread_attr_setschedparam", (uintptr_t)&pthread_attr_setschedparam_fake},
    {"pthread_attr_setstacksize", (uintptr_t)&pthread_attr_setstacksize_fake},
    {"pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake},
    {"pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake},
    {"pthread_cond_init", (uintptr_t)&pthread_cond_init_fake},
    {"pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake},
    {"pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake},
    {"pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake},
    {"pthread_create", (uintptr_t)&pthread_create_fake},
    {"pthread_detach", (uintptr_t)&pthread_detach_fake},
    {"pthread_getspecific", (uintptr_t)&pthread_getspecific_fake},
    {"pthread_join", (uintptr_t)&pthread_join_fake},
    {"pthread_key_create", (uintptr_t)&pthread_key_create_fake},
    {"pthread_key_delete", (uintptr_t)&pthread_key_delete_fake},
    {"pthread_mutexattr_destroy", (uintptr_t)&pthread_mutexattr_destroy_fake},
    {"pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_fake},
    {"pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_fake},
    {"pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake},
    {"pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake},
    {"pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake},
    {"pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake},
    {"pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake},
    {"pthread_once", (uintptr_t)&pthread_once_fake},
    {"pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock_fake},
    {"pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock_fake},
    {"pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock_fake},
    {"pthread_self", (uintptr_t)&pthread_self_fake},
    {"pthread_setschedparam", (uintptr_t)&pthread_setschedparam_fake},
    {"pthread_setspecific", (uintptr_t)&pthread_setspecific_fake},
    {"sem_destroy", (uintptr_t)&sem_destroy_fake},
    {"sem_getvalue", (uintptr_t)&sem_getvalue_fake},
    {"sem_init", (uintptr_t)&sem_init_fake},
    {"sem_post", (uintptr_t)&sem_post_fake},
    {"sem_trywait", (uintptr_t)&sem_trywait_fake},
    {"sem_wait", (uintptr_t)&sem_wait_fake},
};

const int dynlib_functions_count = sizeof(dynlib_functions) / sizeof(dynlib_functions[0]);
