#define _GNU_SOURCE

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "egl_shim.h"
#include "jni_shim.h"
#include "music_player.h"
#include "util.h"

#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006

typedef uint64_t juint;

volatile int jni_quit_requested = 0;

enum {
  TAG_OBJECT = 0x4f424a31, /* 'OBJ1' */
  TAG_STRING = 0x53545231, /* 'STR1' */
  TAG_OBJARR = 0x4f415231, /* 'OAR1' */
  TAG_PRIARR = 0x50415231, /* 'PAR1' */
  TAG_ID = 0x4d494431,     /* 'MID1' */
};

typedef struct {
  uint32_t tag;
  char label[96];
} FakeObject;

typedef struct {
  uint32_t tag;
  char *utf;
} FakeString;

typedef struct {
  uint32_t tag;
  int len;
  void **items;
} FakeObjArray;

typedef struct {
  uint32_t tag;
  int len;
  int elem_size;
  void *data;
} FakePriArray;

typedef struct {
  uint32_t tag;
  char name[80];
  char sig[80];
} FakeID;

void *jni_make_object(const char *label) {
  FakeObject *o = calloc(1, sizeof(*o));
  o->tag = TAG_OBJECT;
  if (label)
    strncpy(o->label, label, sizeof(o->label) - 1);
  return o;
}

void *jni_make_string(const char *utf) {
  FakeString *s = calloc(1, sizeof(*s));
  s->tag = TAG_STRING;
  s->utf = strdup(utf ? utf : "");
  return s;
}

void *jni_make_string_array(int n, const char **strs) {
  FakeObjArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_OBJARR;
  a->len = n;
  a->items = calloc((n ? n : 1), sizeof(void *));
  for (int i = 0; i < n; i++)
    a->items[i] = jni_make_string(strs[i]);
  return a;
}

void *jni_make_int_array(int n, const int *vals) {
  FakePriArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_PRIARR;
  a->len = n;
  a->elem_size = sizeof(int);
  a->data = calloc((n ? n : 1), sizeof(int));
  if (vals)
    memcpy(a->data, vals, n * sizeof(int));
  return a;
}

static const char *obj_str(void *jstr) {
  FakeString *s = jstr;
  if (s && s->tag == TAG_STRING)
    return s->utf;
  return "<not-a-string>";
}

#define KV_MAX 128
typedef struct {
  char key[64];
  char val[512];
} KV;

static KV kv_store[KV_MAX];
static int kv_count = 0;

static const char *kv_get(const char *key) {
  if (!key)
    return NULL;
  for (int i = 0; i < kv_count; i++)
    if (strcmp(kv_store[i].key, key) == 0)
      return kv_store[i].val;
  return NULL;
}

static void kv_set(const char *key, const char *val) {
  if (!key)
    return;
  for (int i = 0; i < kv_count; i++) {
    if (strcmp(kv_store[i].key, key) == 0) {
      strncpy(kv_store[i].val, val ? val : "", sizeof(kv_store[i].val) - 1);
      return;
    }
  }
  if (kv_count < KV_MAX) {
    strncpy(kv_store[kv_count].key, key, sizeof(kv_store[kv_count].key) - 1);
    strncpy(kv_store[kv_count].val, val ? val : "",
            sizeof(kv_store[kv_count].val) - 1);
    kv_count++;
  }
}

#define PLAYLIST_QUEUE_LEN 8
static JniPlaylistEvent playlist_queue[PLAYLIST_QUEUE_LEN];
static int playlist_head = 0, playlist_tail = 0;

static void push_playlist(int success, int count) {
  const int next = (playlist_tail + 1) % PLAYLIST_QUEUE_LEN;
  if (next != playlist_head) {
    playlist_queue[playlist_tail].success = success;
    playlist_queue[playlist_tail].count = count;
    playlist_tail = next;
  }
}

int jni_pop_pending_playlist(JniPlaylistEvent *event) {
  if (playlist_head == playlist_tail)
    return 0;
  if (event)
    *event = playlist_queue[playlist_head];
  playlist_head = (playlist_head + 1) % PLAYLIST_QUEUE_LEN;
  return 1;
}

