// SPDX-License-Identifier: BSD-3-Clause
#include "packet_stats.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    struct packet_stats short_stream = {0}, stream = {0};
    const uint64_t trace[] = {10, 100, 10, 1, 2, 3};
    packet_stats_add(&short_stream, 10);
    packet_stats_add(&short_stream, 100);
    assert(short_stream.max_three == 0);
    for (unsigned i = 0; i < sizeof(trace) / sizeof(trace[0]); ++i)
        packet_stats_add(&stream, trace[i]);
    assert(stream.count == 6 && stream.sum == 126);
    assert(stream.maximum == 100 && stream.maximum_index == 1);
    assert(stream.max_three == 120 && stream.three_index == 0);
    puts("packet statistics tests: PASS");
    return 0;
}
