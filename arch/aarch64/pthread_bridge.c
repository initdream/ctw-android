#define _GNU_SOURCE

#include <pthread.h>
#include <semaphore.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IS_HEAP_PTR(v) ((uintptr_t)(v) > 0x10000u)

int pthread_attr_init_fake(void *attr) {
  (void)attr;
  return 0;
}

int pthread_attr_destroy_fake(void *attr) {
  (void)attr;
  return 0;
}

int pthread_attr_setstacksize_fake(void *attr, size_t stacksize) {
  (void)attr;
  (void)stacksize;
  return 0;
}

int pthread_attr_setschedparam_fake(void *attr, const void *param) {
  (void)attr;
  (void)param;
  return 0;
}

int pthread_attr_getschedparam_fake(const void *attr, void *param) {
  (void)attr;
  if (param)
    memset(param, 0, 16); /* struct sched_param is 4 useful bytes + pad */
  return 0;
}

int pthread_attr_getstacksize_fake(const void *attr, size_t *size) {
  (void)attr;
  if (size)
    *size = 512 * 1024;
  return 0;
}


int pthread_create_fake(unsigned long *thread, const void *attr,
                        void *(*start_routine)(void *), void *arg) {
  (void)attr;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 4 * 1024 * 1024);
  int ret = pthread_create((pthread_t *)thread, &a, start_routine, arg);
  pthread_attr_destroy(&a);
  return ret;
}

int pthread_join_fake(unsigned long thread, void **retval) {
  return pthread_join((pthread_t)thread, retval);
}

int pthread_detach_fake(unsigned long thread) {
  return pthread_detach((pthread_t)thread);
}

unsigned long pthread_self_fake(void) {
  return (unsigned long)pthread_self();
}

int pthread_key_create_fake(unsigned int *key, void (*destructor)(void *)) {
  return pthread_key_create((pthread_key_t *)key, destructor);
}

int pthread_key_delete_fake(unsigned int key) {
  return pthread_key_delete((pthread_key_t)key);
}

void *pthread_getspecific_fake(unsigned int key) {
  return pthread_getspecific((pthread_key_t)key);
}

int pthread_setspecific_fake(unsigned int key, const void *value) {
  return pthread_setspecific((pthread_key_t)key, value);
}

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;


int pthread_setname_np_fake(unsigned long thread, const char *name) {
  (void)thread;
  (void)name;
  return 0;
}

int pthread_setschedparam_fake(unsigned long thread, int policy,
                               const void *param) {
  (void)thread;
  (void)policy;
  (void)param;
  return 0;
}

int pthread_once_fake(volatile int *once_control, void (*init_routine)(void)) {
  if (!once_control || !init_routine)
    return EINVAL;
  if (__sync_lock_test_and_set(once_control, 1) == 0) {
    init_routine();
    *once_control = 2;
  }
  return 0;
}


static pthread_mutex_t *new_recursive_mutex(void) {
  pthread_mutex_t *r = (pthread_mutex_t *)calloc(1, sizeof(*r));
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(r, &a);
  pthread_mutexattr_destroy(&a);
  return r;
}

static pthread_mutex_t *mtx_real(void **slot) {
  if (IS_HEAP_PTR(*slot))
    return (pthread_mutex_t *)*slot;
  pthread_mutex_lock(&g_lock);
  if (!IS_HEAP_PTR(*slot))
    *slot = new_recursive_mutex();
  pthread_mutex_unlock(&g_lock);
  return (pthread_mutex_t *)*slot;
}

int pthread_mutex_init_fake(void *mutex, const void *attr) {
  (void)attr;
  void **slot = (void **)mutex;
  pthread_mutex_t *r = new_recursive_mutex();
  pthread_mutex_lock(&g_lock);
  *slot = r;
  pthread_mutex_unlock(&g_lock);
  return 0;
}

int pthread_mutex_destroy_fake(void *mutex) {
  void **slot = (void **)mutex;
  pthread_mutex_lock(&g_lock);
  if (IS_HEAP_PTR(*slot)) {
    pthread_mutex_t *r = (pthread_mutex_t *)*slot;
    pthread_mutex_destroy(r);
    free(r);
    *slot = NULL;
  }
  pthread_mutex_unlock(&g_lock);
  return 0;
}

int pthread_mutex_lock_fake(void *mutex) {
  return pthread_mutex_lock(mtx_real((void **)mutex));
}

int pthread_mutex_trylock_fake(void *mutex) {
  return pthread_mutex_trylock(mtx_real((void **)mutex));
}

int pthread_mutex_unlock_fake(void *mutex) {
  return pthread_mutex_unlock(mtx_real((void **)mutex));
}


int pthread_mutexattr_init_fake(void *attr) {
  if (attr)
    *(int *)attr = 0;
  return 0;
}

