// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_phase2_ranging.h"

#include <stddef.h>

static bool ranging_context_is_retained(
	const struct pcm_phase2_ranging *ranging)
{
	switch (ranging->info0_exchange.state) {
	case PCM_PHASE2_INFO0_INITIAL:
	case PCM_PHASE2_INFO0_REPEAT:
	case PCM_PHASE2_INFO0_VERIFY_REPEAT:
		return ranging->control.phase != PHASE2_RANGING_INFO0_EXCHANGE;
	case PCM_PHASE2_INFO0_ACKNOWLEDGING:
	case PCM_PHASE2_INFO0_ACK_REPEAT:
	case PCM_PHASE2_INFO0_WAIT_COMPLETION:
	case PCM_PHASE2_INFO0_COMPLETE:
	case PCM_PHASE2_INFO0_RETRAIN:
		return true;
	}
	return false;
}

bool pcm_phase2_ranging_is_valid(
	const struct pcm_phase2_ranging *ranging)
{
	return ranging != NULL &&
	       pcm_phase2_info0_exchange_is_valid(
		       &ranging->info0_exchange) &&
	       phase2_ranging_control_is_valid(
		       &ranging->control,
		       &ranging->info0_exchange.transport,
		       ranging_context_is_retained(ranging));
}

static bool restart_after_repeated_info0(struct pcm_phase2_ranging *ranging)
{
	uint16_t prior_delay = ranging->control.prior_round_trip_delay_samples;

	/* V.90 9.2.1.2.1 and 9.2.2.2.1 restart the first-reversal exchange
	 * after repeated INFO0. Neither role may retain a reversal from before
	 * that interruption: Signal A must send a fresh initial tone and Signal
	 * B must answer its new first reversal before either can begin probing. */
	if (phase2_ranging_control_init(
		    &ranging->control,
		    &ranging->info0_exchange.transport) != PHASE2_RANGING_OK)
		return false;
	ranging->control.prior_round_trip_delay_samples = prior_delay;
	return true;
}

static enum pcm_phase2_ranging_result finish_initialization(
	struct pcm_phase2_ranging *ranging,
	struct pcm_phase2_ranging *initialized,
	enum pcm_phase2_info0_exchange_result exchange_result)
{
	if (ranging == NULL ||
	    exchange_result != PCM_PHASE2_INFO0_EXCHANGE_OK ||
	    phase2_ranging_control_init(
		    &initialized->control,
		    &initialized->info0_exchange.transport) !=
		    PHASE2_RANGING_OK ||
	    !pcm_phase2_ranging_is_valid(initialized))
		return PCM_PHASE2_RANGING_INVALID_ARGUMENT;
	*ranging = *initialized;
	return PCM_PHASE2_RANGING_OK;
}

enum pcm_phase2_ranging_result pcm_phase2_ranging_init_analog(
	struct pcm_phase2_ranging *ranging,
	int16_t nominal_amplitude,
	const struct v90_info0 *local_info0a)
{
	struct pcm_phase2_ranging initialized = { 0 };
	enum pcm_phase2_info0_exchange_result result;

	if (ranging == NULL)
		return PCM_PHASE2_RANGING_INVALID_ARGUMENT;
	result = pcm_phase2_info0_exchange_init_v90_analog(
		&initialized.info0_exchange, nominal_amplitude, local_info0a);
	return finish_initialization(ranging, &initialized, result);
}

enum pcm_phase2_ranging_result pcm_phase2_ranging_init_digital(
	struct pcm_phase2_ranging *ranging,
	int16_t nominal_amplitude,
	const struct v90_info0d *local_info0d)
{
	struct pcm_phase2_ranging initialized = { 0 };
	enum pcm_phase2_info0_exchange_result result;

	if (ranging == NULL)
		return PCM_PHASE2_RANGING_INVALID_ARGUMENT;
	result = pcm_phase2_info0_exchange_init_v90_digital(
		&initialized.info0_exchange, nominal_amplitude, local_info0d);
	return finish_initialization(ranging, &initialized, result);
}

enum pcm_phase2_ranging_result pcm_phase2_ranging_sample(
	struct pcm_phase2_ranging *ranging,
	int16_t received_sample, int16_t *transmit_sample,
	struct pcm_phase2_ranging_event *event)
{
	struct pcm_phase2_ranging next;
	struct pcm_phase2_info0_exchange_event exchange_event;
	struct phase2_ranging_event ranging_event;
	struct pcm_phase2_ranging_event emitted;
	enum phase2_info0_transmit_phase transmit_phase_before;
	int16_t generated;

	if (!pcm_phase2_ranging_is_valid(ranging) ||
	    transmit_sample == NULL || event == NULL)
		return PCM_PHASE2_RANGING_INVALID_ARGUMENT;
	next = *ranging;
	if (phase2_ranging_prepare_sample(
		    &next.control, &next.info0_exchange.transport,
		    ranging_context_is_retained(&next)) !=
	    PHASE2_RANGING_OK)
		return PCM_PHASE2_RANGING_INVALID_ARGUMENT;
	transmit_phase_before = next.info0_exchange.transport.transmit_phase;
	if (pcm_phase2_info0_exchange_sample(
		    &next.info0_exchange, received_sample, &generated,
		    &exchange_event) != PCM_PHASE2_INFO0_EXCHANGE_OK)
		return PCM_PHASE2_RANGING_INVALID_ARGUMENT;
	if (exchange_event.recovery_event == PCM_INFO0_RECOVERY_ACCEPTED &&
	    !restart_after_repeated_info0(&next))
		return PCM_PHASE2_RANGING_INVALID_ARGUMENT;
	if (phase2_ranging_finish_sample(
		    &next.control, &next.info0_exchange.transport,
		    ranging_context_is_retained(&next),
		    transmit_phase_before, &exchange_event.tone_event,
		    &generated, &ranging_event) != PHASE2_RANGING_OK)
		return PCM_PHASE2_RANGING_INVALID_ARGUMENT;
	if (!pcm_phase2_ranging_is_valid(&next))
		return PCM_PHASE2_RANGING_INVALID_ARGUMENT;
	emitted = (struct pcm_phase2_ranging_event){
		.exchange_event = exchange_event,
		.kind = ranging_event.kind,
		.round_trip_delay_samples =
			ranging_event.round_trip_delay_samples,
	};
	*ranging = next;
	*transmit_sample = generated;
	*event = emitted;
	return PCM_PHASE2_RANGING_OK;
}

const char *pcm_phase2_ranging_result_name(
	enum pcm_phase2_ranging_result result)
{
	switch (result) {
	case PCM_PHASE2_RANGING_OK:
		return "ok";
	case PCM_PHASE2_RANGING_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
