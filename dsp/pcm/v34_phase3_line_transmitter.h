// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE3_LINE_TRANSMITTER_H
#define V34_PHASE3_LINE_TRANSMITTER_H

#include "v34_phase3_transmitter.h"
#include "v34_training_line.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_phase3_line_transmitter_result {
	V34_PHASE3_LINE_TRANSMITTER_OK,
	V34_PHASE3_LINE_TRANSMITTER_COMPLETE,
	V34_PHASE3_LINE_TRANSMITTER_INVALID_ARGUMENT,
	V34_PHASE3_LINE_TRANSMITTER_INVALID_STATE,
	V34_PHASE3_LINE_TRANSMITTER_OUT_OF_RANGE,
	V34_PHASE3_LINE_TRANSMITTER_CLIPPING_RISK,
	V34_PHASE3_LINE_TRANSMITTER_NUMERIC_FAILURE,
};

/* Own the zero-MD Phase 3 symbols and their uninterrupted line waveform for
 * either station role. One instance's sample and control calls must be
 * serialized by its caller. */
struct v34_phase3_line_transmitter {
	struct v34_training_line line;
	struct v34_phase3_transmitter symbols;
};

enum v34_phase3_line_transmitter_result
v34_phase3_line_transmitter_init(
	struct v34_phase3_line_transmitter *transmitter,
	const struct v34_phase3_profile *local_profile,
	enum v34_constellation_size remote_constellation,
	uint32_t sample_rate, int16_t nominal_peak_amplitude);

bool v34_phase3_line_transmitter_is_valid(
	const struct v34_phase3_line_transmitter *transmitter);

/* These controls take effect at the next point not yet accepted by the line.
 * Rejected calls leave the complete instance unchanged. */
enum v34_phase3_line_transmitter_result
v34_phase3_line_transmitter_start_j(
	struct v34_phase3_line_transmitter *transmitter);
enum v34_phase3_line_transmitter_result
v34_phase3_line_transmitter_finish_j(
	struct v34_phase3_line_transmitter *transmitter);

/* Emit exactly one line sample. Once J has finished, any in-flight pulse is
 * drained before COMPLETE is reported at the next point boundary. COMPLETE
 * and rejected calls leave both the instance and output unchanged. */
enum v34_phase3_line_transmitter_result
v34_phase3_line_transmitter_sample(
	struct v34_phase3_line_transmitter *transmitter, int16_t *sample);

/* A ready handoff is aligned to the next unconsumed point. Copying the line
 * preserves carrier, pulse-shaper, pre-emphasis, and sample-clock continuity;
 * the caller must continue only one of the resulting line owners. */
bool v34_phase3_line_transmitter_is_handoff_ready(
	const struct v34_phase3_line_transmitter *transmitter);
enum v34_phase3_line_transmitter_result
v34_phase3_line_transmitter_handoff(
	const struct v34_phase3_line_transmitter *transmitter,
	struct v34_training_line *line);

const char *v34_phase3_line_transmitter_result_name(
	enum v34_phase3_line_transmitter_result result);

#endif /* V34_PHASE3_LINE_TRANSMITTER_H */
