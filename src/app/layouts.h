#pragma once
#include "city.h"

/* The collision layouts (city_layout) round the player: a cache of chunks'
 * BlockLayouts, built ahead of the player on a worker thread, so that
 * stepping into a new chunk doesn't build three layouts (2-10 ms each) on the
 * main thread. A layout is a pure function of its chunk's coordinates, so it
 * is the same whichever thread built it. */

/* Chunk (cx, cz)'s layout: from the cache; if the worker is building it, once
 * it has; otherwise built here. Valid until the next layouts_get or
 * layouts_prefetch. */
const BlockLayout *layouts_get(int cx, int cz);
/* Queues for the worker the chunks within LAYOUT_AHEAD of chunk (cx, cz) not
 * yet cached, nearest first. Call each frame with the player's chunk. */
void layouts_prefetch(int cx, int cz);
/* The cache's work so far (one line of text): layouts built ahead, built
 * here on a miss, and waited for. */
void layouts_stats(char *buf, size_t n);
/* Stops the worker. */
void layouts_shutdown(void);
