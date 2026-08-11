// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_info_audio.h"

_Static_assert(
	SIPPYMODEM_V34_PHASE2_DPSK_PULSE_SPAN_SYMBOLS % 2 == 0,
	"Phase 2 pulse span must split into equal causal halves");

bool sippymodem_v34_phase2_info_audio_transmitter_is_valid(
	const struct sippymodem_v34_phase2_info_audio_transmitter *transmitter)
{
	size_t frame_bit_count;

	if (transmitter == NULL ||
	    !sippymodem_v34_phase2_dpsk_transmitter_is_valid(
		    &transmitter->modulation) ||
	    !sippymodem_v34_phase2_info_generator_is_valid(
		    &transmitter->frame))
		return false;
	frame_bit_count = sippymodem_v34_phase2_info_frame_bit_count(
		transmitter->frame.payload_bit_count);
	return transmitter->frame_complete ==
	       (transmitter->frame.bit_index == frame_bit_count);
}

enum sippymodem_v34_phase2_info_audio_result
sippymodem_v34_phase2_info_audio_transmitter_init(
	struct sippymodem_v34_phase2_info_audio_transmitter *transmitter,
	enum sippymodem_v34_station_role local_role,
	int16_t nominal_amplitude, const uint8_t *payload,
	size_t payload_bit_count)
{
	struct sippymodem_v34_phase2_info_audio_transmitter initialized = { 0 };

	if (transmitter == NULL ||
	    sippymodem_v34_phase2_dpsk_transmitter_init(
		    &initialized.modulation, local_role,
		    nominal_amplitude) != SIPPYMODEM_V34_PHASE2_DPSK_OK ||
	    sippymodem_v34_phase2_info_generator_init(
		    &initialized.frame, payload, payload_bit_count) !=
		    SIPPYMODEM_V34_PHASE2_INFO_OK ||
	    !sippymodem_v34_phase2_info_audio_transmitter_is_valid(
		    &initialized))
		return SIPPYMODEM_V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT;
	*transmitter = initialized;
	return SIPPYMODEM_V34_PHASE2_INFO_AUDIO_OK;
}

enum sippymodem_v34_phase2_info_audio_result
sippymodem_v34_phase2_info_audio_transmitter_sample(
	struct sippymodem_v34_phase2_info_audio_transmitter *transmitter,
	int16_t *sample, bool *frame_complete)
{
	struct sippymodem_v34_phase2_info_audio_transmitter next;
	uint8_t bit = 0;
	int16_t generated_sample;
	bool accepts_bit;
	bool bit_accepted;
	bool bit_completes_frame = false;

	if (!sippymodem_v34_phase2_info_audio_transmitter_is_valid(
		    transmitter) ||
	    transmitter->frame_complete || sample == NULL ||
	    frame_complete == NULL)
		return SIPPYMODEM_V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT;
	next = *transmitter;
	accepts_bit = sippymodem_v34_phase2_dpsk_transmitter_can_accept_bit(
		&next.modulation);
	if (accepts_bit &&
	    sippymodem_v34_phase2_info_next_bit(
		    &next.frame, &bit, &bit_completes_frame) !=
		    SIPPYMODEM_V34_PHASE2_INFO_OK)
		return SIPPYMODEM_V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT;
	if (sippymodem_v34_phase2_dpsk_transmitter_sample(
		    &next.modulation, bit, &generated_sample,
		    &bit_accepted) != SIPPYMODEM_V34_PHASE2_DPSK_OK ||
	    bit_accepted != accepts_bit)
		return SIPPYMODEM_V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT;
	if (bit_completes_frame)
		next.frame_complete = true;
	if (!sippymodem_v34_phase2_info_audio_transmitter_is_valid(&next))
		return SIPPYMODEM_V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT;
	*transmitter = next;
	*sample = generated_sample;
	*frame_complete = bit_completes_frame;
	return SIPPYMODEM_V34_PHASE2_INFO_AUDIO_OK;
}

bool sippymodem_v34_phase2_info_audio_receiver_is_valid(
	const struct sippymodem_v34_phase2_info_audio_receiver *receiver)
{
	return receiver != NULL &&
	       sippymodem_v34_phase2_dpsk_receiver_is_valid(
		       &receiver->modulation) &&
	       sippymodem_v34_phase2_info_receiver_is_valid(&receiver->frame);
}

