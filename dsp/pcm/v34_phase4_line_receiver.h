// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE4_LINE_RECEIVER_H
#define V34_PHASE4_LINE_RECEIVER_H

#include "v34_data_line_receiver.h"
#include "v34_phase4_equalizer_receiver.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_phase4_line_receiver_result {
	/* A line sample was accepted without producing a control symbol. */
	V34_PHASE4_LINE_RECEIVER_OK,
	V34_PHASE4_LINE_RECEIVER_SYMBOL,
	V34_PHASE4_LINE_RECEIVER_S_DETECTED,
	V34_PHASE4_LINE_RECEIVER_RECORD_READY,
	V34_PHASE4_LINE_RECEIVER_E_COMPLETE,
	V34_PHASE4_LINE_RECEIVER_COMPLETE,
	/* A prior sample crossed an unrecoverable receive boundary. */
	V34_PHASE4_LINE_RECEIVER_FAILED,
	V34_PHASE4_LINE_RECEIVER_INVALID_ARGUMENT,
	V34_PHASE4_LINE_RECEIVER_INVALID_STATE,
	V34_PHASE4_LINE_RECEIVER_NUMERIC_FAILURE,
	V34_PHASE4_LINE_RECEIVER_SEQUENCE_MISMATCH,
};

/* Own the trained carrier, timing, equalizer, and Phase 4 control parser on
 * one sample timeline. A terminal point-domain error is latched because the
 * demodulator may already have consumed later waveform history. */
struct v34_phase4_line_receiver {
	struct qam_demodulator demodulator;
	struct v34_phase4_equalizer_receiver control;
	struct v34_phase3_profile transmitting_profile;
	bool failed;
};

/* Begin at the Phase 3-to-Phase 4 boundary. Both trained inputs are copied
 * and must describe the profile's exact waveform with timing locked. */
enum v34_phase4_line_receiver_result
v34_phase4_line_receiver_init(
	struct v34_phase4_line_receiver *receiver,
	const struct qam_demodulator *demodulator,
	const struct qam_equalizer *equalizer,
	const struct v34_phase3_profile *transmitting_profile,
	enum v34_constellation_size constellation, double adaptation);

bool v34_phase4_line_receiver_is_valid(
	const struct v34_phase4_line_receiver *receiver);
bool v34_phase4_line_receiver_has_failed(
	const struct v34_phase4_line_receiver *receiver);

/* A record event pauses the line before another sample can be consumed.
 * Continue MP search after a rejected/intermediate record, or start E after
 * accepting the final MP-prime record. */
enum v34_phase4_line_receiver_result
v34_phase4_line_receiver_continue_mp(
	struct v34_phase4_line_receiver *receiver);
enum v34_phase4_line_receiver_result
v34_phase4_line_receiver_start_e(
	struct v34_phase4_line_receiver *receiver);

/* Consume one signed line sample. `event` changes only for RECORD_READY.
 * Invalid, paused, complete, and already-failed calls consume nothing. A
 * terminal receive error consumes the sample, latches failure, and leaves the
 * event unchanged. */
enum v34_phase4_line_receiver_result
v34_phase4_line_receiver_sample(
	struct v34_phase4_line_receiver *receiver, int16_t sample,
	struct v34_mp_receive_event *event);

/* Initialize the continuous B1/data line at this exact completed-E sample
 * boundary. The destination changes only on success. */
enum v34_phase4_line_receiver_result
v34_phase4_line_receiver_start_data(
	const struct v34_phase4_line_receiver *receiver,
	struct v34_data_line_receiver *data_receiver,
	const struct v34_data_line_receiver_configuration *configuration);

const char *v34_phase4_line_receiver_result_name(
	enum v34_phase4_line_receiver_result result);

#endif /* V34_PHASE4_LINE_RECEIVER_H */
