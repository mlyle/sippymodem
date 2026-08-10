// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE2_INFO0_EXCHANGE_H
#define SIPPYMODEM_V34_PHASE2_INFO0_EXCHANGE_H

#include "sippymodem_v34_info0.h"
#include "sippymodem_v34_phase2_info_audio.h"
#include "sippymodem_v34_phase2_tone.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_PHASE2_POST_V8_SILENCE_SAMPLES =
		SIPPYMODEM_V34_PHASE2_SAMPLE_RATE * 75 / 1000,
	/* Retain unchanged DPSK points through the causal half of the pulse
	 * shaper before handing its phase to the following pure carrier. */
	SIPPYMODEM_V34_PHASE2_INFO0_FILTER_TAIL_SYMBOLS =
		SIPPYMODEM_V34_PHASE2_DPSK_PULSE_SPAN_SYMBOLS / 2,
};

enum sippymodem_v34_phase2_info0_exchange_result {
	SIPPYMODEM_V34_PHASE2_INFO0_EXCHANGE_OK,
	SIPPYMODEM_V34_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT,
};

enum sippymodem_v34_phase2_info0_transmit_phase {
	SIPPYMODEM_V34_PHASE2_INFO0_POST_V8_SILENCE,
	SIPPYMODEM_V34_PHASE2_INFO0_SEQUENCE,
	SIPPYMODEM_V34_PHASE2_INFO0_CARRIER_TAIL,
	SIPPYMODEM_V34_PHASE2_INFO0_TONE,
};

enum sippymodem_v34_phase2_peer_info0_event {
	SIPPYMODEM_V34_PHASE2_PEER_INFO0_NO_EVENT,
	SIPPYMODEM_V34_PHASE2_PEER_INFO0_RECEIVED,
	SIPPYMODEM_V34_PHASE2_PEER_INFO0_REJECTED,
};

struct sippymodem_v34_phase2_info0_exchange_event {
	enum sippymodem_v34_phase2_peer_info0_event peer_info0_event;
	struct sippymodem_v34_info0 peer_info0;
	struct sippymodem_v34_phase2_tone_event tone_event;
};

struct sippymodem_v34_phase2_info0_exchange {
	struct sippymodem_v34_phase2_info_audio_transmitter info_transmitter;
	struct sippymodem_v34_phase2_info_audio_receiver info_receiver;
	struct sippymodem_v34_phase2_tone_transmitter tone_transmitter;
	struct sippymodem_v34_phase2_tone_detector tone_detector;
	struct sippymodem_v34_info0 peer_info0;
	uint16_t post_v8_silence_samples;
	uint8_t filter_tail_symbols;
	enum sippymodem_v34_phase2_info0_transmit_phase transmit_phase;
	enum sippymodem_v34_phase2_station_role local_role;
	bool peer_info0_received;
};

/* Initial INFO0 always clears its acknowledgment bit. Recovery repetition and
 * acknowledgment are owned by the later ranging controller. */
enum sippymodem_v34_phase2_info0_exchange_result
sippymodem_v34_phase2_info0_exchange_init(
	struct sippymodem_v34_phase2_info0_exchange *exchange,
	enum sippymodem_v34_phase2_station_role local_role,
	int16_t nominal_amplitude,
	const struct sippymodem_v34_info0 *local_info0);
bool sippymodem_v34_phase2_info0_exchange_is_valid(
	const struct sippymodem_v34_phase2_info0_exchange *exchange);

/* One call consumes and produces one 8 kHz line sample. INFO0 and tone events
 * are independent and may both be populated by the same sample. */
enum sippymodem_v34_phase2_info0_exchange_result
sippymodem_v34_phase2_info0_exchange_sample(
	struct sippymodem_v34_phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct sippymodem_v34_phase2_info0_exchange_event *event);

const char *sippymodem_v34_phase2_info0_exchange_result_name(
	enum sippymodem_v34_phase2_info0_exchange_result result);

#endif