#define ROCKSTAR_QUEUE_LEN 32
static JniRockstarEvent rockstar_queue[ROCKSTAR_QUEUE_LEN];
static int rockstar_head = 0, rockstar_tail = 0;

void jni_push_rockstar(JniRockstarEventType type, int accepted, int signed_in,
                       const char *value, const char *environment,
                       const char *title_id) {
  const int next = (rockstar_tail + 1) % ROCKSTAR_QUEUE_LEN;
  if (next == rockstar_head) {
    debugPrintf("JNI: dropped rockstar event %d (queue full)\n", type);
    return;
  }
  JniRockstarEvent e;
  memset(&e, 0, sizeof(e));
  e.type = type;
  e.accepted = accepted;
  e.signed_in = signed_in;
  if (value)
    e.value = jni_make_string(value);
  if (environment)
    e.environment = jni_make_string(environment);
  if (title_id)
    e.title_id = jni_make_string(title_id);
  rockstar_queue[rockstar_tail] = e;
  rockstar_tail = next;
}

int jni_pop_pending_rockstar(JniRockstarEvent *event) {
  if (rockstar_head == rockstar_tail)
    return 0;
  if (event)
    *event = rockstar_queue[rockstar_head];
  rockstar_head = (rockstar_head + 1) % ROCKSTAR_QUEUE_LEN;
  return 1;
}

#define MAX_IDS 256
static FakeID id_pool[MAX_IDS];
static int id_count = 0;

static FakeID *get_id(const char *name, const char *sig) {
  for (int i = 0; i < id_count; i++) {
    if (!strcmp(id_pool[i].name, name) && !strcmp(id_pool[i].sig, sig))
      return &id_pool[i];
  }
  if (id_count >= MAX_IDS) {
    debugPrintf("JNI: id pool exhausted!\n");
    return &id_pool[0];
  }
  FakeID *id = &id_pool[id_count++];
  id->tag = TAG_ID;
  strncpy(id->name, name ? name : "", sizeof(id->name) - 1);
  strncpy(id->sig, sig ? sig : "", sizeof(id->sig) - 1);
  return id;
}

static juint call_boolean(const char *name, va_list va) {
  if (!strcmp(name, "isMoviePlaying"))
    return 0;
  if (!strcmp(name, "isSplashVisible"))
    return 0;
  if (!strcmp(name, "hasAppLocalValue")) {
    const char *key = obj_str(va_arg(va, void *));
    const int has = kv_get(key) != NULL;
    debugPrintf("JNI: hasAppLocalValue(%s) -> %d\n", key, has);
    return has;
  }
  if (!strcmp(name, "playlistIsPlaying"))
    return music_playlist_is_playing() ? 1 : 0;
  debugPrintf("JNI: CallBooleanMethod(%s) -> false\n", name);
  return 0;
}

static juint call_int(const char *name, va_list va) {
  (void)va;
  if (!strcmp(name, "getDeviceLocale")) {
    const char *e = getenv("CTW_LOCALE");
    /* CTW locale enum: 0 = English (see gtactw_nx config.language=0). Default
     * to 0; 1 would select French (f_*.gxt). Override with CTW_LOCALE=n. */
    const int locale = e ? atoi(e) : 0;
    debugPrintf("JNI: getDeviceLocale -> %d\n", locale);
    return locale;
  }
  if (!strcmp(name, "playlistCount"))
    return music_playlist_count();
  debugPrintf("JNI: CallIntMethod(%s) -> 0\n", name);
  return 0;
}

