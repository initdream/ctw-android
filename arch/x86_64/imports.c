#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sched.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <AL/al.h>
#include <AL/alc.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <mpg123.h>

#include "egl_shim.h"
#include "imports.h"
#include "pthread_bridge.h"
#include "so_util.h"
#include "util.h"

extern void __cxa_atexit(void (*)(void *), void *, void *);
extern void __cxa_finalize(void *);

extern int _setjmp(jmp_buf) __attribute__((nothrow));
extern void longjmp(jmp_buf, int) __attribute__((nothrow, noreturn));
void __stack_chk_fail(void);
long syscall(long, ...);
extern int __register_atfork(void (*prepare)(void), void (*parent)(void),
                             void (*child)(void), void *dso_handle);

#include <execinfo.h>
static void ctw_log_abort(const char *why) {
  fprintf(stderr, "[ctw] GAME ABORT via %s\n", why);
  void *bt[24];
  int n = backtrace(bt, 24);
  char **ss = backtrace_symbols(bt, n);
  for (int i = 0; i < n; i++) {
    uintptr_t a = (uintptr_t)bt[i];
    fprintf(stderr, "  [ctw] %s", ss ? (ss[i] ? ss[i] : "?") : "?");
    if (text_base != NULL && a >= (uintptr_t)text_base &&
        a < (uintptr_t)text_base + text_size) {
      uintptr_t sym_off = 0;
      char *sn = so_find_nearest_symbol(a - (uintptr_t)text_base, &sym_off);
      fprintf(stderr, "  [libGame+0x%lx %s+0x%lx]",
              (unsigned long)(a - (uintptr_t)text_base), sn ? sn : "?",
              (unsigned long)(a - (uintptr_t)text_base - sym_off));
    }
    fprintf(stderr, "\n");
  }
  if (ss)
    free(ss);
  fflush(stderr);
}

static void ctw_abort(void) {
  ctw_log_abort("abort()");
  abort();
}
static void ctw_stack_chk_fail(void) {
  ctw_log_abort("__stack_chk_fail");
  abort();
}

int __cxa_thread_atexit_impl_fake(void (*fn)(void *), void *arg, void *dso) {
  (void)fn;
  (void)arg;
  (void)dso;
  return 0;
}

static int *bionic___errno(void) {
  extern int *__errno_location(void);
  return __errno_location();
}

static long b_gettid(void) { return (long)syscall(SYS_gettid); }

static int b_android_log(int prio, const char *tag, const char *fmt, ...) {
  (void)prio;
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "[CTW:%s] ", tag ? tag : "?");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  return 0;
}

static void *b_memcpy_chk(void *d, const void *s, size_t n, size_t dn) {
  (void)dn;
  return memcpy(d, s, n);
}
static char *b_strcat_chk(char *d, const char *s, size_t dn) {
  (void)dn;
  return strcat(d, s);
}
static char *b_strcpy_chk(char *d, const char *s, size_t dn) {
  (void)dn;
  return strcpy(d, s);
}
static size_t b_strlen_chk(const char *s, size_t sn) {
  (void)sn;
  return strlen(s);
}
static char *b_strncpy_chk(char *d, const char *s, size_t n, size_t dn) {
  (void)dn;
  return strncpy(d, s, n);
}
static ssize_t b_read_chk(int fd, void *b, size_t n, size_t bn) {
  (void)bn;
  return read(fd, b, n);
}
static ssize_t b_write_chk(int fd, const void *b, size_t n, size_t bn) {
  (void)bn;
  return write(fd, b, n);
}
static int b_vsnprintf_chk(char *s, size_t maxlen, int fl, size_t slen,
                           const char *fmt, va_list ap) {
  (void)fl;
  (void)slen;
  return vsnprintf(s, maxlen, fmt, ap);
}
static int b_vsprintf_chk(char *s, int fl, size_t slen, const char *fmt,
                          va_list ap) {
  (void)fl;
  (void)slen;
  return vsprintf(s, fmt, ap);
}

static char bionic_sF[3][152];

#ifndef ASSET_DIR
#define ASSET_DIR "assets"
#endif

typedef struct {
  FILE *fp;
  long len;
} CtwAAsset;

