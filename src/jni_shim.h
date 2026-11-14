#ifndef __JNI_SHIM_H__
#define __JNI_SHIM_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void *fake_vm;  /* JavaVM * */
extern void *fake_env; /* JNIEnv * */

/* set by main.c (or a signal handler) to ask the main loop to shut down */
extern volatile int jni_quit_requested;

void jni_init(void);

typedef enum {
  JNI_RS_INITIAL_COMPLETE = 1,
  JNI_RS_SIGN_IN_COMPLETE,
  JNI_RS_SIGN_OUT_COMPLETE,
  JNI_RS_CLOUD_DISABLED_COMPLETE,
  JNI_RS_GATE_COMPLETE,
  JNI_RS_STATE_CHANGED,
  JNI_RS_ID_CHANGED,
  JNI_RS_TICKET_CHANGED,
  JNI_RS_SETUP,
} JniRockstarEventType;

typedef struct {
  JniRockstarEventType type;
  int accepted;
  int signed_in;
  void *value;      /* JString for ID/TICKET_CHANGED, else NULL */
  void *environment; /* JString for SETUP, else NULL */
  void *title_id;    /* JString for SETUP, else NULL */
} JniRockstarEvent;

typedef struct {
  int success;
  int count;
} JniPlaylistEvent;

void *jni_make_string(const char *utf);
void *jni_make_string_array(int n, const char **strs);
void *jni_make_int_array(int n, const int *vals);
void *jni_make_object(const char *label);

int jni_pop_pending_playlist(JniPlaylistEvent *event);
int jni_pop_pending_rockstar(JniRockstarEvent *event);

/* rockstar service completion queued by main.c once the game is booted.
 * value/environment/title_id are copied, so callers may free them. */
void jni_push_rockstar(JniRockstarEventType type, int accepted, int signed_in,
                       const char *value, const char *environment, const char *title_id);

void *NVThreadGetCurrentJNIEnv(void);
int   NVThreadSpawnJNIThread(long *tid, const void *attr, const char *name,
                             void *(*fn)(void *), void *arg);
void *OS_ThreadLaunch(int (*func)(void *), void *arg, int r2, char *name,
                      int r4, int priority);
int   OS_ThreadWait(void *handle);
void  OS_ThreadClose(void *handle);
int   OS_ThreadIsRunning(void *handle);
int   OS_ScreenGetWidth(void);
int   OS_ScreenGetHeight(void);

#ifdef __cplusplus
}
#endif

#endif