static void *call_object(const char *name, va_list va) {
  if (!strcmp(name, "getAppLocalValue")) {
    const char *key = obj_str(va_arg(va, void *));
    const char *val = kv_get(key);

    /* FIX: If an audio/volume key is requested and has not been explicitly
     * saved yet, default to "1.0" or "10" instead of NULL (which converts to
     * 0.0f/muted in C++). */
    if (!val && key) {
      if (strcasestr(key, "volume") || strcasestr(key, "audio") ||
          strcasestr(key, "music") || strcasestr(key, "radio")) {
        debugPrintf(
            "JNI: getAppLocalValue(%s) [UNSET] -> defaulting to '1.0'\n", key);
        return jni_make_string("1.0");
      }
      if (strcasestr(key, "station")) {
        debugPrintf("JNI: getAppLocalValue(%s) [UNSET] -> defaulting to '1'\n",
                    key);
        return jni_make_string("1");
      }
    }

    debugPrintf("JNI: getAppLocalValue(%s) -> %s\n", key, val ? val : "(null)");
    return val ? jni_make_string(val) : NULL;
  }
  if (!strcmp(name, "getParameter")) {
    const char *key = obj_str(va_arg(va, void *));
    debugPrintf("JNI: getParameter(%s) -> null\n", key);
    return NULL;
  }
  if (!strcmp(name, "getInstance")) {
    static void *singleton = NULL;
    if (!singleton)
      singleton = jni_make_object("NvUtil");
    debugPrintf("JNI: CallObjectMethod(getInstance) -> %p\n", singleton);
    return singleton;
  }
  debugPrintf("JNI: CallObjectMethod(%s) -> null\n", name);
  return NULL;
}

static void call_void(const char *name, va_list va) {
  if (!strcmp(name, "playMovie")) {
    void *path = va_arg(va, void *);
    const int skippable = va_arg(va, int);
    debugPrintf("JNI: playMovie(%s, skippable=%d) -- no player\n",
                obj_str(path), skippable);
    return;
  }
  if (!strcmp(name, "stopMovie")) {
    debugPrintf("JNI: stopMovie\n");
    return;
  }
  if (!strcmp(name, "setMovieText")) {
    void *str = va_arg(va, void *);
    debugPrintf("JNI: setMovieText(%s)\n", str ? obj_str(str) : "");
    return;
  }
  if (!strcmp(name, "setAppLocalValue")) {
    void *key = va_arg(va, void *);
    void *val = va_arg(va, void *);
    debugPrintf("JNI: setAppLocalValue(%s, %s)\n", obj_str(key), obj_str(val));
    kv_set(obj_str(key), obj_str(val));
    return;
  }
  if (!strcmp(name, "playlistOpen")) {
    void *str = va_arg(va, void *);
    const int n = music_playlist_open(obj_str(str));
    debugPrintf("JNI: playlistOpen(%s) -> %s (%d tracks)\n", obj_str(str),
                n > 0 ? "ok" : "fail", n);
    push_playlist(n > 0 ? 1 : 0, n > 0 ? n : 0);
    return;
  }
  if (!strcmp(name, "playlistPlay")) {
    music_playlist_play();
    return;
  }
  if (!strcmp(name, "playlistPause")) {
    music_playlist_pause();
    return;
  }
  if (!strcmp(name, "playlistStop")) {
    music_playlist_stop();
    return;
  }
  if (!strcmp(name, "rockstarShowInitial") ||
      !strcmp(name, "rockstarShowGate") || !strcmp(name, "showSignInDialog") ||
      !strcmp(name, "showGateDialog")) {
    debugPrintf("JNI: %s ignored\n", name);
    return;
  }
  debugPrintf("JNI: CallVoidMethod(%s) ignored\n", name);
}

static juint get_boolean_field(const char *name) {
  juint v = 0;
  if (!strcmp(name, "isTvDevice"))     v = 1;
  if (!strcmp(name, "hasTouchScreen")) v = 1;
  if (!strcmp(name, "isPhone")) {
    const char *e = getenv("CTW_PHONE");
    v = (e && atoi(e)) ? 1 : 0;
  }
  // if (!strcmp(name, "hasVibrator"))    v = 0;
  debugPrintf("JNI: GetBooleanField(%s) -> %d\n", name, (int)v);
  return v;
}