static const char *resolve_android_path(const char *path) {
  static _Thread_local char alt_path[2048];
  static _Thread_local char asset_path[2048];

  static const char *app_data_prefix = "/data/data/com.rockstargames.gtactw/";
  static const char *app_data_prefix_2 =
      "/data/user/0/com.rockstargames.gtactw/";
  static const char *sdcard_prefix =
      "/storage/emulated/0/Android/data/com.rockstargames.gtactw/";

  if (!path || path[0] == '\0')
    return path;

  if (strncmp(path, app_data_prefix, strlen(app_data_prefix)) == 0) {
    snprintf(alt_path, sizeof(alt_path), "./%s",
             path + strlen(app_data_prefix));
    return alt_path;
  }
  if (strncmp(path, app_data_prefix_2, strlen(app_data_prefix_2)) == 0) {
    snprintf(alt_path, sizeof(alt_path), "./%s",
             path + strlen(app_data_prefix_2));
    return alt_path;
  }
  if (strncmp(path, sdcard_prefix, strlen(sdcard_prefix)) == 0) {
    snprintf(alt_path, sizeof(alt_path), "./%s", path + strlen(sdcard_prefix));
    return alt_path;
  }

  if (access(path, F_OK) == 0)
    return path;

  if (path[0] == '/') {
    if (snprintf(alt_path, sizeof(alt_path), ".%s", path) <
            (int)sizeof(alt_path) &&
        access(alt_path, F_OK) == 0) {
      return alt_path;
    }
  }

  const char *basename = strrchr(path, '/');
  basename = basename ? basename + 1 : path;
  if (basename[0] != '\0') {
    if (snprintf(asset_path, sizeof(asset_path), "./%s/%s", ASSET_DIR,
                 basename) < (int)sizeof(asset_path) &&
        access(asset_path, F_OK) == 0) {
      return asset_path;
    }
  }

  return path;
}

static void *aw_fromSurface(void *env, void *surface) {
  (void)env;
  (void)surface;
  return (void *)0xAA11; /* fake ANativeWindow */
}
static int aw_setBuffersGeometry(void *w, int x, int y, int f) {
  (void)w;
  (void)x;
  (void)y;
  (void)f;
  return 0;
}
static int aw_getWidth(void *w) {
  (void)w;
  return ctw_screen_w();
}
static int aw_getHeight(void *w) {
  (void)w;
  return ctw_screen_h();
}
static void aw_release(void *w) { (void)w; }

static void *am_fromJava(void *env, void *obj) {
  (void)env;
  (void)obj;
  return (void *)0xA55E7; /* fake AAssetManager */
}

static FILE *open_asset_ci(const char *rel) {
  if (!rel || !rel[0])
    return NULL;
  char cur[1024];
  snprintf(cur, sizeof(cur), "./%s", ASSET_DIR);
  char tmp[1024];
  snprintf(tmp, sizeof(tmp), "%s", rel);
  char *save = NULL;
  for (char *tok = strtok_r(tmp, "/", &save); tok;
       tok = strtok_r(NULL, "/", &save)) {
    if (tok[0] == '\0')
      continue;
    DIR *d = opendir(cur);
    if (!d)
      return NULL;
    struct dirent *e;
    char match[1024];
    match[0] = '\0';
    while ((e = readdir(d))) {
      if (strcasecmp(e->d_name, tok) == 0) {
        snprintf(match, sizeof(match), "%s/%s", cur, e->d_name);
        break;
      }
    }
    closedir(d);
    if (!match[0])
      return NULL;
    snprintf(cur, sizeof(cur), "%s", match);
  }
  struct stat st;
  if (stat(cur, &st) != 0 || !S_ISREG(st.st_mode))
    return NULL;
  return fopen(cur, "rb");
}

static void *aa_open(void *mgr, const char *path, int mode) {
  (void)mgr;
  (void)mode;
  if (!path || path[0] == '\0')
    return NULL;
  const char *rel = path;
  if (strncmp(rel, "./assets/", 9) == 0)
    rel += 9;
  else if (strncmp(rel, "assets/", 7) == 0)
    rel += 7;
  char full[1024];
  snprintf(full, sizeof(full), "./%s/%s", ASSET_DIR, rel);
  const char *r = resolve_android_path(full);
  FILE *fp = fopen(r, "rb");
  if (!fp && r != full)
    fp = fopen(full, "rb");
  if (!fp) {
    const char *r2 = resolve_android_path(path);
    if (r2 != path && access(r2, F_OK) == 0)
      fp = fopen(r2, "rb");
  }
  if (!fp)
    fp = open_asset_ci(rel);
  if (!fp) {
    fprintf(stderr, "[fs] AAssetManager_open(\"%s\") -> FAILED\n", path);
    return NULL;
  }
  fprintf(stderr, "[fs] AAssetManager_open(\"%s\") -> OK\n", path);
  CtwAAsset *a = calloc(1, sizeof(*a));
  if (!a) {
    fclose(fp);
    return NULL;
  }
  a->fp = fp;
  fseek(fp, 0, SEEK_END);
  a->len = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  return a;
}
static int aa_read(void *h, void *buf, size_t n) {
  CtwAAsset *a = h;
  return a ? (int)fread(buf, 1, n, a->fp) : -1;
}
static long aa_seek64(void *h, long off, int wh) {
  CtwAAsset *a = h;
  if (!a)
    return -1;
  fseek(a->fp, off, wh);
  return ftell(a->fp);
}
static long aa_getLength64(void *h) {
  CtwAAsset *a = h;
  return a ? a->len : 0;
}
static long aa_getRemainingLength64(void *h) {
  CtwAAsset *a = h;
  return a ? a->len - ftell(a->fp) : 0;
}
static void aa_close(void *h) {
  CtwAAsset *a = h;
  if (a) {
    if (a->fp)
      fclose(a->fp);
    free(a);
  }
}

