// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE2_INFO_AUDIO_H
#define V34_PHASE2_INFO_AUDIO_H

#include "v34_phase2_dpsk.h"
#include "v34_phase2_info.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	/* Retain unchanged DPSK points through the causal half of the pulse
	 * shaper before handing the line to the following waveform owner. */
	V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS =
		V34_PHASE2_DPSK_PULSE_SPAN_SYMBOLS / 2,
	/* The receive filter centre is expressed in sixths of an 8 kHz sample:
	 * the carrier FIR runs at 8 kHz and the interpolator and detector FIRs
	 * run at three times that rate. */
	V34_PHASE2_INFO_RX_FILTER_DELAY_SIXTHS =
		3 * (V34_PHASE2_DPSK_CARRIER_FILTER_TAPS - 1) +
		(V34_PHASE2_DPSK_INTERPOLATION *
		 V34_PHASE2_DPSK_INTERPOLATOR_TAPS - 1) +
		(V34_PHASE2_DPSK_LOWPASS_TAPS - 1),
	V34_PHASE2_INFO_RX_FILTER_DELAY_SAMPLES =
		(V34_PHASE2_INFO_RX_FILTER_DELAY_SIXTHS + 3) / 6,
	/* The receiver recognizes sync after three causal transmit-filter
	 * symbols (40 samples), its rounded receive-filter centre (49), and the
	 * inclusive recovered-bit boundary (one).  This implementation-derived
	 * pipeline delay dates the following line transition without observing
	 * samples while the synchronized INFO receiver owns the carrier. */
	V34_PHASE2_INFO_TX_FILTER_DELAY_SAMPLES =
		V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS *
		V34_PHASE2_SAMPLE_RATE / V34_PHASE2_DPSK_BIT_RATE,
	V34_PHASE2_INFO_SYNC_PIPELINE_DELAY_SAMPLES =
		V34_PHASE2_INFO_TX_FILTER_DELAY_SAMPLES +
		V34_PHASE2_INFO_RX_FILTER_DELAY_SAMPLES + 1,
};

enum v34_phase2_info_audio_result {
	V34_PHASE2_INFO_AUDIO_OK,
	V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT,
};

struct v34_phase2_info_audio_transmitter {
	struct v34_phase2_dpsk_transmitter modulation;
	struct v34_phase2_info_generator frame;
	bool frame_complete;
};

struct v34_phase2_info_audio_receiver {
	struct v34_phase2_dpsk_receiver modulation;
	struct v34_phase2_info_receiver frame;
	uint16_t samples_since_sync;
	uint16_t handoff_delay_samples;
};

/* One transmitter emits one INFO sequence, including its arbitrary reference
 * point. A Phase 2 controller owns any following tone or repeated group. */
enum v34_phase2_info_audio_result
v34_phase2_info_audio_transmitter_init(
	struct v34_phase2_info_audio_transmitter *transmitter,
	enum v34_station_role local_role,
	int16_t nominal_amplitude, const uint8_t *payload,
	size_t payload_bit_count);
bool v34_phase2_info_audio_transmitter_is_valid(
	const struct v34_phase2_info_audio_transmitter *transmitter);

/* frame_complete is true on the sample that accepts the final sequence bit.
 * Producing another sample from a completed one-shot transmitter is invalid. */
enum v34_phase2_info_audio_result
v34_phase2_info_audio_transmitter_sample(
	struct v34_phase2_info_audio_transmitter *transmitter,
	int16_t *sample, bool *frame_complete);

enum v34_phase2_info_audio_result
v34_phase2_info_audio_receiver_init(
	struct v34_phase2_info_audio_receiver *receiver,
	enum v34_station_role local_role,
	size_t expected_payload_bit_count);
bool v34_phase2_info_audio_receiver_is_valid(
	const struct v34_phase2_info_audio_receiver *receiver);
enum v34_phase2_info_audio_result
v34_phase2_info_audio_receiver_reset(
	struct v34_phase2_info_audio_receiver *receiver);

/* Each successful call emits either NO_EVENT or the typed frame event from
 * the first recovered bit boundary in this sample. */
enum v34_phase2_info_audio_result
v34_phase2_info_audio_receiver_sample(
	struct v34_phase2_info_audio_receiver *receiver,
	int16_t sample, struct v34_phase2_info_event *event);
struct v34_phase2_dpsk_measurement
v34_phase2_info_audio_receiver_measure(
	const struct v34_phase2_info_audio_receiver *receiver);

/* Return the implementation-derived delay from the peer's following-tone
 * transition through delivery of a just-completed, tail-valid INFO frame.
 * Other states, including a rejected physical frame, return zero. */
uint16_t v34_phase2_info_audio_receiver_handoff_delay_samples(
	const struct v34_phase2_info_audio_receiver *receiver);

const char *v34_phase2_info_audio_result_name(
	enum v34_phase2_info_audio_result result);

#endif
