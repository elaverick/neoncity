#include "thread.h"
#include <stdlib.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

struct Thread { HANDLE h; ThreadFn fn; void *arg; };

static DWORD WINAPI thread_trampoline(LPVOID p) {
    Thread *t = (Thread *)p;
    return (DWORD)t->fn(t->arg);
}

Thread *thread_create(ThreadFn fn, void *arg) {
    Thread *t = (Thread *)calloc(1, sizeof(Thread));
    t->fn = fn; t->arg = arg;
    t->h = CreateThread(NULL, 0, thread_trampoline, t, 0, NULL);
    return t;
}
void thread_join(Thread *t) { WaitForSingleObject(t->h, INFINITE); CloseHandle(t->h); free(t); }
int cpu_count(void) { SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }

void mutex_init(Mutex *m) { m->impl = calloc(1, sizeof(SRWLOCK)); InitializeSRWLock((SRWLOCK *)m->impl); }
void mutex_destroy(Mutex *m) { free(m->impl); }
void mutex_lock(Mutex *m) { AcquireSRWLockExclusive((SRWLOCK *)m->impl); }
void mutex_unlock(Mutex *m) { ReleaseSRWLockExclusive((SRWLOCK *)m->impl); }

void cond_init(CondVar *c) { c->impl = calloc(1, sizeof(CONDITION_VARIABLE)); InitializeConditionVariable((CONDITION_VARIABLE *)c->impl); }
void cond_destroy(CondVar *c) { free(c->impl); }
void cond_wait(CondVar *c, Mutex *m) { SleepConditionVariableSRW((CONDITION_VARIABLE *)c->impl, (SRWLOCK *)m->impl, INFINITE, 0); }
void cond_signal(CondVar *c) { WakeConditionVariable((CONDITION_VARIABLE *)c->impl); }
void cond_broadcast(CondVar *c) { WakeAllConditionVariable((CONDITION_VARIABLE *)c->impl); }

#else
#include <pthread.h>
#include <unistd.h>

struct Thread { pthread_t h; ThreadFn fn; void *arg; };

static void *thread_trampoline(void *p) {
    Thread *t = (Thread *)p;
    t->fn(t->arg);
    return NULL;
}

Thread *thread_create(ThreadFn fn, void *arg) {
    Thread *t = (Thread *)calloc(1, sizeof(Thread));
    t->fn = fn; t->arg = arg;
    pthread_create(&t->h, NULL, thread_trampoline, t);
    return t;
}
void thread_join(Thread *t) { pthread_join(t->h, NULL); free(t); }
int cpu_count(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 4; }

void mutex_init(Mutex *m) { m->impl = malloc(sizeof(pthread_mutex_t)); pthread_mutex_init((pthread_mutex_t *)m->impl, NULL); }
void mutex_destroy(Mutex *m) { pthread_mutex_destroy((pthread_mutex_t *)m->impl); free(m->impl); }
void mutex_lock(Mutex *m) { pthread_mutex_lock((pthread_mutex_t *)m->impl); }
void mutex_unlock(Mutex *m) { pthread_mutex_unlock((pthread_mutex_t *)m->impl); }

void cond_init(CondVar *c) { c->impl = malloc(sizeof(pthread_cond_t)); pthread_cond_init((pthread_cond_t *)c->impl, NULL); }
void cond_destroy(CondVar *c) { pthread_cond_destroy((pthread_cond_t *)c->impl); free(c->impl); }
void cond_wait(CondVar *c, Mutex *m) { pthread_cond_wait((pthread_cond_t *)c->impl, (pthread_mutex_t *)m->impl); }
void cond_signal(CondVar *c) { pthread_cond_signal((pthread_cond_t *)c->impl); }
void cond_broadcast(CondVar *c) { pthread_cond_broadcast((pthread_cond_t *)c->impl); }
#endif

/* --- thread_parallel_for ------------------------------------------------------ */

typedef struct ParallelJob { Mutex lock; int next, n; ParallelFn fn; void *ctx; } ParallelJob;

static int parallel_worker(void *arg) {
    ParallelJob *j = (ParallelJob *)arg;
    for (;;) {
        mutex_lock(&j->lock);
        int i = j->next < j->n ? j->next++ : -1;
        mutex_unlock(&j->lock);
        if (i < 0) return 0;
        j->fn(j->ctx, i);
    }
}

void thread_parallel_for(int n, int threads, ParallelFn fn, void *ctx) {
    if (threads > n) threads = n;
    if (threads <= 1) {
        for (int i = 0; i < n; ++i) fn(ctx, i);
        return;
    }
    ParallelJob j;
    mutex_init(&j.lock);
    j.next = 0; j.n = n; j.fn = fn; j.ctx = ctx;
    Thread **t = (Thread **)malloc(sizeof(Thread *) * (size_t)(threads - 1));
    int started = 0;
    for (int k = 0; t && k < threads - 1; ++k) t[started++] = thread_create(parallel_worker, &j);
    parallel_worker(&j);
    for (int k = 0; k < started; ++k) thread_join(t[k]);
    free(t);
    mutex_destroy(&j.lock);
}
