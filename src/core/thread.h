#pragma once
/* Minimal portable threading: threads, mutexes, condition variables. */

typedef struct Thread Thread;
typedef struct Mutex  { void *impl; } Mutex;
typedef struct CondVar { void *impl; } CondVar;
typedef int (*ThreadFn)(void *arg);

Thread *thread_create(ThreadFn fn, void *arg);
void    thread_join(Thread *t);
int     cpu_count(void);

void mutex_init(Mutex *m);
void mutex_destroy(Mutex *m);
void mutex_lock(Mutex *m);
void mutex_unlock(Mutex *m);

void cond_init(CondVar *c);
void cond_destroy(CondVar *c);
void cond_wait(CondVar *c, Mutex *m);
void cond_signal(CondVar *c);
void cond_broadcast(CondVar *c);

/* Calls fn(ctx, i) for every i in [0, n), on up to `threads` threads (this
 * one among them), and returns once all have returned. Which thread runs
 * which i, and in what order, is unspecified: fn must write only what
 * belongs to its i. */
typedef void (*ParallelFn)(void *ctx, int i);
void thread_parallel_for(int n, int threads, ParallelFn fn, void *ctx);