static int read_intent_mode(const char *mode) {
  if (!mode)
    return 0;
  if (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, 'x'))
    return 0;
  return strchr(mode, 'r') != NULL;
}

static FILE *ctw_fopen(const char *path, const char *mode) {
  if (!path || !path[0])
    return NULL;
  FILE *fp = NULL;
  if (read_intent_mode(mode)) {
    const char *r = resolve_android_path(path);
    fp = fopen(r, mode);
    if (!fp) {
      char full[1024];
      snprintf(full, sizeof(full), "./%s/%s", ASSET_DIR, path);
      fp = fopen(resolve_android_path(full), mode);
    }
    if (!fp) {
      fp = open_asset_ci(path);
    }
  } else {
    fp = fopen(resolve_android_path(path), mode);
  }

  if (fp && strchr(mode, 'r')) {
    const char *ext = strrchr(path, '.');
    if (ext && (strcasecmp(ext, ".ras") == 0 || strcasecmp(ext, ".msf") == 0 ||
                strcasecmp(ext, ".mp3") == 0)) {
      setvbuf(fp, NULL, _IOFBF, 128 * 1024);
    }
  }

  fprintf(stderr, "[fs] fopen(\"%s\", \"%s\") -> %p\n", path, mode, fp);
  return fp;
}

extern int open(const char *, int, ...);
static int ctw_open(const char *path, int flags, ...) {
  va_list ap;
  va_start(ap, flags);
  mode_t mode = va_arg(ap, mode_t);
  va_end(ap);
  int readonly = ((flags & O_ACCMODE) == O_RDONLY) &&
                 !(flags & (O_CREAT | O_WRONLY | O_TRUNC | O_APPEND));
  const char *r = readonly ? resolve_android_path(path) : path;
  int fd = open(r, flags, mode);
  if (fd < 0 && readonly && path && path[0]) {
    char full[1024];
    snprintf(full, sizeof(full), "./%s/%s", ASSET_DIR, path);
    fd = open(resolve_android_path(full), flags, mode);
  }
  fprintf(stderr, "[fs] open(\"%s\", 0x%x) -> %d\n", path, flags, fd);
  return fd;
}

struct bionic_timespec {
  int64_t tv_sec;
  int64_t tv_nsec;
};

struct bionic_stat {
  uint64_t st_dev;
  uint64_t st_ino;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint32_t st_uid;
  uint32_t st_gid;
  uint64_t st_rdev;
  uint64_t __pad1;
  int64_t st_size;
  int32_t st_blksize;
  int32_t __pad2;
  int64_t st_blocks;
  struct bionic_timespec st_atim;
  struct bionic_timespec st_mtim;
  struct bionic_timespec st_ctim;
  uint32_t __unused4;
  uint32_t __unused5;
};

static void convert_stat(const struct stat *in, struct bionic_stat *out) {
  memset(out, 0, sizeof(*out));
  out->st_dev = in->st_dev;
  out->st_ino = in->st_ino;
  out->st_mode = in->st_mode;
  out->st_nlink = in->st_nlink;
  out->st_uid = in->st_uid;
  out->st_gid = in->st_gid;
  out->st_rdev = in->st_rdev;
  out->st_size = in->st_size;
  out->st_blksize = in->st_blksize;
  out->st_blocks = in->st_blocks;
  out->st_atim.tv_sec = in->st_atim.tv_sec;
  out->st_atim.tv_nsec = in->st_atim.tv_nsec;
  out->st_mtim.tv_sec = in->st_mtim.tv_sec;
  out->st_mtim.tv_nsec = in->st_mtim.tv_nsec;
  out->st_ctim.tv_sec = in->st_ctim.tv_sec;
  out->st_ctim.tv_nsec = in->st_ctim.tv_nsec;
}

