// SPDX-License-Identifier: BSD-3-Clause

#ifndef PHASE2_INFO0_EXCHANGE_H
#define PHASE2_INFO0_EXCHANGE_H

#include "phase2_signal.h"
#include "v34_phase2_info_audio.h"
#include "v34_phase2_tone.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	PHASE2_INFO0_POST_V8_SILENCE_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 75 / 1000,
};

enum phase2_info0_exchange_result {
	PHASE2_INFO0_EXCHANGE_OK,
	PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT,
};

enum phase2_info0_transmit_phase {
	PHASE2_INFO0_POST_V8_SILENCE,
	PHASE2_INFO0_SEQUENCE,
	PHASE2_INFO0_CARRIER_TAIL,
	PHASE2_INFO0_TONE,
};

struct phase2_info0_exchange_event {
	struct v34_phase2_info_event info_event;
	struct v34_phase2_tone_event tone_event;
};

struct phase2_info0_exchange {
	struct v34_phase2_info_audio_transmitter info_transmitter;
	struct v34_phase2_info_audio_receiver info_receiver;
	struct v34_phase2_tone_transmitter tone_transmitter;
	struct v34_phase2_tone_detector tone_detector;
	uint16_t post_v8_silence_samples;
	uint8_t filter_tail_symbols;
	enum phase2_info0_transmit_phase transmit_phase;
	enum phase2_signal_role signal_role;
};

enum phase2_info0_exchange_result phase2_info0_exchange_init(
	struct phase2_info0_exchange *exchange,
	enum phase2_signal_role signal_role,
	int16_t nominal_amplitude,
	const uint8_t *local_payload, size_t local_payload_bit_count,
	size_t peer_payload_bit_count);
bool phase2_info0_exchange_is_valid(
	const struct phase2_info0_exchange *exchange);

/* Restart INFO immediately from the following-tone state.  The new arbitrary
 * reference point inherits the carrier and guard phases for a continuous
 * recovery handoff; no post-V.8 silence is inserted. */
enum phase2_info0_exchange_result phase2_info0_exchange_restart_info(
	struct phase2_info0_exchange *exchange,
	const uint8_t *local_payload, size_t local_payload_bit_count);

/* One call consumes and produces one 8 kHz line sample.  Reception is active
 * throughout the post-V.8 transmit silence so an early peer INFO0 is retained.
 * The INFO frame and tone events are independent and may both be populated by
 * the same sample. */
enum phase2_info0_exchange_result phase2_info0_exchange_sample(
	struct phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_info0_exchange_event *event);

#endif
