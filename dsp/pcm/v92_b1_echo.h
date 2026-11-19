// SPDX-License-Identifier: BSD-3-Clause
#ifndef V92_B1_ECHO_H
#define V92_B1_ECHO_H

#include "pcm_host_echo.h"
#include "v92_cpd_distance.h"
#include "v92_data_quality.h"
#include "v92_upstream_data_rx.h"

enum {
	V92_B1_ECHO_RADIUS = 320,
	V92_B1_ECHO_BINS = 2 * V92_B1_ECHO_RADIUS + 1,
	V92_B1_ECHO_SAMPLES = V92_B1U_SYMBOLS,
	V92_B1_ECHO_FIT_INPUTS = 144,
	V92_B1_ECHO_INPUTS = (V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES) *
		V92_PCM_MAPPING_FRAME_SYMBOLS,
};

struct v92_b1_echo_fit {
	double target[V92_B1_ECHO_SAMPLES];
	double correlation_squared;
	int alignment, shift, runner;
	unsigned model;
};

struct v92_b1_echo_evidence {
	double score[V92_B1_ECHO_BINS];
	double old_sum, old_square, runner_sum, runner_square;
	unsigned count;
};

/* One provisional receiver owned beside the echo model, outside the copied
 * call state. No borrowed pointers or process-global state. A zero initializer
 * begins a call. The working receiver is copied once at the B1u handoff; it
 * is not part of the frequently reset metadata prefix. */
struct v92_b1_echo {
	struct v92_b1_echo_fit fit;
	struct v92_b1_echo_evidence held[2];
	struct v92_b1_quality quality;
	uint64_t clock;
	uint32_t delay;
	unsigned count, prefix, replayed, budget;
	unsigned stage;
	bool retained;
	int16_t ideal[V92_B1_ECHO_SAMPLES];
	int16_t raw[V92_B1_ECHO_INPUTS];
	/* Only the fitting interval is reread to validate captured subtraction.
	 * The full raw and wire histories serve the alternate decoder below. */
	int16_t cleaned[V92_B1_ECHO_FIT_INPUTS];
	uint8_t code[V92_B1_ECHO_INPUTS];
	double prediction[V92_B1_ECHO_INPUTS + 2 * V92_B1_ECHO_RADIUS];
	float coefficients[PCM_HOST_ECHO_FILTER_TAPS];
	struct v92_upstream_data_rx receiver;
};

void v92_b1_echo_reset(struct v92_b1_echo *recovery);
void v92_b1_echo_begin_block_owned(struct v92_b1_echo *recovery, unsigned samples);

/* Called before the ordinary receiver consumes this input. The enclosing
 * owner qualifies an initial raw or short B1u epoch and validates echo/receiver.
 * The owner's distance bank follows that receiver through the data epoch.
 * Missing media, clipping, queued input or any model/clock change abandon it.
 * Work is bounded by two replay submissions per input and ceil(1.6 times
 * the block length) submissions per block.
 * True supplies a complete native alternate for this input only; the ordinary
 * call still validates and decides whether to publish it. */
bool v92_b1_echo_observe_owned(struct v92_b1_echo *recovery,
	const struct pcm_host_echo *echo, const struct v92_upstream_data_rx *receiver,
	struct v92_cpd_distances *distances,
	uint8_t code, int16_t raw, int16_t cleaned, bool available);

/* After the call validates the alternate, commit the independently confirmed
 * frozen-model retiming. Failure leaves echo unchanged. */
bool v92_b1_echo_accept_owned(struct v92_b1_echo *recovery, struct pcm_host_echo *echo);

#endif