static int stat_fake(const char *path, struct bionic_stat *st) {
  struct stat real;
  int ret = stat(resolve_android_path(path), &real);
  if (ret < 0 && path && path[0]) {
    char full[1024];
    snprintf(full, sizeof(full), "./%s/%s", ASSET_DIR, path);
    ret = stat(resolve_android_path(full), &real);
  }
  if (ret == 0 && st)
    convert_stat(&real, st);
  fprintf(stderr, "[fs] stat(\"%s\") -> %d\n", path, ret);
  return ret;
}

static int fstat_fake(int fd, struct bionic_stat *st) {
  struct stat real;
  int ret = fstat(fd, &real);
  if (ret == 0 && st)
    convert_stat(&real, st);
  return ret;
}

static int lstat_fake(const char *path, struct bionic_stat *st) {
  struct stat real;
  int ret = lstat(resolve_android_path(path), &real);
  if (ret < 0 && path && path[0]) {
    char full[1024];
    snprintf(full, sizeof(full), "./%s/%s", ASSET_DIR, path);
    ret = lstat(resolve_android_path(full), &real);
  }
  if (ret == 0 && st)
    convert_stat(&real, st);
  return ret;
}

struct bionic_dirent {
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
  char d_name[256];
};

static void *readdir_fake(void *dirp) {
  static _Thread_local struct bionic_dirent out;
  struct dirent *e = readdir((DIR *)dirp);
  if (!e)
    return NULL;
  memset(&out, 0, sizeof(out));
  out.d_ino = e->d_ino;
  out.d_reclen = sizeof(out);
  out.d_type = e->d_type;
  snprintf(out.d_name, sizeof(out.d_name), "%s", e->d_name);
  return &out;
}

static int ctw_strerror_r(int errnum, char *buf, size_t buflen) {
  char *res = strerror_r(errnum, buf, buflen);
  if (res != buf) {
    strncpy(buf, res, buflen);
    buf[buflen - 1] = '\0';
  }
  return 0;
}

/* Heap allocate jmp_buf so Bionic's tiny jmp_buf struct won't be buffer
 * overflowed by glibc */
static int ctw_setjmp(void *env) {
  jmp_buf *real_env = malloc(sizeof(jmp_buf));
  *(jmp_buf **)env = real_env;
  return _setjmp(*real_env);
}

static void ctw_longjmp(void *env, int val) {
  jmp_buf *real_env = *(jmp_buf **)env;
  longjmp(*real_env, val);
}

static void b_set_abort_message(const char *m) {
  fprintf(stderr, "[abort_msg] %s\n", m ? m : "?");
}

ALCcontext *al_hook_ctx = NULL;
ALCdevice *al_hook_dev = NULL;

ALCcontext *alcCreateContextHook(ALCdevice *dev, const ALCint *unused) {
  (void)unused;
  if (al_hook_ctx)
    return al_hook_ctx;
  const ALCint attr[] = {ALC_FREQUENCY, 44100, 0};
  al_hook_ctx = alcCreateContext(dev, attr);
  return al_hook_ctx;
}

ALCdevice *alcOpenDeviceHook(const char *name) {
  if (al_hook_dev)
    return al_hook_dev;
  al_hook_dev = alcOpenDevice(name);
  return al_hook_dev;
}

void alcDestroyContextHook(ALCcontext *ctx) {
  if (ctx == al_hook_ctx)
    al_hook_ctx = NULL;
  alcDestroyContext(ctx);
}

ALCboolean alcCloseDeviceHook(ALCdevice *dev) {
  if (dev == al_hook_dev)
    al_hook_dev = NULL;
  return alcCloseDevice(dev);
}

void alSourcef_hook(ALuint source, ALenum param, ALfloat value) {
  /* Fixes CTW 4.4.243 Android libGame.so bug: cWavStream sets AL_GAIN to 0.0f
   * when creating radio stream sources and fails to update AL_GAIN during play.
   * Override 0.0f gain on stream sources so radio is audible. */
  if (param == AL_GAIN && value == 0.0f) {
    value = 1.0f;
  }
  alSourcef(source, param, value);
}

void ctw_audio_prewarm(void) {
  if (!al_hook_dev)
    al_hook_dev = alcOpenDevice(NULL);
  if (!al_hook_ctx && al_hook_dev) {
    const ALCint attr[] = {ALC_FREQUENCY, 44100, 0};
    al_hook_ctx = alcCreateContext(al_hook_dev, attr);
    alcMakeContextCurrent(al_hook_ctx);
  }
  fprintf(stderr, "[audio] prewarm: dev=%p ctx=%p\n", (void *)al_hook_dev,
          (void *)al_hook_ctx);
}

