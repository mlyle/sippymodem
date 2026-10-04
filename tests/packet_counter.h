// SPDX-License-Identifier: BSD-3-Clause
#ifndef PACKET_COUNTER_H
#define PACKET_COUNTER_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#if defined(__x86_64__)
#include <x86intrin.h>
#include <cpuid.h>
static inline uint64_t packet_ticks(void) {
    unsigned aux;
    _mm_lfence();
    uint64_t value = __rdtscp(&aux);
    _mm_lfence();
    return value;
}
#define PACKET_COUNTER_NAME "x86 invariant TSC reference cycles"
#elif defined(__aarch64__)
static inline uint64_t packet_ticks(void) {
    uint64_t value;
    __asm__ volatile("isb; mrs %0, cntvct_el0; isb" : "=r"(value) :: "memory");
    return value;
}
#define PACKET_COUNTER_NAME "ARM generic timer ticks"
#else
#error Hardware packet counter requires x86_64 or aarch64
#endif
static inline uint64_t packet_monotonic_ns(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &value)) abort();
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + value.tv_nsec;
}
static inline double packet_counter_hz(void) {
#if defined(__aarch64__)
    uint64_t frequency;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    return frequency;
#else
    unsigned a, b, c, d;
    if (!__get_cpuid(0x80000001, &a, &b, &c, &d) || !(d & (1u << 27)) ||
        !__get_cpuid(0x80000007, &a, &b, &c, &d) || !(d & (1u << 8))) {
        fputs("benchmark requires RDTSCP and an invariant TSC\n", stderr);
        exit(2);
    }
    uint64_t before_ns = packet_monotonic_ns(), before = packet_ticks();
    struct timespec delay = {.tv_nsec = 100000000};
    nanosleep(&delay, NULL);
    uint64_t after = packet_ticks(), after_ns = packet_monotonic_ns();
    return (double)(after - before) * 1e9 / (after_ns - before_ns);
#endif
}
#include "packet_stats.h"
#endif
