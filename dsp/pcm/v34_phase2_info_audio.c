// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_info_audio.h"

_Static_assert(
	V34_PHASE2_DPSK_PULSE_SPAN_SYMBOLS % 2 == 0,
	"Phase 2 pulse span must split into equal causal halves");
_Static_assert(
	V34_PHASE2_INFO_RX_FILTER_DELAY_SIXTHS == 291 &&
	V34_PHASE2_INFO_RX_FILTER_DELAY_SAMPLES == 49 &&
	V34_PHASE2_INFO_TX_FILTER_DELAY_SAMPLES == 40 &&
	V34_PHASE2_INFO_SYNC_PIPELINE_DELAY_SAMPLES == 90,
	"Phase 2 INFO timing must follow the causal audio pipeline");

static uint16_t following_tone_samples_after_sync(
	size_t payload_bit_count)
{
	size_t remaining_symbols =
		payload_bit_count + V34_PHASE2_INFO_CRC_BITS +
		V34_PHASE2_INFO_TRAILING_FILL_BITS +
		V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS;
	size_t samples =
		(remaining_symbols * V34_PHASE2_SAMPLE_RATE +
		 V34_PHASE2_DPSK_BIT_RATE / 2) /
		V34_PHASE2_DPSK_BIT_RATE;

	/* The division rounds the line interval to its nearest 8 kHz sample.
	 * Sync delivery already includes the 90-sample causal pipeline above. */
	return samples > V34_PHASE2_INFO_SYNC_PIPELINE_DELAY_SAMPLES ?
		(uint16_t)(samples -
			   V34_PHASE2_INFO_SYNC_PIPELINE_DELAY_SAMPLES) : 0;
}

bool v34_phase2_info_audio_transmitter_is_valid(
	const struct v34_phase2_info_audio_transmitter *transmitter)
{
	size_t frame_bit_count;

	if (transmitter == NULL ||
	    !v34_phase2_dpsk_transmitter_is_valid(
		    &transmitter->modulation) ||
	    !v34_phase2_info_generator_is_valid(
		    &transmitter->frame))
		return false;
	frame_bit_count = v34_phase2_info_frame_bit_count(
		transmitter->frame.payload_bit_count);
	return transmitter->frame_complete ==
	       (transmitter->frame.bit_index == frame_bit_count);
}

enum v34_phase2_info_audio_result
v34_phase2_info_audio_transmitter_init(
	struct v34_phase2_info_audio_transmitter *transmitter,
	enum v34_station_role local_role,
	int16_t nominal_amplitude, const uint8_t *payload,
	size_t payload_bit_count)
{
	struct v34_phase2_info_audio_transmitter initialized = { 0 };

	if (transmitter == NULL ||
	    v34_phase2_dpsk_transmitter_init(
		    &initialized.modulation, local_role,
		    nominal_amplitude) != V34_PHASE2_DPSK_OK ||
	    v34_phase2_info_generator_init(
		    &initialized.frame, payload, payload_bit_count) !=
		    V34_PHASE2_INFO_OK ||
	    !v34_phase2_info_audio_transmitter_is_valid(
		    &initialized))
		return V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT;
	*transmitter = initialized;
	return V34_PHASE2_INFO_AUDIO_OK;
}

enum v34_phase2_info_audio_result
v34_phase2_info_audio_transmitter_sample(
	struct v34_phase2_info_audio_transmitter *transmitter,
	int16_t *sample, bool *frame_complete)
{
	struct v34_phase2_info_audio_transmitter next;
	uint8_t bit = 0;
	int16_t generated_sample;
	bool accepts_bit;
	bool bit_accepted;
	bool bit_completes_frame = false;

	if (!v34_phase2_info_audio_transmitter_is_valid(
		    transmitter) ||
	    transmitter->frame_complete || sample == NULL ||
	    frame_complete == NULL)
		return V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT;
	next = *transmitter;
	accepts_bit = v34_phase2_dpsk_transmitter_can_accept_bit(
		&next.modulation);
	if (accepts_bit &&
	    v34_phase2_info_next_bit(
		    &next.frame, &bit, &bit_completes_frame) !=
		    V34_PHASE2_INFO_OK)
		return V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT;
	if (v34_phase2_dpsk_transmitter_sample(
		    &next.modulation, bit, &generated_sample,
		    &bit_accepted) != V34_PHASE2_DPSK_OK ||
	    bit_accepted != accepts_bit)
		return V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT;
	if (bit_completes_frame)
		next.frame_complete = true;
	if (!v34_phase2_info_audio_transmitter_is_valid(&next))
		return V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT;
	*transmitter = next;
	*sample = generated_sample;
	*frame_complete = bit_completes_frame;
	return V34_PHASE2_INFO_AUDIO_OK;
}

bool v34_phase2_info_audio_receiver_is_valid(
	const struct v34_phase2_info_audio_receiver *receiver)
{
	if (receiver == NULL ||
	    !v34_phase2_dpsk_receiver_is_valid(&receiver->modulation) ||
	    !v34_phase2_info_receiver_is_valid(&receiver->frame))
		return false;
	if (receiver->frame.stage != V34_PHASE2_INFO_SEARCH_SYNC)
		return receiver->handoff_delay_samples == 0;
	return receiver->samples_since_sync == 0 &&
	       receiver->handoff_delay_samples <=
		       V34_PHASE2_INFO_SYNC_PIPELINE_DELAY_SAMPLES;
}