static juint get_int_field(const char *name) {
  juint v = 0;
  if (!strcmp(name, "osVersion"))
    v = 30; /* Android 11 */
  if (!strcmp(name, "cpuFrequency"))
    v = 2500;
  debugPrintf("JNI: GetIntField(%s) -> %d\n", name, (int)v);
  return v;
}

static void *get_object_field(const char *name) {
  if (!strcmp(name, "manufacturer"))
    return jni_make_string("Linux");
  if (!strcmp(name, "model"))
    return jni_make_string(getenv("CTW_MODEL") ? getenv("CTW_MODEL") : "PC");
  if (!strcmp(name, "hardware"))
    return jni_make_string("x86_64");
  if (!strcmp(name, "product"))
    return jni_make_string("linux");
  debugPrintf("JNI: GetObjectField(%s) -> null\n", name);
  return NULL;
}

static juint j_GetVersion(void *env) {
  (void)env;
  return JNI_VERSION_1_6;
}

static void *j_FindClass(void *env, const char *name) {
  (void)env;
  debugPrintf("JNI: FindClass(%s)\n", name);
  return jni_make_object(name);
}

static void *j_GetMethodID(void *env, void *cls, const char *name,
                           const char *sig) {
  (void)env;
  (void)cls;
  debugPrintf("JNI: GetMethodID(%s %s)\n", name, sig);
  return get_id(name, sig);
}

static void *j_GetFieldID(void *env, void *cls, const char *name,
                          const char *sig) {
  (void)env;
  (void)cls;
  debugPrintf("JNI: GetFieldID(%s %s)\n", name, sig);
  return get_id(name, sig);
}

static void *j_GetObjectClass(void *env, void *obj) {
  (void)env;
  (void)obj;
  return jni_make_object("class");
}

static void *j_NewGlobalRef(void *env, void *obj) {
  (void)env;
  return obj;
}
static void *j_NewLocalRef(void *env, void *obj) {
  (void)env;
  return obj;
}
static juint j_ret0_2(void *env, void *a) {
  (void)env;
  (void)a;
  return 0;
}
static juint j_ret0_3(void *env, void *a, void *b) {
  (void)env;
  (void)a;
  (void)b;
  return 0;
}

static juint j_CallBooleanMethodV(void *env, void *obj, FakeID *id,
                                  va_list va) {
  (void)env;
  (void)obj;
  return call_boolean(id->name, va);
}
static juint j_CallBooleanMethod(void *env, void *obj, FakeID *id, ...) {
  va_list va;
  va_start(va, id);
  juint r = call_boolean(id->name, va);
  va_end(va);
  return r;
}

static juint j_CallIntMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env;
  (void)obj;
  return call_int(id->name, va);
}
static juint j_CallIntMethod(void *env, void *obj, FakeID *id, ...) {
  va_list va;
  va_start(va, id);
  juint r = call_int(id->name, va);
  va_end(va);
  return r;
}

static void *j_CallObjectMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env;
  (void)obj;
  return call_object(id->name, va);
}
static void *j_CallObjectMethod(void *env, void *obj, FakeID *id, ...) {
  va_list va;
  va_start(va, id);
  void *r = call_object(id->name, va);
  va_end(va);
  return r;
}

static void j_CallVoidMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env;
  (void)obj;
  call_void(id->name, va);
}
static void j_CallVoidMethod(void *env, void *obj, FakeID *id, ...) {
  va_list va;
  va_start(va, id);
  call_void(id->name, va);
  va_end(va);
}

static juint j_CallLongMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env;
  (void)obj;
  (void)va;
  debugPrintf("JNI: CallLongMethod(%s) -> 0\n", id->name);
  return 0;
}

static float j_CallFloatMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env;
  (void)obj;
  (void)va;
  debugPrintf("JNI: CallFloatMethod(%s) -> 0\n", id->name);
  return 0.0f;
}