int pthread_mutexattr_destroy_fake(void *attr) {
  (void)attr;
  return 0;
}

int pthread_mutexattr_settype_fake(void *attr, int type) {
  if (attr)
    *(int *)attr = type;
  return 0;
}

static pthread_cond_t *cnd_real(void **slot) {
  if (IS_HEAP_PTR(*slot))
    return (pthread_cond_t *)*slot;
  pthread_mutex_lock(&g_lock);
  if (!IS_HEAP_PTR(*slot)) {
    pthread_cond_t *c = (pthread_cond_t *)calloc(1, sizeof(*c));
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(c, &a);
    pthread_condattr_destroy(&a);
    *slot = c;
  }
  pthread_mutex_unlock(&g_lock);
  return (pthread_cond_t *)*slot;
}

int pthread_cond_init_fake(void *cond, const void *attr) {
  (void)attr;
  cnd_real((void **)cond);
  return 0;
}

int pthread_cond_destroy_fake(void *cond) {
  void **slot = (void **)cond;
  pthread_mutex_lock(&g_lock);
  if (IS_HEAP_PTR(*slot)) {
    pthread_cond_t *c = (pthread_cond_t *)*slot;
    pthread_cond_destroy(c);
    free(c);
    *slot = NULL;
  }
  pthread_mutex_unlock(&g_lock);
  return 0;
}

int pthread_cond_signal_fake(void *cond) {
  return pthread_cond_signal(cnd_real((void **)cond));
}

int pthread_cond_broadcast_fake(void *cond) {
  return pthread_cond_broadcast(cnd_real((void **)cond));
}

int pthread_cond_wait_fake(void *cond, void *mutex) {
  return pthread_cond_wait(cnd_real((void **)cond), mtx_real((void **)mutex));
}

int pthread_cond_timedwait_fake(void *cond, void *mutex,
                                const struct timespec *abstime) {
  return pthread_cond_timedwait(cnd_real((void **)cond),
                                mtx_real((void **)mutex), abstime);
}

static pthread_rwlock_t *rw_real(void **slot) {
  if (IS_HEAP_PTR(*slot))
    return (pthread_rwlock_t *)*slot;
  pthread_mutex_lock(&g_lock);
  if (!IS_HEAP_PTR(*slot)) {
    pthread_rwlock_t *r = (pthread_rwlock_t *)calloc(1, sizeof(*r));
    pthread_rwlock_init(r, NULL);
    *slot = r;
  }
  pthread_mutex_unlock(&g_lock);
  return (pthread_rwlock_t *)*slot;
}

int pthread_rwlock_rdlock_fake(void *rwlock) {
  return pthread_rwlock_rdlock(rw_real((void **)rwlock));
}

int pthread_rwlock_wrlock_fake(void *rwlock) {
  return pthread_rwlock_wrlock(rw_real((void **)rwlock));
}

int pthread_rwlock_unlock_fake(void *rwlock) {
  return pthread_rwlock_unlock(rw_real((void **)rwlock));
}

static sem_t *sem_real(void **slot) {
  if (IS_HEAP_PTR(*slot))
    return (sem_t *)*slot;
  pthread_mutex_lock(&g_lock);
  if (!IS_HEAP_PTR(*slot)) {
    sem_t *s = (sem_t *)malloc(sizeof(*s));
    sem_init(s, 0, 0);
    *slot = s;
  }
  pthread_mutex_unlock(&g_lock);
  return (sem_t *)*slot;
}

int sem_init_fake(void *sem, int pshared, unsigned int value) {
  void **slot = (void **)sem;
  sem_t *s = (sem_t *)malloc(sizeof(*s));
  if (sem_init(s, pshared, value) != 0) {
    free(s);
    return -1;
  }
  pthread_mutex_lock(&g_lock);
  *slot = s;
  pthread_mutex_unlock(&g_lock);
  return 0;
}

int sem_destroy_fake(void *sem) {
  void **slot = (void **)sem;
  pthread_mutex_lock(&g_lock);
  if (IS_HEAP_PTR(*slot)) {
    sem_t *s = (sem_t *)*slot;
    sem_destroy(s);
    free(s);
    *slot = NULL;
  }
  pthread_mutex_unlock(&g_lock);
  return 0;
}

int sem_post_fake(void *sem) {
  return sem_post(sem_real((void **)sem));
}

int sem_wait_fake(void *sem) {
  return sem_wait(sem_real((void **)sem));
}

int sem_trywait_fake(void *sem) {
  return sem_trywait(sem_real((void **)sem));
}

int sem_getvalue_fake(void *sem, int *sval) {
  return sem_getvalue(sem_real((void **)sem), sval);
}

