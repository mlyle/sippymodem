// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_DATA_STREAM_H
#define V34_DATA_STREAM_H

#include "v34_data_mapping.h"
#include "v34_data_points.h"
#include "v34_training_line.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_data_stream_result {
	V34_DATA_STREAM_OK,
	V34_DATA_STREAM_SOURCE_FAILURE,
	V34_DATA_STREAM_INVALID_ARGUMENT,
	V34_DATA_STREAM_OUT_OF_RANGE,
	V34_DATA_STREAM_CLIPPING_RISK,
	V34_DATA_STREAM_NUMERIC_FAILURE,
	V34_DATA_STREAM_IN_PROGRESS,
};

/* Supply one complete post-B1 mapping frame. The source must initialize every
 * requested primary bit and, when non-NULL, the auxiliary bit to zero or one.
 * Returning false rejects the frame. A NULL source deliberately transmits idle
 * ones, matching an empty modem bit queue. */
typedef bool (*v34_mapping_frame_source)(
	void *context,
	const struct v34_mapping_frame *request,
	uint8_t *primary_bits, uint8_t *auxiliary_bit);

struct v34_data_stream {
	struct v34_data_points points;
	struct v34_training_line line;
	/* The data scrambler accepts 16-bit DTE words. B1 can leave that word
	 * cursor between mapping-frame boundaries, so retain the exact pending
	 * mark bits as well as their negotiated delay. */
	uint32_t delayed_primary_bits;
	uint8_t primary_delay_bits;
	bool b1_active;
};
/* Bound the pending mapping frame, word delay and finite line-filter tail. */
int v34_data_stream_tx_tail_samples(const struct v34_data_stream *stream);

/* Immutable B1 preparation, including the profile- and sample-rate-specific
 * peak used by the clipping preflight. No carrier/filter history is captured:
 * that remains owned by the active training waveform until the E boundary. */
struct v34_data_stream_preparation {
	struct v34_data_mapping mapping;
	double line_peak;
	uint32_t sample_rate;
};

/* Own the same transmit preparation used by standalone V.34 and PCM calls.
 * Each step visits at most frame_budget mapping frames in one power pass. */
struct v34_data_stream_preparation_job {
	struct v34_data_stream_preparation preparation;
	union {
		struct v34_mapping_power_calibration mapping;
		struct v34_line_power_calibration line;
	} calibration;
	bool calibrating_line;
	bool complete;
};

enum v34_data_stream_result v34_data_stream_begin_preparation(
	struct v34_data_stream_preparation_job *job,
	const struct v34_phase3_profile *profile,
	const struct v34_data_direction_parameters *direction,
	bool auxiliary_channel_enabled, uint32_t sample_rate);
/* Output changes only on completion. It must be disjoint from the active
 * job; a completed job may return its preparation in place. Discard a job
 * after any terminal failure. */
enum v34_data_stream_result v34_data_stream_step_preparation(
	struct v34_data_stream_preparation_job *job, uint32_t frame_budget,
	struct v34_data_stream_preparation *preparation);

bool v34_data_stream_preparation_is_valid(
	const struct v34_data_stream_preparation *preparation);
/* Continue an active training waveform with the completed preparation. Only
 * attach live line history and check its gain here: no generated calibration
 * stream may run while the first B1 samples are already due. */
enum v34_data_stream_result v34_data_stream_continue_prepared_b1(
	struct v34_data_stream *stream,
	const struct v34_training_line *active_line,
	const struct v34_data_stream_preparation *preparation);
bool v34_data_stream_is_valid(
	const struct v34_data_stream *stream);
bool v34_data_stream_is_transmitting_b1(
	const struct v34_data_stream *stream);

/* Emit one line sample. Mapping-frame acquisition, queueing, B1 handoff, and
 * line emission commit together; rejected calls leave the stream and both
 * outputs unchanged. The source itself owns any external read side effects. */
enum v34_data_stream_result v34_data_stream_sample(
	struct v34_data_stream *stream,
	v34_mapping_frame_source source, void *source_context,
	int16_t *sample, bool *symbol_accepted);

#endif
