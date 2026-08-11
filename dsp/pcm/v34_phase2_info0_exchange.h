// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE2_INFO0_EXCHANGE_H
#define V34_PHASE2_INFO0_EXCHANGE_H

#include "v34_info0.h"
#include "v34_phase2_info_audio.h"
#include "v34_phase2_tone.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_PHASE2_POST_V8_SILENCE_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 75 / 1000,
};

enum v34_phase2_info0_exchange_result {
	V34_PHASE2_INFO0_EXCHANGE_OK,
	V34_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT,
};

enum v34_phase2_info0_transmit_phase {
	V34_PHASE2_INFO0_POST_V8_SILENCE,
	V34_PHASE2_INFO0_SEQUENCE,
	V34_PHASE2_INFO0_CARRIER_TAIL,
	V34_PHASE2_INFO0_TONE,
};

enum v34_phase2_peer_info0_event {
	V34_PHASE2_PEER_INFO0_NO_EVENT,
	V34_PHASE2_PEER_INFO0_RECEIVED,
	V34_PHASE2_PEER_INFO0_REJECTED,
};

struct v34_phase2_info0_exchange_event {
	enum v34_phase2_peer_info0_event peer_info0_event;
	struct v34_info0 peer_info0;
	struct v34_phase2_tone_event tone_event;
};

struct v34_phase2_info0_exchange {
	struct v34_phase2_info_audio_transmitter info_transmitter;
	struct v34_phase2_info_audio_receiver info_receiver;
	struct v34_phase2_tone_transmitter tone_transmitter;
	struct v34_phase2_tone_detector tone_detector;
	struct v34_info0 peer_info0;
	uint16_t post_v8_silence_samples;
	uint8_t filter_tail_symbols;
	enum v34_phase2_info0_transmit_phase transmit_phase;
	enum v34_station_role local_role;
	bool peer_info0_received;
};

/* Initial INFO0 always clears its acknowledgment bit. Recovery repetition and
 * acknowledgment are owned by the later ranging controller. */
enum v34_phase2_info0_exchange_result
v34_phase2_info0_exchange_init(
	struct v34_phase2_info0_exchange *exchange,
	enum v34_station_role local_role,
	int16_t nominal_amplitude,
	const struct v34_info0 *local_info0);
bool v34_phase2_info0_exchange_is_valid(
	const struct v34_phase2_info0_exchange *exchange);

/* One call consumes and produces one 8 kHz line sample. INFO0 and tone events
 * are independent and may both be populated by the same sample. */
enum v34_phase2_info0_exchange_result
v34_phase2_info0_exchange_sample(
	struct v34_phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct v34_phase2_info0_exchange_event *event);

const char *v34_phase2_info0_exchange_result_name(
	enum v34_phase2_info0_exchange_result result);

#endif
