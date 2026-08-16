// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_ranging.h"

#include <stddef.h>

bool v34_phase2_ranging_is_valid(
	const struct v34_phase2_ranging *ranging)
{
	return ranging != NULL &&
	       v34_phase2_info0_exchange_is_valid(
		       &ranging->info0_exchange) &&
	       phase2_ranging_control_is_valid(
		       &ranging->control,
		       &ranging->info0_exchange.transport,
		       ranging->info0_exchange.peer_info0_received);
}

enum v34_phase2_ranging_result
v34_phase2_ranging_init(
	struct v34_phase2_ranging *ranging,
	enum v34_station_role local_role,
	int16_t nominal_amplitude,
	const struct v34_info0 *local_info0)
{
	struct v34_phase2_ranging initialized;

	if (ranging == NULL ||
	    v34_phase2_info0_exchange_init(
		    &initialized.info0_exchange, local_role,
		    nominal_amplitude, local_info0) !=
		    V34_PHASE2_INFO0_EXCHANGE_OK ||
	    phase2_ranging_control_init(
		    &initialized.control,
		    &initialized.info0_exchange.transport) !=
		    PHASE2_RANGING_OK ||
	    !v34_phase2_ranging_is_valid(&initialized))
		return V34_PHASE2_RANGING_INVALID_ARGUMENT;
	*ranging = initialized;
	return V34_PHASE2_RANGING_OK;
}

enum v34_phase2_ranging_result
v34_phase2_ranging_init_retrain(
	struct v34_phase2_ranging *ranging,
	enum v34_station_role local_role, int16_t nominal_amplitude,
	const struct v34_info0 *local_info0,
	const struct v34_info0 *peer_info0)
{
	struct v34_phase2_ranging initialized;

	if (ranging == NULL ||
	    v34_phase2_info0_exchange_init_retrain(
		    &initialized.info0_exchange, local_role,
		    nominal_amplitude, local_info0, peer_info0) !=
		    V34_PHASE2_INFO0_EXCHANGE_OK ||
	    phase2_ranging_control_init(
		    &initialized.control,
		    &initialized.info0_exchange.transport) !=
		    PHASE2_RANGING_OK ||
	    !v34_phase2_ranging_is_valid(&initialized))
		return V34_PHASE2_RANGING_INVALID_ARGUMENT;
	*ranging = initialized;
	return V34_PHASE2_RANGING_OK;
}

enum v34_phase2_ranging_result
v34_phase2_ranging_sample(
	struct v34_phase2_ranging *ranging,
	int16_t received_sample, int16_t *transmit_sample,
	struct v34_phase2_ranging_event *event)
{
	struct v34_phase2_ranging next;
	struct v34_phase2_info0_exchange_event exchange_event;
	struct phase2_ranging_event ranging_event;
	struct v34_phase2_ranging_event emitted;
	enum phase2_info0_transmit_phase transmit_phase_before;
	int16_t generated;

	if (!v34_phase2_ranging_is_valid(ranging) ||
	    transmit_sample == NULL || event == NULL)
		return V34_PHASE2_RANGING_INVALID_ARGUMENT;
	next = *ranging;
	if (phase2_ranging_prepare_sample(
		    &next.control, &next.info0_exchange.transport,
		    next.info0_exchange.peer_info0_received) !=
	    PHASE2_RANGING_OK)
		return V34_PHASE2_RANGING_INVALID_ARGUMENT;
	transmit_phase_before =
		next.info0_exchange.transport.transmit_phase;
	if (v34_phase2_info0_exchange_sample(
		    &next.info0_exchange, received_sample, &generated,
		    &exchange_event) != V34_PHASE2_INFO0_EXCHANGE_OK ||
	    (exchange_event.recovery_started &&
	     phase2_ranging_control_init(
		     &next.control,
		     &next.info0_exchange.transport) !=
		     PHASE2_RANGING_OK) ||
	    phase2_ranging_finish_sample(
		    &next.control, &next.info0_exchange.transport,
		    next.info0_exchange.peer_info0_received,
		    transmit_phase_before, &exchange_event.tone_event,
		    &generated, &ranging_event) != PHASE2_RANGING_OK ||
	    !v34_phase2_ranging_is_valid(&next))
		return V34_PHASE2_RANGING_INVALID_ARGUMENT;
	emitted = (struct v34_phase2_ranging_event){
		.exchange_event = exchange_event,
		.kind = ranging_event.kind,
		.round_trip_delay_samples =
			ranging_event.round_trip_delay_samples,
	};
	*ranging = next;
	*transmit_sample = generated;
	*event = emitted;
	return V34_PHASE2_RANGING_OK;
}

const char *v34_phase2_ranging_result_name(
	enum v34_phase2_ranging_result result)
{
	switch (result) {
	case V34_PHASE2_RANGING_OK:
		return "ok";
	case V34_PHASE2_RANGING_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
