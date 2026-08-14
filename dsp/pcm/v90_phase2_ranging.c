// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase2_ranging.h"

#include <stddef.h>

bool v90_phase2_ranging_is_valid(
	const struct v90_phase2_ranging *ranging)
{
	return ranging != NULL &&
	       pcm_phase2_info0_exchange_is_valid(
		       &ranging->info0_exchange) &&
	       phase2_ranging_control_is_valid(
		       &ranging->control,
		       &ranging->info0_exchange.transport,
		       ranging->info0_exchange.peer_info0_received);
}

static enum v90_phase2_ranging_result finish_initialization(
	struct v90_phase2_ranging *ranging,
	struct v90_phase2_ranging *initialized,
	enum pcm_phase2_info0_exchange_result exchange_result)
{
	if (ranging == NULL ||
	    exchange_result != PCM_PHASE2_INFO0_EXCHANGE_OK ||
	    phase2_ranging_control_init(
		    &initialized->control,
		    &initialized->info0_exchange.transport) !=
		    PHASE2_RANGING_OK ||
	    !v90_phase2_ranging_is_valid(initialized))
		return V90_PHASE2_RANGING_INVALID_ARGUMENT;
	*ranging = *initialized;
	return V90_PHASE2_RANGING_OK;
}

enum v90_phase2_ranging_result v90_phase2_ranging_init_analog(
	struct v90_phase2_ranging *ranging,
	int16_t nominal_amplitude,
	const struct v90_info0 *local_info0a)
{
	struct v90_phase2_ranging initialized;
	enum pcm_phase2_info0_exchange_result result;

	if (ranging == NULL)
		return V90_PHASE2_RANGING_INVALID_ARGUMENT;
	result = pcm_phase2_info0_exchange_init_v90_analog(
		&initialized.info0_exchange, nominal_amplitude, local_info0a);
	return finish_initialization(ranging, &initialized, result);
}

enum v90_phase2_ranging_result v90_phase2_ranging_init_digital(
	struct v90_phase2_ranging *ranging,
	int16_t nominal_amplitude,
	const struct v90_info0d *local_info0d)
{
	struct v90_phase2_ranging initialized;
	enum pcm_phase2_info0_exchange_result result;

	if (ranging == NULL)
		return V90_PHASE2_RANGING_INVALID_ARGUMENT;
	result = pcm_phase2_info0_exchange_init_v90_digital(
		&initialized.info0_exchange, nominal_amplitude, local_info0d);
	return finish_initialization(ranging, &initialized, result);
}

enum v90_phase2_ranging_result v90_phase2_ranging_sample(
	struct v90_phase2_ranging *ranging,
	int16_t received_sample, int16_t *transmit_sample,
	struct v90_phase2_ranging_event *event)
{
	struct v90_phase2_ranging next;
	struct pcm_phase2_info0_exchange_event exchange_event;
	struct phase2_ranging_event ranging_event;
	struct v90_phase2_ranging_event emitted;
	enum phase2_info0_transmit_phase transmit_phase_before;
	int16_t generated;

	if (!v90_phase2_ranging_is_valid(ranging) ||
	    transmit_sample == NULL || event == NULL)
		return V90_PHASE2_RANGING_INVALID_ARGUMENT;
	next = *ranging;
	if (phase2_ranging_prepare_sample(
		    &next.control, &next.info0_exchange.transport,
		    next.info0_exchange.peer_info0_received) !=
	    PHASE2_RANGING_OK)
		return V90_PHASE2_RANGING_INVALID_ARGUMENT;
	transmit_phase_before = next.info0_exchange.transport.transmit_phase;
	if (pcm_phase2_info0_exchange_sample(
		    &next.info0_exchange, received_sample, &generated,
		    &exchange_event) != PCM_PHASE2_INFO0_EXCHANGE_OK ||
	    phase2_ranging_finish_sample(
		    &next.control, &next.info0_exchange.transport,
		    next.info0_exchange.peer_info0_received,
		    transmit_phase_before, &exchange_event.tone_event,
		    &generated, &ranging_event) != PHASE2_RANGING_OK ||
	    !v90_phase2_ranging_is_valid(&next))
		return V90_PHASE2_RANGING_INVALID_ARGUMENT;
	emitted = (struct v90_phase2_ranging_event){
		.exchange_event = exchange_event,
		.kind = ranging_event.kind,
		.round_trip_delay_samples =
			ranging_event.round_trip_delay_samples,
	};
	*ranging = next;
	*transmit_sample = generated;
	*event = emitted;
	return V90_PHASE2_RANGING_OK;
}

const char *v90_phase2_ranging_result_name(
	enum v90_phase2_ranging_result result)
{
	switch (result) {
	case V90_PHASE2_RANGING_OK:
		return "ok";
	case V90_PHASE2_RANGING_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