static void *j_CallStaticObjectMethodV(void *env, void *cls, FakeID *id,
                                       va_list va) {
  (void)env;
  (void)cls;
  return call_object(id->name, va);
}
static void *j_CallStaticObjectMethod(void *env, void *cls, FakeID *id, ...) {
  va_list va;
  va_start(va, id);
  void *r = call_object(id->name, va);
  va_end(va);
  return r;
}

static void j_CallStaticVoidMethodV(void *env, void *cls, FakeID *id,
                                    va_list va) {
  (void)env;
  (void)cls;
  call_void(id->name, va);
}
static void j_CallStaticVoidMethod(void *env, void *cls, FakeID *id, ...) {
  va_list va;
  va_start(va, id);
  call_void(id->name, va);
  va_end(va);
}

static void *j_NewObjectV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env;
  (void)va;
  FakeObject *c = cls;
  debugPrintf("JNI: NewObject(%s)\n",
              (c && c->tag == TAG_OBJECT) ? c->label : "?");
  return jni_make_object("newobject");
}
static void *j_NewObject(void *env, void *cls, FakeID *id, ...) {
  va_list va;
  va_start(va, id);
  void *r = j_NewObjectV(env, cls, id, va);
  va_end(va);
  return r;
}

static void *j_GetObjectField(void *env, void *obj, FakeID *id) {
  (void)env;
  (void)obj;
  return get_object_field(id->name);
}
static juint j_GetBooleanField(void *env, void *obj, FakeID *id) {
  (void)env;
  (void)obj;
  return get_boolean_field(id->name);
}
static juint j_GetIntField(void *env, void *obj, FakeID *id) {
  (void)env;
  (void)obj;
  return get_int_field(id->name);
}
static juint j_GetLongField(void *env, void *obj, FakeID *id) {
  (void)env;
  (void)obj;
  debugPrintf("JNI: GetLongField(%s) -> 0\n", id->name);
  return 0;
}
static float j_GetFloatField(void *env, void *obj, FakeID *id) {
  (void)env;
  (void)obj;
  debugPrintf("JNI: GetFloatField(%s) -> 0\n", id->name);
  return 0.0f;
}

static void *j_NewStringUTF(void *env, const char *utf) {
  (void)env;
  return jni_make_string(utf);
}

static const char *j_GetStringUTFChars(void *env, void *jstr,
                                       uint8_t *is_copy) {
  (void)env;
  if (is_copy)
    *is_copy = 0;
  return obj_str(jstr);
}

static void j_ReleaseStringUTFChars(void *env, void *jstr, const char *utf) {
  (void)env;
  (void)jstr;
  (void)utf;
}

static juint j_GetStringUTFLength(void *env, void *jstr) {
  (void)env;
  return strlen(obj_str(jstr));
}

static juint j_GetStringLength(void *env, void *jstr) {
  (void)env;
  return strlen(obj_str(jstr));
}

static const uint16_t *j_GetStringChars(void *env, void *jstr,
                                        uint8_t *is_copy) {
  (void)env;
  if (is_copy)
    *is_copy = 1;
  const char *utf = obj_str(jstr);
  const int len = strlen(utf);
  uint16_t *out = calloc(len + 1, sizeof(uint16_t));
  for (int i = 0; i < len; i++)
    out[i] = (uint8_t)utf[i];
  return out;
}

static void j_ReleaseStringChars(void *env, void *jstr, const uint16_t *chars) {
  (void)env;
  (void)jstr;
  free((void *)chars);
}

static juint j_GetArrayLength(void *env, void *arr) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && (a->tag == TAG_OBJARR || a->tag == TAG_PRIARR))
    return a->len;
  debugPrintf("JNI: GetArrayLength on non-array %p\n", arr);
  return 0;
}

static void *j_GetObjectArrayElement(void *env, void *arr, int idx) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && a->tag == TAG_OBJARR && idx >= 0 && idx < a->len)
    return a->items[idx];
  debugPrintf("JNI: GetObjectArrayElement OOB %p[%d] -> empty string\n", arr,
              idx);
  return jni_make_string("");
}

