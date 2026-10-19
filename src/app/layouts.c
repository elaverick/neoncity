#include "layouts.h"
#include "thread.h"
#include <stdio.h>
#include <stdlib.h>

/* Each slot of the cache holds one chunk's layout. A slot is EMPTY, QUEUED
 * for the worker, BUILDING on it, or READY. The worker only writes a slot's
 * layout while it is BUILDING, and the main thread only reads a READY one or
 * builds into one it has taken (EMPTY until done), so the layouts themselves
 * need no lock; g_lock guards the slots' keys and states, the queue and the
 * counts. Only READY and EMPTY slots are ever reused, least recently used
 * first. A queued slot the main thread needs before the worker has started
 * on it is taken back and built at once; one already building is waited for.
 * The queue holds each job's chunk as well as its slot, so a job whose slot
 * has since been taken back is skipped. */

#define LAYOUT_AHEAD 2                       /* chunks either side of the player's built ahead: 5 x 5 */
#define LAYOUT_CACHE 32                      /* the 5 x 5, and the row just left (about 0.5 MB each) */
#define LAYOUT_QUEUE (2 * LAYOUT_CACHE)

enum { LS_EMPTY = 0, LS_QUEUED, LS_BUILDING, LS_READY };

typedef struct LSlot { int cx, cz, state; uint32_t use; } LSlot;
typedef struct LJob { int slot, cx, cz; } LJob;

static BlockLayout g_layout[LAYOUT_CACHE];
static LSlot       g_slot[LAYOUT_CACHE];
static uint32_t    g_clock;
static LJob        g_queue[LAYOUT_QUEUE];
static uint32_t    g_head, g_tail;
static int         g_quit, g_started;
static int         g_ahead, g_here, g_waits;
static Thread     *g_worker;
static Mutex       g_lock;
static CondVar     g_wake, g_built;

static int layout_worker(void *arg) {
    (void)arg;
    mutex_lock(&g_lock);
    for (;;) {
        while (!g_quit && g_head == g_tail) cond_wait(&g_wake, &g_lock);
        if (g_quit) break;
        LJob j = g_queue[g_head++ % LAYOUT_QUEUE];
        LSlot *s = &g_slot[j.slot];
        if (s->state != LS_QUEUED || s->cx != j.cx || s->cz != j.cz) continue;   /* taken back */
        s->state = LS_BUILDING;
        mutex_unlock(&g_lock);
        city_layout(j.cx, j.cz, &g_layout[j.slot]);
        mutex_lock(&g_lock);
        s->state = LS_READY;
        g_ahead++;
        cond_broadcast(&g_built);
    }
    mutex_unlock(&g_lock);
    return 0;
}

static void start(void) {
    if (g_started) return;
    g_started = 1;
    mutex_init(&g_lock);
    cond_init(&g_wake);
    cond_init(&g_built);
    g_worker = thread_create(layout_worker, NULL);
}

/* The slot holding (or about to hold) chunk (cx, cz), or -1. Under g_lock. */
static int find(int cx, int cz) {
    for (int i = 0; i < LAYOUT_CACHE; ++i)
        if (g_slot[i].state != LS_EMPTY && g_slot[i].cx == cx && g_slot[i].cz == cz) return i;
    return -1;
}

/* The least recently used slot that is free to reuse, or -1. Under g_lock. */
static int victim(void) {
    int v = -1;
    for (int i = 0; i < LAYOUT_CACHE; ++i) {
        int st = g_slot[i].state;
        if (st != LS_EMPTY && st != LS_READY) continue;
        if (v < 0 || st == LS_EMPTY || (g_slot[v].state == LS_READY && g_slot[i].use < g_slot[v].use)) v = i;
        if (st == LS_EMPTY) break;
    }
    return v;
}

const BlockLayout *layouts_get(int cx, int cz) {
    start();
    mutex_lock(&g_lock);
    int s = find(cx, cz);
    if (s >= 0 && g_slot[s].state == LS_BUILDING) {
        g_waits++;
        while (g_slot[s].state == LS_BUILDING) cond_wait(&g_built, &g_lock);
    }
    if (s >= 0 && g_slot[s].state == LS_READY) {
        g_slot[s].use = ++g_clock;
        mutex_unlock(&g_lock);
        return &g_layout[s];
    }
    /* Not cached, or queued and not started: build it here. */
    if (s < 0) s = victim();
    if (s < 0) fatal("layouts: every cached layout is being built");
    g_slot[s] = (LSlot){ cx, cz, LS_EMPTY, ++g_clock };
    g_here++;
    mutex_unlock(&g_lock);
    city_layout(cx, cz, &g_layout[s]);
    mutex_lock(&g_lock);
    g_slot[s].state = LS_READY;
    mutex_unlock(&g_lock);
    return &g_layout[s];
}

void layouts_prefetch(int cx, int cz) {
    start();
    mutex_lock(&g_lock);
    int queued = 0;
    for (int r = 0; r <= LAYOUT_AHEAD; ++r)   /* ring by ring, nearest first */
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx) {
                if (abs(dx) != r && abs(dz) != r) continue;
                if (find(cx + dx, cz + dz) >= 0 || g_tail - g_head >= LAYOUT_QUEUE) continue;
                int s = victim();
                if (s < 0) continue;
                g_slot[s] = (LSlot){ cx + dx, cz + dz, LS_QUEUED, ++g_clock };
                g_queue[g_tail++ % LAYOUT_QUEUE] = (LJob){ s, cx + dx, cz + dz };
                queued = 1;
            }
    if (queued) cond_signal(&g_wake);
    mutex_unlock(&g_lock);
}

void layouts_stats(char *buf, size_t n) {
    if (g_started) mutex_lock(&g_lock);
    snprintf(buf, n, "collision layouts: %d built ahead, %d built on the main thread, %d waited for", g_ahead, g_here, g_waits);
    if (g_started) mutex_unlock(&g_lock);
}

void layouts_shutdown(void) {
    if (!g_started) return;
    mutex_lock(&g_lock);
    g_quit = 1;
    cond_broadcast(&g_wake);
    mutex_unlock(&g_lock);
    thread_join(g_worker);
    cond_destroy(&g_built);
    cond_destroy(&g_wake);
    mutex_destroy(&g_lock);
    g_started = 0;
}