enum v34_phase2_info_audio_result
v34_phase2_info_audio_receiver_init(
	struct v34_phase2_info_audio_receiver *receiver,
	enum v34_station_role local_role,
	size_t expected_payload_bit_count)
{
	struct v34_phase2_info_audio_receiver initialized = { 0 };

	if (receiver == NULL ||
	    v34_phase2_dpsk_receiver_init(
		    &initialized.modulation, local_role) !=
		    V34_PHASE2_DPSK_OK ||
	    v34_phase2_info_receiver_init(
		    &initialized.frame, expected_payload_bit_count) !=
		    V34_PHASE2_INFO_OK ||
	    !v34_phase2_info_audio_receiver_is_valid(&initialized))
		return V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_PHASE2_INFO_AUDIO_OK;
}

enum v34_phase2_info_audio_result
v34_phase2_info_audio_receiver_reset(
	struct v34_phase2_info_audio_receiver *receiver)
{
	struct v34_phase2_info_audio_receiver reset;

	if (!v34_phase2_info_audio_receiver_is_valid(receiver))
		return V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT;
	reset = *receiver;
	reset.samples_since_sync = 0;
	reset.handoff_delay_samples = 0;
	if (v34_phase2_dpsk_receiver_reset(&reset.modulation) !=
		    V34_PHASE2_DPSK_OK ||
	    v34_phase2_info_receiver_reset(&reset.frame) !=
		    V34_PHASE2_INFO_OK ||
	    !v34_phase2_info_audio_receiver_is_valid(&reset))
		return V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT;
	*receiver = reset;
	return V34_PHASE2_INFO_AUDIO_OK;
}

enum v34_phase2_info_audio_result
v34_phase2_info_audio_receiver_sample(
	struct v34_phase2_info_audio_receiver *receiver,
	int16_t sample, struct v34_phase2_info_event *event)
{
	struct v34_phase2_info_audio_receiver next;
	struct v34_phase2_info_event emitted = { 0 };
	enum v34_phase2_info_receive_stage stage_before;
	uint8_t bit;
	bool bit_ready;

	if (!v34_phase2_info_audio_receiver_is_valid(receiver) ||
	    event == NULL)
		return V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT;
	next = *receiver;
	stage_before = next.frame.stage;
	/* A completed delay is consumable only on its frame-event sample. */
	next.handoff_delay_samples = 0;
	if (v34_phase2_dpsk_receiver_sample(
		    &next.modulation, sample, &bit, &bit_ready) !=
		    V34_PHASE2_DPSK_OK ||
	    (bit_ready &&
	     v34_phase2_info_receive_bit(
		     &next.frame, bit, &emitted) !=
		     V34_PHASE2_INFO_OK))
		return V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT;
	if (stage_before == V34_PHASE2_INFO_SEARCH_SYNC &&
	    next.frame.stage != V34_PHASE2_INFO_SEARCH_SYNC) {
		next.samples_since_sync = 0;
	} else if (stage_before != V34_PHASE2_INFO_SEARCH_SYNC) {
		uint16_t following_tone_samples =
			following_tone_samples_after_sync(
				next.frame.expected_payload_bit_count);

		if (next.samples_since_sync == UINT16_MAX)
			return V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT;
		next.samples_since_sync++;
		if (emitted.type != V34_PHASE2_INFO_NO_EVENT) {
			uint16_t delay = next.samples_since_sync >
						 following_tone_samples ?
				(uint16_t)(next.samples_since_sync -
					   following_tone_samples) : 0;

			/* CRC rejection precedes trailing fill and transmit-filter
			 * drain, so only a tail-complete frame has a physical line-edge
			 * bound. A receiver resynchronization starts fresh lineage. */
			if (emitted.type == V34_PHASE2_INFO_FRAME_RECEIVED &&
			    delay <= V34_PHASE2_INFO_SYNC_PIPELINE_DELAY_SAMPLES)
				next.handoff_delay_samples = delay;
			next.samples_since_sync = 0;
		}
	}
	if (!v34_phase2_info_audio_receiver_is_valid(&next))
		return V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT;
	*receiver = next;
	*event = emitted;
	return V34_PHASE2_INFO_AUDIO_OK;
}

struct v34_phase2_dpsk_measurement
v34_phase2_info_audio_receiver_measure(
	const struct v34_phase2_info_audio_receiver *receiver)
{
	if (!v34_phase2_info_audio_receiver_is_valid(receiver))
		return (struct v34_phase2_dpsk_measurement){ 0 };
	return v34_phase2_dpsk_receiver_measure(
		&receiver->modulation);
}

uint16_t v34_phase2_info_audio_receiver_handoff_delay_samples(
	const struct v34_phase2_info_audio_receiver *receiver)
{
	if (!v34_phase2_info_audio_receiver_is_valid(receiver))
		return 0;
	return receiver->handoff_delay_samples;
}

const char *v34_phase2_info_audio_result_name(
	enum v34_phase2_info_audio_result result)
{
	switch (result) {
	case V34_PHASE2_INFO_AUDIO_OK:
		return "ok";
	case V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
