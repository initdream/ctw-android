#ifndef __PTHREAD_BRIDGE_H__
#define __PTHREAD_BRIDGE_H__
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

int pthread_attr_init_fake(void *attr);
int pthread_attr_destroy_fake(void *attr);
int pthread_attr_setstacksize_fake(void *attr, size_t stacksize);
int pthread_attr_setschedparam_fake(void *attr, const void *param);
int pthread_attr_getschedparam_fake(const void *attr, void *param);
int pthread_attr_getstacksize_fake(const void *attr, size_t *size);

int pthread_create_fake(unsigned long *thread, const void *attr, void *(*start_routine)(void *), void *arg);
int pthread_join_fake(unsigned long thread, void **retval);
int pthread_detach_fake(unsigned long thread);
unsigned long pthread_self_fake(void);
int pthread_key_create_fake(unsigned int *key, void (*destructor)(void *));
int pthread_key_delete_fake(unsigned int key);
void *pthread_getspecific_fake(unsigned int key);
int pthread_setspecific_fake(unsigned int key, const void *value);

int pthread_setname_np_fake(unsigned long thread, const char *name);
int pthread_setschedparam_fake(unsigned long thread, int policy, const void *param);
int pthread_once_fake(volatile int *once_control, void (*init_routine)(void));

int pthread_mutexattr_init_fake(void *attr);
int pthread_mutexattr_destroy_fake(void *attr);
int pthread_mutexattr_settype_fake(void *attr, int type);
int pthread_mutex_init_fake(void *mutex, const void *attr);
int pthread_mutex_destroy_fake(void *mutex);
int pthread_mutex_lock_fake(void *mutex);
int pthread_mutex_trylock_fake(void *mutex);
int pthread_mutex_unlock_fake(void *mutex);

int pthread_cond_init_fake(void *cond, const void *attr);
int pthread_cond_destroy_fake(void *cond);
int pthread_cond_signal_fake(void *cond);
int pthread_cond_broadcast_fake(void *cond);
int pthread_cond_wait_fake(void *cond, void *mutex);
int pthread_cond_timedwait_fake(void *cond, void *mutex, const struct timespec *abstime);

int pthread_rwlock_rdlock_fake(void *rwlock);
int pthread_rwlock_wrlock_fake(void *rwlock);
int pthread_rwlock_unlock_fake(void *rwlock);

int sem_init_fake(void *sem, int pshared, unsigned int value);
int sem_destroy_fake(void *sem);
int sem_post_fake(void *sem);
int sem_wait_fake(void *sem);
int sem_trywait_fake(void *sem);
int sem_getvalue_fake(void *sem, int *sval);

#ifdef __cplusplus
}
#endif
#endif