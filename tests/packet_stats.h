// SPDX-License-Identifier: BSD-3-Clause
#ifndef PACKET_STATS_H
#define PACKET_STATS_H
#include <stdint.h>
struct packet_stats {
    uint64_t count, sum, maximum, max_three, history[2];
    uint64_t maximum_index, three_index;
};
static inline void packet_stats_add(struct packet_stats *s, uint64_t ticks) {
    uint64_t sum = ticks + s->history[0] + s->history[1];
    if (ticks > s->maximum) { s->maximum = ticks; s->maximum_index = s->count; }
    if (s->count >= 2 && sum > s->max_three) {
        s->max_three = sum; s->three_index = s->count - 2;
    }
    s->history[0] = s->history[1]; s->history[1] = ticks;
    s->count++; s->sum += ticks;
}
#endif