void deinit_openal(void) {
  if (al_hook_dev) {
    if (al_hook_ctx) {
      alcMakeContextCurrent(NULL);
      alcDestroyContext(al_hook_ctx);
    }
    alcCloseDevice(al_hook_dev);
  }
}

DynLibFunction dynlib_functions[] = {
    {"abort", (uintptr_t)&ctw_abort},
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
    {"ANativeWindow_setBuffersGeometry",
     (uintptr_t)&aw_setBuffersGeometry},
    {"android_set_abort_message", (uintptr_t)&b_set_abort_message},
    {"alBufferData", (uintptr_t)&alBufferData},
    {"alBufferi", (uintptr_t)&alBufferi},
    {"alcCloseDevice", (uintptr_t)&alcCloseDeviceHook},
    {"alcCreateContext", (uintptr_t)&alcCreateContextHook},
    {"alcDestroyContext", (uintptr_t)&alcDestroyContextHook},
    {"alcGetProcAddress", (uintptr_t)&alcGetProcAddress},
    {"alcMakeContextCurrent", (uintptr_t)&alcMakeContextCurrent},
    {"alcOpenDevice", (uintptr_t)&alcOpenDeviceHook},
    {"alcProcessContext", (uintptr_t)&alcProcessContext},
    {"alcSuspendContext", (uintptr_t)&alcSuspendContext},
    {"alDeleteBuffers", (uintptr_t)&alDeleteBuffers},
    {"alDeleteSources", (uintptr_t)&alDeleteSources},
    {"alDistanceModel", (uintptr_t)&alDistanceModel},
    {"alGenBuffers", (uintptr_t)&alGenBuffers},
    {"alGenSources", (uintptr_t)&alGenSources},
    {"alGetError", (uintptr_t)&alGetError},
    {"alGetSourcef", (uintptr_t)&alGetSourcef},
    {"alGetSourcei", (uintptr_t)&alGetSourcei},
    {"alListener3f", (uintptr_t)&alListener3f},
    {"alListenerfv", (uintptr_t)&alListenerfv},
    {"alSource3f", (uintptr_t)&alSource3f},
    {"alSourcef", (uintptr_t)&alSourcef_hook},
    {"alSourcei", (uintptr_t)&alSourcei},
    {"alSourcePause", (uintptr_t)&alSourcePause},
    {"alSourcePlay", (uintptr_t)&alSourcePlay},
    {"alSourceQueueBuffers", (uintptr_t)&alSourceQueueBuffers},
    {"alSourceStop", (uintptr_t)&alSourceStop},
    {"alSourceUnqueueBuffers", (uintptr_t)&alSourceUnqueueBuffers},
    {"_ZTHN10ALCcontext13sLocalContextE", (uintptr_t)&ret0},
    {"__android_log_print", (uintptr_t)&b_android_log},
    {"asinf", (uintptr_t)&asinf},
    {"atan", (uintptr_t)&atan},
    {"atan2f", (uintptr_t)&atan2f},
    {"atanf", (uintptr_t)&atanf},
    {"atof", (uintptr_t)&atof},
    {"atoi", (uintptr_t)&atoi},
    {"calloc", (uintptr_t)&calloc},
    {"close", (uintptr_t)&close},
    {"cosf", (uintptr_t)&cosf},

    {"__cxa_atexit", (uintptr_t)&__cxa_atexit},
    {"__cxa_finalize", (uintptr_t)&__cxa_finalize},
    {"__cxa_thread_atexit_impl",
     (uintptr_t)&__cxa_thread_atexit_impl_fake},
    {"dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr},
    {"eglChooseConfig", (uintptr_t)&eglChooseConfig_shim},
    {"eglCreateContext", (uintptr_t)&eglCreateContext_shim},
    {"eglCreateWindowSurface",
     (uintptr_t)&eglCreateWindowSurface_shim},
    {"eglDestroySurface", (uintptr_t)&eglDestroySurface_shim},
    {"eglGetConfigAttrib", (uintptr_t)&eglGetConfigAttrib_shim},
    {"eglGetDisplay", (uintptr_t)&eglGetDisplay_shim},
    {"eglGetProcAddress", (uintptr_t)&eglGetProcAddress_shim},
    {"eglInitialize", (uintptr_t)&eglInitialize_shim},
    {"eglMakeCurrent", (uintptr_t)&eglMakeCurrent_shim},
    {"eglQueryString", (uintptr_t)&eglQueryString_shim},
    {"eglSwapBuffers", (uintptr_t)&eglSwapBuffers_shim},
    {"eglSwapInterval", (uintptr_t)&eglSwapInterval_shim},
    {"__errno", (uintptr_t)&bionic___errno},
    {"expf", (uintptr_t)&expf},
    {"fclose", (uintptr_t)&fclose},
    {"feof", (uintptr_t)&feof},
    {"ferror", (uintptr_t)&ferror},
    {"fflush", (uintptr_t)&fflush},
    {"fgetc", (uintptr_t)&fgetc},
    {"fgets", (uintptr_t)&fgets},
    {"fopen", (uintptr_t)&ctw_fopen},
    {"fprintf", (uintptr_t)&fprintf},
    {"fputc", (uintptr_t)&fputc},
    {"fread", (uintptr_t)&fread},
    {"free", (uintptr_t)&free},
    {"frexp", (uintptr_t)&frexp},
    {"fseek", (uintptr_t)&fseek},
    {"fstat", (uintptr_t)&fstat_fake},
    {"ftell", (uintptr_t)&ftell},
    {"fwrite", (uintptr_t)&fwrite},
    {"gettid", (uintptr_t)&b_gettid},
    {"gettimeofday", (uintptr_t)&gettimeofday},

    {"glActiveTexture", (uintptr_t)&glActiveTexture},
    {"glAttachShader", (uintptr_t)&glAttachShader},
    {"glBindAttribLocation", (uintptr_t)&glBindAttribLocation},
    {"glBindBuffer", (uintptr_t)&glBindBuffer},
    {"glBindFramebuffer", (uintptr_t)&glBindFramebuffer},
    {"glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer},
    {"glBindTexture", (uintptr_t)&glBindTexture},
    {"glBlendEquation", (uintptr_t)&glBlendEquation},
    {"glBlendFunc", (uintptr_t)&glBlendFunc},
    {"glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus},
    {"glClear", (uintptr_t)&glClear},
    {"glClearColor", (uintptr_t)&glClearColor},
    {"glColorMask", (uintptr_t)&glColorMask},
    {"glCompileShader", (uintptr_t)&glCompileShader},
    {"glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D},
    {"glCreateProgram", (uintptr_t)&glCreateProgram},
    {"glCreateShader", (uintptr_t)&glCreateShader},
    {"glCullFace", (uintptr_t)&glCullFace},
    {"glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers},
    {"glDeleteProgram", (uintptr_t)&glDeleteProgram},
    {"glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers},
    {"glDeleteShader", (uintptr_t)&glDeleteShader},
    {"glDeleteTextures", (uintptr_t)&glDeleteTextures},
    {"glDepthFunc", (uintptr_t)&glDepthFunc},
    {"glDepthMask", (uintptr_t)&glDepthMask},
    {"glDisable", (uintptr_t)&glDisable},
    {"glDisableVertexAttribArray",
     (uintptr_t)&glDisableVertexAttribArray},
    {"glDrawArrays", (uintptr_t)&glDrawArrays},
    {"glEnable", (uintptr_t)&glEnable},
    {"glEnableVertexAttribArray",
     (uintptr_t)&glEnableVertexAttribArray},
    {"glFramebufferRenderbuffer",
     (uintptr_t)&glFramebufferRenderbuffer},
    {"glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D},
    {"glGenFramebuffers", (uintptr_t)&glGenFramebuffers},
    {"glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers},
    {"glGenTextures", (uintptr_t)&glGenTextures},
    {"glGetIntegerv", (uintptr_t)&glGetIntegerv},
    {"glGetProgramiv", (uintptr_t)&glGetProgramiv},
    {"glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog},
    {"glGetShaderiv", (uintptr_t)&glGetShaderiv},
    {"glGetString", (uintptr_t)&glGetString},
    {"glGetUniformLocation", (uintptr_t)&glGetUniformLocation},
    {"glLineWidth", (uintptr_t)&glLineWidth},
    {"glLinkProgram", (uintptr_t)&glLinkProgram},
    {"glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage},
    {"glScissor", (uintptr_t)&glScissor},
    {"glShaderSource", (uintptr_t)&glShaderSource},
    {"glTexImage2D", (uintptr_t)&glTexImage2D},
    {"glTexParameteri", (uintptr_t)&glTexParameteri},
    {"glUniform1i", (uintptr_t)&glUniform1i},
    {"glUniform3fv", (uintptr_t)&glUniform3fv},
    {"glUniform4f", (uintptr_t)&glUniform4f},
    {"glUniformMatrix3fv", (uintptr_t)&glUniformMatrix3fv},
    {"glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv},
    {"glUseProgram", (uintptr_t)&glUseProgram},
    {"glVertexAttrib2f", (uintptr_t)&glVertexAttrib2f},
    {"glVertexAttrib3f", (uintptr_t)&glVertexAttrib3f},
    {"glVertexAttrib4f", (uintptr_t)&glVertexAttrib4f},
    {"glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer},
    {"glViewport", (uintptr_t)&glViewport},
    {"gmtime", (uintptr_t)&gmtime},
    {"localtime", (uintptr_t)&localtime},
    {"logf", (uintptr_t)&logf},
    {"longjmp", (uintptr_t)&ctw_longjmp},
    {"lseek", (uintptr_t)&lseek},
    {"lstat", (uintptr_t)&lstat_fake},
    {"malloc", (uintptr_t)&malloc},
    {"memchr", (uintptr_t)&memchr},
    {"memcmp", (uintptr_t)&memcmp},
    {"memcpy", (uintptr_t)&memcpy},
    {"__memcpy_chk", (uintptr_t)&b_memcpy_chk},
    {"memmove", (uintptr_t)&memmove},
    {"memset", (uintptr_t)&memset},
    {"mkdir", (uintptr_t)&mkdir},
    {"modf", (uintptr_t)&modf},

    {"mpg123_delete", (uintptr_t)&mpg123_delete},
    {"mpg123_exit", (uintptr_t)&mpg123_exit},
    {"mpg123_feed", (uintptr_t)&mpg123_feed},
    {"mpg123_feedseek", (uintptr_t)&mpg123_feedseek},
    {"mpg123_format_all", (uintptr_t)&mpg123_format_all},
    {"mpg123_getformat", (uintptr_t)&mpg123_getformat},
    {"mpg123_info", (uintptr_t)&mpg123_info},
    {"mpg123_init", (uintptr_t)&mpg123_init},
    {"mpg123_new", (uintptr_t)&mpg123_new},
    {"mpg123_open_feed", (uintptr_t)&mpg123_open_feed},
    {"mpg123_outblock", (uintptr_t)&mpg123_outblock},
    {"mpg123_param", (uintptr_t)&mpg123_param},
    {"mpg123_read", (uintptr_t)&mpg123_read},
    {"nanosleep", (uintptr_t)&nanosleep},
    {"open", (uintptr_t)&ctw_open},
    {"posix_memalign", (uintptr_t)&posix_memalign},
    {"pow", (uintptr_t)&pow},
    {"printf", (uintptr_t)&printf},
    {"pthread_attr_destroy",
     (uintptr_t)&pthread_attr_destroy_fake},
    {"pthread_attr_getschedparam",
     (uintptr_t)&pthread_attr_getschedparam_fake},
    {"pthread_attr_getstacksize",
     (uintptr_t)&pthread_attr_getstacksize_fake},
    {"pthread_attr_init",
     (uintptr_t)&pthread_attr_init_fake},
    {"pthread_attr_setschedparam",
     (uintptr_t)&pthread_attr_setschedparam_fake},
    {"pthread_attr_setstacksize",
     (uintptr_t)&pthread_attr_setstacksize_fake},
    {"pthread_cond_broadcast",
     (uintptr_t)&pthread_cond_broadcast_fake},
    {"pthread_cond_destroy",
     (uintptr_t)&pthread_cond_destroy_fake},
    {"pthread_cond_init",
     (uintptr_t)&pthread_cond_init_fake},
    {"pthread_cond_signal",
     (uintptr_t)&pthread_cond_signal_fake},
    {"pthread_cond_timedwait",
     (uintptr_t)&pthread_cond_timedwait_fake},
    {"pthread_cond_wait",
     (uintptr_t)&pthread_cond_wait_fake},
    {"pthread_create",
     (uintptr_t)&pthread_create_fake},
    {"pthread_detach",
     (uintptr_t)&pthread_detach_fake},
    {"pthread_getspecific",
     (uintptr_t)&pthread_getspecific_fake},
    {"pthread_join", (uintptr_t)&pthread_join_fake},
    {"pthread_key_create",
     (uintptr_t)&pthread_key_create_fake},
    {"pthread_key_delete",
     (uintptr_t)&pthread_key_delete_fake},
    {"pthread_mutexattr_destroy",
     (uintptr_t)&pthread_mutexattr_destroy_fake},
    {"pthread_mutexattr_init",
     (uintptr_t)&pthread_mutexattr_init_fake},
    {"pthread_mutexattr_settype",
     (uintptr_t)&pthread_mutexattr_settype_fake},
    {"pthread_mutex_destroy",
     (uintptr_t)&pthread_mutex_destroy_fake},
    {"pthread_mutex_init",
     (uintptr_t)&pthread_mutex_init_fake},
    {"pthread_mutex_lock",
     (uintptr_t)&pthread_mutex_lock_fake},
    {"pthread_mutex_trylock",
     (uintptr_t)&pthread_mutex_trylock_fake},
    {"pthread_mutex_unlock",
     (uintptr_t)&pthread_mutex_unlock_fake},
    {"pthread_once", (uintptr_t)&pthread_once_fake},
    {"pthread_rwlock_rdlock",
     (uintptr_t)&pthread_rwlock_rdlock_fake},
    {"pthread_rwlock_unlock",
     (uintptr_t)&pthread_rwlock_unlock_fake},
    {"pthread_rwlock_wrlock",
     (uintptr_t)&pthread_rwlock_wrlock_fake},
    {"pthread_self", (uintptr_t)&pthread_self_fake},
    {"pthread_setname_np",
     (uintptr_t)&pthread_setname_np_fake},
    {"pthread_setschedparam",
     (uintptr_t)&pthread_setschedparam_fake},
    {"pthread_setspecific",
     (uintptr_t)&pthread_setspecific_fake},
    {"putchar", (uintptr_t)&putchar},
    {"puts", (uintptr_t)&puts},
    {"rand", (uintptr_t)&rand},
    {"__read_chk", (uintptr_t)&b_read_chk},
    {"readdir", (uintptr_t)&readdir_fake},
    {"realloc", (uintptr_t)&realloc},
    {"__register_atfork", (uintptr_t)&__register_atfork},
    {"remove", (uintptr_t)&remove},
    {"rename", (uintptr_t)&rename},
    {"sched_get_priority_max", (uintptr_t)&sched_get_priority_max},
    {"sched_get_priority_min", (uintptr_t)&sched_get_priority_min},
    {"sem_destroy", (uintptr_t)&sem_destroy_fake},
    {"sem_getvalue", (uintptr_t)&sem_getvalue_fake},
    {"sem_init", (uintptr_t)&sem_init_fake},
    {"sem_post", (uintptr_t)&sem_post_fake},
    {"sem_trywait", (uintptr_t)&sem_trywait_fake},
    {"sem_wait", (uintptr_t)&sem_wait_fake},
    {"setjmp", (uintptr_t)&ctw_setjmp},
    {"__sF", (uintptr_t)&bionic_sF},
    {"sincosf", (uintptr_t)&sincosf},
    {"sinf", (uintptr_t)&sinf},
    {"srand", (uintptr_t)&srand},
    {"__stack_chk_fail", (uintptr_t)&ctw_stack_chk_fail},
    {"stat", (uintptr_t)&stat_fake},
    {"stderr", (uintptr_t)&stderr},
    {"strcasecmp", (uintptr_t)&strcasecmp},
    {"strcat", (uintptr_t)&strcat},
    {"__strcat_chk", (uintptr_t)&b_strcat_chk},
    {"strchr", (uintptr_t)&strchr},
    {"strcmp", (uintptr_t)&strcmp},
    {"strcpy", (uintptr_t)&strcpy},
    {"__strcpy_chk", (uintptr_t)&b_strcpy_chk},
    {"strerror", (uintptr_t)&strerror},
    {"strerror_r", (uintptr_t)&ctw_strerror_r},
    {"strlen", (uintptr_t)&strlen},
    {"__strlen_chk", (uintptr_t)&b_strlen_chk},
    {"strncmp", (uintptr_t)&strncmp},
    {"strncpy", (uintptr_t)&strncpy},
    {"__strncpy_chk", (uintptr_t)&b_strncpy_chk},
    {"strstr", (uintptr_t)&strstr},
    {"strtol", (uintptr_t)&strtol},
    {"syscall", (uintptr_t)&syscall},
    {"time", (uintptr_t)&time},
    {"unlink", (uintptr_t)&unlink},
    {"usleep", (uintptr_t)&usleep},
    {"vsnprintf", (uintptr_t)&vsnprintf},
    {"__vsnprintf_chk", (uintptr_t)&b_vsnprintf_chk},
    {"__vsprintf_chk", (uintptr_t)&b_vsprintf_chk},
    {"__write_chk", (uintptr_t)&b_write_chk},
};
const int dynlib_functions_count =
    sizeof(dynlib_functions) / sizeof(dynlib_functions[0]);

void ctw_imports_init(void) {}