enum sippymodem_v34_phase2_info_audio_result
sippymodem_v34_phase2_info_audio_receiver_init(
	struct sippymodem_v34_phase2_info_audio_receiver *receiver,
	enum sippymodem_v34_station_role local_role,
	size_t expected_payload_bit_count)
{
	struct sippymodem_v34_phase2_info_audio_receiver initialized = { 0 };

	if (receiver == NULL ||
	    sippymodem_v34_phase2_dpsk_receiver_init(
		    &initialized.modulation, local_role) !=
		    SIPPYMODEM_V34_PHASE2_DPSK_OK ||
	    sippymodem_v34_phase2_info_receiver_init(
		    &initialized.frame, expected_payload_bit_count) !=
		    SIPPYMODEM_V34_PHASE2_INFO_OK ||
	    !sippymodem_v34_phase2_info_audio_receiver_is_valid(&initialized))
		return SIPPYMODEM_V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT;
	*receiver = initialized;
	return SIPPYMODEM_V34_PHASE2_INFO_AUDIO_OK;
}

enum sippymodem_v34_phase2_info_audio_result
sippymodem_v34_phase2_info_audio_receiver_reset(
	struct sippymodem_v34_phase2_info_audio_receiver *receiver)
{
	struct sippymodem_v34_phase2_info_audio_receiver reset;

	if (!sippymodem_v34_phase2_info_audio_receiver_is_valid(receiver))
		return SIPPYMODEM_V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT;
	reset = *receiver;
	if (sippymodem_v34_phase2_dpsk_receiver_reset(&reset.modulation) !=
		    SIPPYMODEM_V34_PHASE2_DPSK_OK ||
	    sippymodem_v34_phase2_info_receiver_reset(&reset.frame) !=
		    SIPPYMODEM_V34_PHASE2_INFO_OK ||
	    !sippymodem_v34_phase2_info_audio_receiver_is_valid(&reset))
		return SIPPYMODEM_V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT;
	*receiver = reset;
	return SIPPYMODEM_V34_PHASE2_INFO_AUDIO_OK;
}

enum sippymodem_v34_phase2_info_audio_result
sippymodem_v34_phase2_info_audio_receiver_sample(
	struct sippymodem_v34_phase2_info_audio_receiver *receiver,
	int16_t sample, struct sippymodem_v34_phase2_info_event *event)
{
	struct sippymodem_v34_phase2_info_audio_receiver next;
	struct sippymodem_v34_phase2_info_event emitted = { 0 };
	uint8_t bit;
	bool bit_ready;

	if (!sippymodem_v34_phase2_info_audio_receiver_is_valid(receiver) ||
	    event == NULL)
		return SIPPYMODEM_V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT;
	next = *receiver;
	if (sippymodem_v34_phase2_dpsk_receiver_sample(
		    &next.modulation, sample, &bit, &bit_ready) !=
		    SIPPYMODEM_V34_PHASE2_DPSK_OK ||
	    (bit_ready &&
	     sippymodem_v34_phase2_info_receive_bit(
		     &next.frame, bit, &emitted) !=
		     SIPPYMODEM_V34_PHASE2_INFO_OK) ||
	    !sippymodem_v34_phase2_info_audio_receiver_is_valid(&next))
		return SIPPYMODEM_V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT;
	*receiver = next;
	*event = emitted;
	return SIPPYMODEM_V34_PHASE2_INFO_AUDIO_OK;
}

struct sippymodem_v34_phase2_dpsk_measurement
sippymodem_v34_phase2_info_audio_receiver_measure(
	const struct sippymodem_v34_phase2_info_audio_receiver *receiver)
{
	if (!sippymodem_v34_phase2_info_audio_receiver_is_valid(receiver))
		return (struct sippymodem_v34_phase2_dpsk_measurement){ 0 };
	return sippymodem_v34_phase2_dpsk_receiver_measure(
		&receiver->modulation);
}

const char *sippymodem_v34_phase2_info_audio_result_name(
	enum sippymodem_v34_phase2_info_audio_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_PHASE2_INFO_AUDIO_OK:
		return "ok";
	case SIPPYMODEM_V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
