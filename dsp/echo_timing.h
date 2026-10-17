// SPDX-License-Identifier: BSD-3-Clause

#ifndef ECHO_TIMING_H
#define ECHO_TIMING_H

#include <stdbool.h>
#include <stdint.h>

/* Units follow the owner's sample clock; the search is bounded to +/-320
 * samples. At 8 kHz this covers 40 ms. Each scale requires agreeing disjoint
 * windows and independently qualified frozen predictions. */
enum {
	ECHO_TIMING_RADIUS = 320,
	ECHO_TIMING_BINS = 2 * ECHO_TIMING_RADIUS + 1,
	ECHO_TIMING_WINDOW = 8192,
	ECHO_TIMING_MEDIUM_WINDOW = 2048,
	ECHO_TIMING_FAST_WINDOW = 128,
};

_Static_assert(ECHO_TIMING_MEDIUM_WINDOW % ECHO_TIMING_FAST_WINDOW == 0 &&
	       ECHO_TIMING_WINDOW % ECHO_TIMING_MEDIUM_WINDOW == 0,
	       "echo timing windows must contain whole shorter windows");

struct echo_timing_evidence {
	int64_t cross, current_cross;
	uint64_t energy, current_energy, input;
};

struct echo_timing_window {
	int64_t cross[ECHO_TIMING_BINS];
	uint64_t energy[ECHO_TIMING_BINS];
	struct echo_timing_evidence first;
	uint64_t input;
	uint16_t count, candidate;
	bool second, first_qualified;
};

struct echo_timing {
	int32_t prediction[ECHO_TIMING_BINS];
	int16_t received[ECHO_TIMING_BINS];
	struct echo_timing_window slow, medium, fast;
	uint16_t write, fill;
};

/* Borrowed for this observe call only. retime is synchronous and returns
 * true only after the owner accepts the delay change. No pointer is retained.
 * The monitor clears its evidence after acceptance; it never adapts a filter.
 * require_support applies the existing weak-model confidence policy. */
struct echo_timing_model {
	bool require_support;
	bool suspended;
	bool (*retime)(void *owner, int delay_change_samples);
	void *owner;
};

static inline bool echo_timing_is_valid(const struct echo_timing *t)
{
	return t != 0 && t->write < ECHO_TIMING_BINS && t->fill <= ECHO_TIMING_BINS &&
	       t->slow.count < ECHO_TIMING_WINDOW &&
	       t->medium.count < ECHO_TIMING_MEDIUM_WINDOW &&
	       t->fast.count < ECHO_TIMING_FAST_WINDOW &&
	       t->slow.candidate < ECHO_TIMING_BINS &&
	       t->medium.candidate < ECHO_TIMING_BINS &&
	       t->fast.candidate < ECHO_TIMING_BINS;
}

/* The owner validates state and supplies its frozen prediction as
 * received-cleaned. Clipped samples reset evidence. Zero initialization resets
 * the monitor; model.suspended reports confidence after each observation. */
void echo_timing_observe(struct echo_timing *timing,
	struct echo_timing_model *model, int16_t received, int16_t cleaned);

#endif
