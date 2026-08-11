// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_info_audio.h"

_Static_assert(
	V34_PHASE2_DPSK_PULSE_SPAN_SYMBOLS % 2 == 0,
	"Phase 2 pulse span must split into equal causal halves");

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
	return receiver != NULL &&
	       v34_phase2_dpsk_receiver_is_valid(
		       &receiver->modulation) &&
	       v34_phase2_info_receiver_is_valid(&receiver->frame);
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
	uint8_t bit;
	bool bit_ready;

	if (!v34_phase2_info_audio_receiver_is_valid(receiver) ||
	    event == NULL)
		return V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT;
	next = *receiver;
	if (v34_phase2_dpsk_receiver_sample(
		    &next.modulation, sample, &bit, &bit_ready) !=
		    V34_PHASE2_DPSK_OK ||
	    (bit_ready &&
	     v34_phase2_info_receive_bit(
		     &next.frame, bit, &emitted) !=
		     V34_PHASE2_INFO_OK) ||
	    !v34_phase2_info_audio_receiver_is_valid(&next))
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