static void j_SetObjectArrayElement(void *env, void *arr, int idx, void *val) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && a->tag == TAG_OBJARR && idx >= 0 && idx < a->len)
    a->items[idx] = val;
}

static void *j_NewObjectArray(void *env, int len, void *cls, void *init) {
  (void)env;
  (void)cls;
  FakeObjArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_OBJARR;
  a->len = len;
  a->items = calloc(len ? len : 1, sizeof(void *));
  for (int i = 0; i < len; i++)
    a->items[i] = init;
  return a;
}

static void *new_pri_array(int len, int elem_size) {
  FakePriArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_PRIARR;
  a->len = len;
  a->elem_size = elem_size;
  a->data = calloc(len ? len : 1, elem_size);
  return a;
}

static void *j_NewByteArray(void *env, int len) {
  (void)env;
  return new_pri_array(len, 1);
}
static void *j_NewIntArray(void *env, int len) {
  (void)env;
  return new_pri_array(len, 4);
}
static void *j_NewFloatArray(void *env, int len) {
  (void)env;
  return new_pri_array(len, 4);
}

static void *j_GetPriArrayElements(void *env, void *arr, uint8_t *is_copy) {
  (void)env;
  if (is_copy)
    *is_copy = 0;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR)
    return a->data;
  return NULL;
}

static void j_ReleasePriArrayElements(void *env, void *arr, void *elems,
                                      int mode) {
  (void)env;
  (void)arr;
  (void)elems;
  (void)mode;
}

static void j_GetPriArrayRegion(void *env, void *arr, int start, int len,
                                void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy(buf, (char *)a->data + start * a->elem_size, len * a->elem_size);
}

static void j_SetPriArrayRegion(void *env, void *arr, int start, int len,
                                const void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy((char *)a->data + start * a->elem_size, buf, len * a->elem_size);
}

static juint j_RegisterNatives(void *env, void *cls, void *methods, int n) {
  (void)env;
  (void)cls;
  (void)methods;
  debugPrintf("JNI: RegisterNatives(%d methods) ignored\n", n);
  return 0;
}

static juint j_GetJavaVM(void *env, void **vm) {
  (void)env;
  *vm = fake_vm;
  return JNI_OK;
}

static juint j_ExceptionCheck(void *env) {
  (void)env;
  return 0;
}
static void *j_ExceptionOccurred(void *env) {
  (void)env;
  return NULL;
}
static void j_ExceptionClearDescribe(void *env) { (void)env; }
static void j_DeleteRef(void *env, void *obj) {
  (void)env;
  (void)obj;
}
static juint j_PushLocalFrame(void *env, int cap) {
  (void)env;
  (void)cap;
  return 0;
}
static void *j_PopLocalFrame(void *env, void *result) {
  (void)env;
  return result;
}

static juint j_unimplemented(void) {
  debugPrintf("JNI: call to unimplemented function slot\n");
  return 0;
}

static void *env_table[233];
static void **env_table_ptr = env_table;
void *fake_env = &env_table_ptr;

static juint vm_DestroyJavaVM(void *vm) {
  (void)vm;
  return JNI_OK;
}
static juint vm_AttachCurrentThread(void *vm, void **env, void *args) {
  (void)vm;
  (void)args;
  if (env)
    *env = fake_env;
  return JNI_OK;
}
static juint vm_DetachCurrentThread(void *vm) {
  (void)vm;
  return JNI_OK;
}
static juint vm_GetEnv(void *vm, void **env, int version) {
  (void)vm;
  (void)version;
  if (env)
    *env = fake_env;
  return JNI_OK;
}

static void *vm_table[8];
static void **vm_table_ptr = vm_table;
void *fake_vm = &vm_table_ptr;

