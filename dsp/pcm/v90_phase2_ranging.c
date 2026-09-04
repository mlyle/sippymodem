// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase2_ranging.h"

#include <stddef.h>

static bool control_is_clear(const struct phase2_ranging_control *control)
{
	return control->phase == PHASE2_RANGING_INFO0_EXCHANGE &&
	       control->local_tone_samples == 0 &&
	       control->deferred_reversal_samples == 0 &&
	       control->response_delay_samples == 0 &&
	       control->post_reversal_samples == 0 &&
	       control->probe_start_delay_samples == 0 &&
	       control->measurement_elapsed_samples == 0 &&
	       control->round_trip_delay_samples == 0 &&
	       !control->round_trip_delay_valid;
}

static bool control_is_empty(const struct phase2_ranging_control *control)
{
	return control_is_clear(control) &&
	       control->prior_round_trip_delay_samples == 0;
}

static bool control_has_signal_b_epoch(
	const struct phase2_ranging_control *control)
{
	return control->phase == PHASE2_RESPONSE_TONE_TAIL ||
	       control->phase == PHASE2_SIGNAL_B_WAIT_SECOND_REMOTE_REVERSAL ||
	       control->phase == PHASE2_SIGNAL_B_WAIT_PROBE_START ||
	       control->phase == PHASE2_RANGING_PROBE_READY;
}

static bool ranging_context_is_retained(
	const struct v90_phase2_ranging *ranging)
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

bool v90_phase2_ranging_is_valid(
	const struct v90_phase2_ranging *ranging)
{
	if (ranging == NULL ||
	    !pcm_phase2_info0_exchange_is_valid(
		    &ranging->info0_exchange) ||
	    !phase2_ranging_control_is_valid(
		    &ranging->control,
		    &ranging->info0_exchange.transport,
		    ranging_context_is_retained(ranging)) ||
	    ranging->recovery_elapsed_samples >
		    PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES)
		return false;
	if (!ranging->recovery_paused)
		return ranging->recovery_elapsed_samples == 0 &&
		       control_is_empty(&ranging->recovery_control);
	return ranging->info0_exchange.transport.signal_role == PHASE2_SIGNAL_B &&
	       control_is_clear(&ranging->control) &&
	       control_has_signal_b_epoch(&ranging->recovery_control) &&
	       phase2_ranging_control_is_valid(
		       &ranging->recovery_control,
		       &ranging->info0_exchange.transport, true) &&
	       ranging->info0_exchange.state ==
		       PCM_PHASE2_INFO0_VERIFY_REPEAT;
}

static bool pause_for_repeated_info0(struct v90_phase2_ranging *ranging)
{
	struct phase2_ranging_control retained = ranging->control;

	/* Signal A keeps its active reversal detector across an INFO0d repeat.
	 * Only Signal B has an established first-to-second-reversal epoch whose
	 * receive interval must be excluded while INFO0a owns the detector. */
	if (ranging->info0_exchange.transport.signal_role != PHASE2_SIGNAL_B)
		return true;
	if (phase2_ranging_control_init(
		    &ranging->control,
		    &ranging->info0_exchange.transport) != PHASE2_RANGING_OK)
		return false;
	ranging->control.prior_round_trip_delay_samples =
		retained.prior_round_trip_delay_samples;
	if (!control_has_signal_b_epoch(&retained))
		return true;
	ranging->recovery_control = retained;
	ranging->recovery_elapsed_samples = 0;
	ranging->recovery_paused = true;
	return true;
}

static void discard_recovery(struct v90_phase2_ranging *ranging)
{
	ranging->recovery_control = (struct phase2_ranging_control){ 0 };
	ranging->recovery_elapsed_samples = 0;
	ranging->recovery_paused = false;
}

static void count_recovery_sample(struct v90_phase2_ranging *ranging)
{
	if (!ranging->recovery_paused)
		return;
	if (ranging->recovery_elapsed_samples ==
	    PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES) {
		discard_recovery(ranging);
		return;
	}
	ranging->recovery_elapsed_samples++;
}

static void resume_after_repeated_info0(struct v90_phase2_ranging *ranging)
{
	struct phase2_ranging_control retained;
	uint32_t elapsed;

	if (!ranging->recovery_paused)
		return;
	retained = ranging->recovery_control;
	elapsed = (uint32_t)retained.measurement_elapsed_samples +
		ranging->recovery_elapsed_samples;
	if ((retained.phase == PHASE2_RESPONSE_TONE_TAIL ||
	     retained.phase == PHASE2_SIGNAL_B_WAIT_SECOND_REMOTE_REVERSAL) &&
	    elapsed > PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES) {
		discard_recovery(ranging);
		return;
	}
	if (retained.phase == PHASE2_RESPONSE_TONE_TAIL ||
	    retained.phase == PHASE2_SIGNAL_B_WAIT_SECOND_REMOTE_REVERSAL)
		retained.measurement_elapsed_samples = (uint16_t)elapsed;
	/* A repeated INFO0 record interrupts the physical Tone-B tail. Resume
	 * with the complete required 10 ms, while retaining its elapsed ranging
	 * epoch. */
	if (retained.phase == PHASE2_RESPONSE_TONE_TAIL)
		retained.post_reversal_samples = 0;
	ranging->control = retained;
	discard_recovery(ranging);
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
	struct v90_phase2_ranging initialized = { 0 };
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
	struct v90_phase2_ranging initialized = { 0 };
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
		    ranging_context_is_retained(&next)) !=
	    PHASE2_RANGING_OK)
		return V90_PHASE2_RANGING_INVALID_ARGUMENT;
	transmit_phase_before = next.info0_exchange.transport.transmit_phase;
	if (pcm_phase2_info0_exchange_sample(
		    &next.info0_exchange, received_sample, &generated,
		    &exchange_event) != PCM_PHASE2_INFO0_EXCHANGE_OK)
		return V90_PHASE2_RANGING_INVALID_ARGUMENT;
	if (exchange_event.recovery_event == PCM_INFO0_RECOVERY_ACCEPTED) {
		resume_after_repeated_info0(&next);
	} else if (exchange_event.recovery_event ==
		   PCM_INFO0_RECOVERY_REJECTED)
		discard_recovery(&next);
	if (phase2_ranging_finish_sample(
		    &next.control, &next.info0_exchange.transport,
		    ranging_context_is_retained(&next),
		    transmit_phase_before, &exchange_event.tone_event,
		    &generated, &ranging_event) != PHASE2_RANGING_OK ||
	    (exchange_event.recovery_event == PCM_INFO0_RECOVERY_STARTED &&
	     !pause_for_repeated_info0(&next)))
		return V90_PHASE2_RANGING_INVALID_ARGUMENT;
	if (exchange_event.recovery_event == PCM_INFO0_RECOVERY_NO_EVENT)
		count_recovery_sample(&next);
	if (!v90_phase2_ranging_is_valid(&next))
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
