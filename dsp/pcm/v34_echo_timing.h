// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_ECHO_TIMING_H
#define V34_ECHO_TIMING_H

#include "v34_data_decision_receiver.h"
#include "v34_line_receiver.h"
#include "pcm_host_echo.h"
#include "echo_shift.h"

enum {
	V34_ECHO_TIMING_RADIUS = ECHO_TIMING_RADIUS,
	V34_ECHO_TIMING_BINS = 2 * V34_ECHO_TIMING_RADIUS + 1,
	V34_ECHO_TIMING_HISTORY = 1024,
	V34_ECHO_TIMING_OBSERVATION_STRIDE = 4,
	V34_ECHO_TIMING_OBSERVATIONS = V34_ECHO_TIMING_HISTORY / V34_ECHO_TIMING_OBSERVATION_STRIDE,
};

struct v34_echo_timing_bin {
	double weight[4];
	double cosine, sine;
	int16_t lag;
};

struct v34_echo_timing_point {
	uint64_t epoch;
	float real, imaginary;
	float cosine, sine;
};

/* Equalizer observations occur at one out of four half-symbol epochs. */
struct v34_echo_timing_observation {
	uint64_t epoch;
	float real, imaginary;
};

/* Owned beside the echo model, outside transactionally copied receive paths.
 * All input pointers are borrowed only for the duration of a call. */
struct v34_echo_timing {
	struct v34_receive_timing reference_timing;
	struct v34_equalizer reference_equalizer;
	struct v34_echo_timing_point history[V34_ECHO_TIMING_HISTORY];
	struct v34_echo_timing_observation observations[V34_ECHO_TIMING_OBSERVATIONS];
	struct v34_echo_timing_bin bins[V34_ECHO_TIMING_BINS];
	struct echo_shift_window normal, fast;
	double scale, symbol_rate, carrier_frequency;
	uint64_t first_half, latest_half, drain_half, receiver_epoch;
	unsigned lookahead;
	uint32_t model_delay;
	bool initialized, prepared;
};

/* Logical reset invalidates retained evidence without clearing the large
 * buffers on every unsupported or paused receive sample. */
void v34_echo_timing_reset(struct v34_echo_timing *timing);

/* Prepare one reference sample using the receiver's pre-sample controls.
 * Only a linear, unprecoded data constellation is currently supported.
 * Unsupported configuration or a discontinuity discards prior evidence. */
bool v34_echo_timing_prepare(
	struct v34_echo_timing *timing,
	const struct pcm_host_echo *echo, enum pcm_host_echo_mode mode,
	const struct v34_line_receiver *receiver,
	const struct v34_data_decision_receiver *data,
	int16_t received, int16_t cleaned);

/* Commit after the same physical receive step. Work is bounded by the at most
 * two half-symbols produced by that step. A nonzero result reports an applied
 * delay change in host samples; the receiver and media flow are untouched. */
int v34_echo_timing_observe(
	struct v34_echo_timing *timing,
	struct pcm_host_echo *echo,
	const struct v34_line_receiver *receiver);

#endif
