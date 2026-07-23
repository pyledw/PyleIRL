#ifndef COMPAT_PTHREAD_H
#define COMPAT_PTHREAD_H

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#include <stdlib.h>

typedef HANDLE compat_pthread_t;
typedef SRWLOCK compat_pthread_mutex_t;

#define COMPAT_PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT

static inline int compat_pthread_mutex_lock(compat_pthread_mutex_t *mutex) {
    AcquireSRWLockExclusive(mutex);
    return 0;
}

static inline int compat_pthread_mutex_unlock(compat_pthread_mutex_t *mutex) {
    ReleaseSRWLockExclusive(mutex);
    return 0;
}

struct compat_pthread_wrapper_data {
    void *(*start_routine)(void *);
    void *arg;
};

static unsigned __stdcall compat_pthread_wrapper_func(void *p) {
    struct compat_pthread_wrapper_data *data = (struct compat_pthread_wrapper_data *)p;
    void *(*start_routine)(void *) = data->start_routine;
    void *arg = data->arg;
    free(data);
    start_routine(arg);
    return 0;
}

static inline int compat_pthread_create(compat_pthread_t *thread, const void *attr, void *(*start_routine)(void *), void *arg) {
    (void)attr;
    struct compat_pthread_wrapper_data *data = (struct compat_pthread_wrapper_data *)malloc(sizeof(struct compat_pthread_wrapper_data));
    if (!data) return 1;
    data->start_routine = start_routine;
    data->arg = arg;
    *thread = (HANDLE)_beginthreadex(NULL, 0, compat_pthread_wrapper_func, data, 0, NULL);
    if (*thread == 0) {
        free(data);
        return 1;
    }
    return 0;
}

static inline int compat_pthread_join(compat_pthread_t thread, void **retval) {
    if (retval) *retval = NULL;
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return 0;
}

#define pthread_t compat_pthread_t
#define pthread_mutex_t compat_pthread_mutex_t
#ifdef PTHREAD_MUTEX_INITIALIZER
#undef PTHREAD_MUTEX_INITIALIZER
#endif
#define PTHREAD_MUTEX_INITIALIZER COMPAT_PTHREAD_MUTEX_INITIALIZER
#define pthread_mutex_lock compat_pthread_mutex_lock
#define pthread_mutex_unlock compat_pthread_mutex_unlock
#define pthread_create compat_pthread_create
#define pthread_join compat_pthread_join

#else
#include <pthread.h>
#endif

#endif // COMPAT_PTHREAD_H