void jni_init(void) {
  if (!kv_get("STORAGE_ROOT"))
    kv_set("STORAGE_ROOT", ".");
  if (!kv_get("STORAGE_ROOT_BASE"))
    kv_set("STORAGE_ROOT_BASE", ".");

  if (!kv_get("AUDIO_MUSIC_VOLUME"))
    kv_set("AUDIO_MUSIC_VOLUME", "1.0");
  if (!kv_get("AUDIO_SFX_VOLUME"))
    kv_set("AUDIO_SFX_VOLUME", "1.0");
  if (!kv_get("MusicVolume"))
    kv_set("MusicVolume", "1.0");
  if (!kv_get("RadioVolume"))
    kv_set("RadioVolume", "1.0");
  if (!kv_get("SFXVolume"))
    kv_set("SFXVolume", "1.0");

  for (int i = 0; i < 233; i++)
    env_table[i] = (void *)j_unimplemented;

  env_table[4] = (void *)j_GetVersion;
  env_table[6] = (void *)j_FindClass;
  env_table[15] = (void *)j_ExceptionOccurred;
  env_table[16] = (void *)j_ExceptionClearDescribe;
  env_table[17] = (void *)j_ExceptionClearDescribe;
  env_table[19] = (void *)j_PushLocalFrame;
  env_table[20] = (void *)j_PopLocalFrame;
  env_table[21] = (void *)j_NewGlobalRef;
  env_table[22] = (void *)j_DeleteRef;
  env_table[23] = (void *)j_DeleteRef;
  env_table[24] = (void *)j_ret0_3;
  env_table[25] = (void *)j_NewLocalRef;
  env_table[26] = (void *)j_ret0_2;
  env_table[28] = (void *)j_NewObject;
  env_table[29] = (void *)j_NewObjectV;
  env_table[31] = (void *)j_GetObjectClass;
  env_table[33] = (void *)j_GetMethodID;
  env_table[34] = (void *)j_CallObjectMethod;
  env_table[35] = (void *)j_CallObjectMethodV;
  env_table[37] = (void *)j_CallBooleanMethod;
  env_table[38] = (void *)j_CallBooleanMethodV;
  env_table[49] = (void *)j_CallIntMethod;
  env_table[50] = (void *)j_CallIntMethodV;
  env_table[53] = (void *)j_CallLongMethodV;
  env_table[56] = (void *)j_CallFloatMethodV;
  env_table[61] = (void *)j_CallVoidMethod;
  env_table[62] = (void *)j_CallVoidMethodV;
  env_table[94] = (void *)j_GetFieldID;
  env_table[95] = (void *)j_GetObjectField;
  env_table[96] = (void *)j_GetBooleanField;
  env_table[100] = (void *)j_GetIntField;
  env_table[101] = (void *)j_GetLongField;
  env_table[102] = (void *)j_GetFloatField;
  env_table[113] = (void *)j_GetMethodID;
  env_table[114] = (void *)j_CallStaticObjectMethod;
  env_table[115] = (void *)j_CallStaticObjectMethodV;
  env_table[141] = (void *)j_CallStaticVoidMethod;
  env_table[142] = (void *)j_CallStaticVoidMethodV;
  env_table[144] = (void *)j_GetFieldID;
  env_table[164] = (void *)j_GetStringLength;
  env_table[165] = (void *)j_GetStringChars;
  env_table[166] = (void *)j_ReleaseStringChars;
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars;
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[171] = (void *)j_GetArrayLength;
  env_table[172] = (void *)j_NewObjectArray;
  env_table[173] = (void *)j_GetObjectArrayElement;
  env_table[174] = (void *)j_SetObjectArrayElement;
  env_table[176] = (void *)j_NewByteArray;
  env_table[179] = (void *)j_NewIntArray;
  env_table[181] = (void *)j_NewFloatArray;
  env_table[183] = (void *)j_GetPriArrayElements;
  env_table[184] = (void *)j_GetPriArrayElements;
  env_table[185] = (void *)j_GetPriArrayElements;
  env_table[186] = (void *)j_GetPriArrayElements;
  env_table[187] = (void *)j_GetPriArrayElements;
  env_table[188] = (void *)j_GetPriArrayElements;
  env_table[189] = (void *)j_GetPriArrayElements;
  env_table[190] = (void *)j_GetPriArrayElements;
  for (int i = 191; i <= 198; i++)
    env_table[i] = (void *)j_ReleasePriArrayElements;
  for (int i = 199; i <= 206; i++)
    env_table[i] = (void *)j_GetPriArrayRegion;
  for (int i = 207; i <= 214; i++)
    env_table[i] = (void *)j_SetPriArrayRegion;
  env_table[215] = (void *)j_RegisterNatives;
  env_table[216] = (void *)j_ret0_2;
  env_table[217] = (void *)j_ret0_2;
  env_table[218] = (void *)j_ret0_2;
  env_table[219] = (void *)j_GetJavaVM;
  env_table[222] =
      (void *)j_GetPriArrayElements;
  env_table[223] =
      (void *)j_ReleasePriArrayElements;
  env_table[226] = (void *)j_NewGlobalRef;
  env_table[227] = (void *)j_DeleteRef;
  env_table[228] = (void *)j_ExceptionCheck;

  vm_table[3] = (void *)vm_DestroyJavaVM;
  vm_table[4] = (void *)vm_AttachCurrentThread;
  vm_table[5] = (void *)vm_DetachCurrentThread;
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] =
      (void *)vm_AttachCurrentThread;

  debugPrintf("JNI: fake environment initialized (env=%p vm=%p)\n", fake_env,
              fake_vm);
}

