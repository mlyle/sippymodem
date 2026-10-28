// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_ECHO_TIMING_H
#define V92_ECHO_TIMING_H

#include "echo_shift.h"
#include "pcm_host_echo.h"
#include "v92_upstream_data_rx.h"

enum {
	V92_ECHO_TIMING_HISTORY = 1024,
	V92_ECHO_TIMING_STRIDE = 4,
	V92_ECHO_TIMING_WINDOW_SAMPLES = 256,
};

/* Owned beside the frozen echo model, outside transactional frame decoding.
 * Keep its prediction in constellation coordinates, with the same causal
 * residual inverse as the actual input. No decoded source bits are needed. */
struct v92_echo_timing {
	struct echo_shift_window window;
	float prediction[V92_ECHO_TIMING_HISTORY];
	float observation[V92_ECHO_TIMING_HISTORY];
	float half_width[V92_ECHO_TIMING_HISTORY];
	uint8_t phase[V92_ECHO_TIMING_HISTORY];
	struct v92_residual inverse;
	double gain, offset;
	double step[V92_CPD_MAX_CONSTELLATIONS];
	double inverse_step[V92_CPD_MAX_CONSTELLATIONS];
	uint64_t samples, expected_sample;
	uint32_t model_delay;
	int8_t polarity;
	bool initialized;
};

void v92_echo_timing_reset(struct v92_echo_timing *timing);

/* Internal pre-sample owner path. The receiver and echo are already validated
 * for this media block; the mapping stays fixed within one ordinary data
 * epoch. Unsupported coordinates or a discontinuity discard old evidence.
 * No borrowed pointer is retained. A nonzero result has retimed only the
 * frozen echo model: predict this unconsumed input again before receiving it.
 * Coding, quality recovery and error-control state remain with their owners. */
int v92_echo_timing_observe(
	struct v92_echo_timing *timing, struct pcm_host_echo *echo,
	const struct v92_upstream_data_rx *receiver,
	enum pcm_host_echo_mode mode, uint8_t wire,
	int16_t received, int16_t cleaned, bool available);

#endif