typedef struct {
  int (*func)(void *);
  void *arg;
  pthread_t thrd;
  volatile int done;
  int joined;
} OSThreadHandle;

typedef struct {
  void *(*func)(void *);
  void *arg;
} NVThreadStart;

static void *os_thread_trampoline(void *arg) {
  OSThreadHandle *h = arg;
  const int rc = h->func(h->arg);
  h->done = 1;
  return (void *)(intptr_t)rc;
}

void *OS_ThreadLaunch(int (*func)(void *), void *arg, int r2, char *name,
                      int r4, int priority) {
  (void)r2;
  (void)r4;
  (void)priority;
  debugPrintf("OS_ThreadLaunch: %s\n", name ? name : "(unnamed)");
  OSThreadHandle *h = calloc(1, sizeof(*h));
  if (!h)
    return NULL;
  h->func = func;
  h->arg = arg;
  if (pthread_create(&h->thrd, NULL, os_thread_trampoline, h) != 0) {
    free(h);
    return NULL;
  }
  return h;
}

int OS_ThreadWait(void *handle) {
  OSThreadHandle *h = handle;
  if (h && !h->joined) {
    void *res = NULL;
    pthread_join(h->thrd, &res);
    h->joined = 1;
  }
  return 0;
}

void OS_ThreadClose(void *handle) {
  OSThreadHandle *h = handle;
  if (h) {
    if (!h->joined)
      pthread_detach(h->thrd);
    free(h);
  }
}

int OS_ThreadIsRunning(void *handle) {
  OSThreadHandle *h = handle;
  return h && !h->done;
}

static void *nv_thread_trampoline(void *arg) {
  NVThreadStart *start = arg;
  void *(*func)(void *) = start->func;
  void *user_arg = start->arg;
  free(start);
  return func(user_arg);
}

int NVThreadSpawnJNIThread(long *tid, const void *attr, const char *name,
                           void *(*fn)(void *), void *arg) {
  (void)attr;
  debugPrintf("NVThreadSpawnJNIThread: %s\n", name ? name : "(unnamed)");
  NVThreadStart *start = calloc(1, sizeof(*start));
  if (!start)
    return -1;
  start->func = fn;
  start->arg = arg;
  pthread_t thrd;
  if (pthread_create(&thrd, NULL, nv_thread_trampoline, start) != 0) {
    free(start);
    return -1;
  }
  if (tid)
    *tid = (long)thrd;
  return 0;
}

void *NVThreadGetCurrentJNIEnv(void) { return fake_env; }

int OS_ScreenGetWidth(void) { return ctw_screen_w(); }
int OS_ScreenGetHeight(void) { return ctw_screen_h(); }
