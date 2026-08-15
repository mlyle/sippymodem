// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_phase2.h"

#include <stddef.h>

static bool failure_is_valid(enum pcm_phase2_failure failure)
{
	return failure >= PCM_PHASE2_FAILURE_NONE &&
	       failure <= PCM_PHASE2_FAILURE_INTERNAL;
}

static bool local_info0_is_valid(const struct pcm_phase2 *phase2)
{
	if (phase2->info0_protocol == PCM_PROTOCOL_V92) {
		if (phase2->local_role == V90_ANALOG_MODEM)
			return v92_info0a_is_valid(
				       &phase2->local_info0.v92_analog) &&
			       phase2->local_info0.v92_analog.v92_capable &&
			       !phase2->local_info0.v92_analog
					.short_phase2_requested &&
			       !phase2->local_info0.v92_analog.v90
					.acknowledges_peer_info0;
		return v92_info0d_is_valid(&phase2->local_info0.v92_digital) &&
		       phase2->local_info0.v92_digital.v92_capable &&
		       !phase2->local_info0.v92_digital.short_phase2_requested &&
		       !phase2->local_info0.v92_digital.v90.common
				.acknowledges_peer_info0;
	}
	if (phase2->local_role == V90_ANALOG_MODEM)
		return v90_info0_is_valid(&phase2->local_info0.analog) &&
		       !phase2->local_info0.analog.acknowledges_peer_info0;
	return v90_info0d_is_valid(&phase2->local_info0.digital) &&
	       !phase2->local_info0.digital.common.acknowledges_peer_info0;
}

static bool peer_info0_is_valid(const struct pcm_phase2 *phase2)
{
	if (!phase2->peer_info0_stored)
		return true;
	if (phase2->info0_protocol == PCM_PROTOCOL_V92)
		return phase2->local_role == V90_ANALOG_MODEM ?
			       v92_info0d_is_valid(
				       &phase2->peer_info0.v92_digital) :
			       v92_info0a_is_valid(
				       &phase2->peer_info0.v92_analog);
	return phase2->local_role == V90_ANALOG_MODEM ?
		       v90_info0d_is_valid(&phase2->peer_info0.digital) :
		       v90_info0_is_valid(&phase2->peer_info0.analog);
}

static bool derive_startup(
	const struct pcm_phase2 *phase2,
	struct pcm_startup_selection *selection)
{
	if (phase2->info0_protocol == PCM_PROTOCOL_V90) {
		*selection = (struct pcm_startup_selection){
			.protocol = PCM_PROTOCOL_V90,
			.phase2 = PCM_PHASE2_FULL,
		};
		return true;
	}
	{
		const struct v92_info0a *analog =
			phase2->local_role == V90_ANALOG_MODEM ?
				&phase2->local_info0.v92_analog :
				&phase2->peer_info0.v92_analog;
		const struct v92_info0d *digital =
			phase2->local_role == V90_DIGITAL_MODEM ?
				&phase2->local_info0.v92_digital :
				&phase2->peer_info0.v92_digital;

		return pcm_startup_select(analog, digital, selection) ==
			       PCM_STARTUP_OK &&
		       selection->phase2 == PCM_PHASE2_FULL;
	}
}

static bool startup_is_valid(const struct pcm_phase2 *phase2)
{
	struct pcm_startup_selection expected;

	if (!phase2->peer_info0_stored)
		return phase2->stage == PCM_PHASE2_RANGING;
	return derive_startup(phase2, &expected) &&
	       phase2->startup.protocol == expected.protocol &&
	       phase2->startup.phase2 == expected.phase2;
}

static bool remote_probe_state_is_valid(const struct pcm_phase2 *phase2)
{
	if (phase2->remote_probe_stored &&
	    !v34_phase2_probe_measurement_is_valid(&phase2->remote_probe))
		return false;
	if (phase2->stage == PCM_PHASE2_RANGING)
		return !phase2->remote_probe_stored;
	if (phase2->stage == PCM_PHASE2_INFO1)
		return phase2->remote_probe_stored;
	if (phase2->local_role == V90_ANALOG_MODEM)
		return phase2->stage == PCM_PHASE2_RECIPROCAL_PROBE ||
		       !phase2->remote_probe_stored;
	return phase2->stage == PCM_PHASE2_INITIAL_PROBE ||
	       phase2->remote_probe_stored;
}

static enum v90_modem_role child_role(const struct pcm_phase2 *phase2)
{
	switch (phase2->stage) {
	case PCM_PHASE2_RANGING:
		return phase2->state.ranging.info0_exchange.local_role;
	case PCM_PHASE2_INITIAL_PROBE:
		return phase2->state.initial_probe.ranging.info0_exchange
			.local_role;
	case PCM_PHASE2_PROBE_TURNAROUND:
		return phase2->state.turnaround.initial_probe.ranging
			.info0_exchange.local_role;
	case PCM_PHASE2_RECIPROCAL_PROBE:
		return phase2->state.reciprocal_probe.turnaround.initial_probe
			.ranging.info0_exchange.local_role;
	case PCM_PHASE2_INFO1:
		return phase2->state.info1.control.signal_role ==
			       PHASE2_SIGNAL_A ?
			       V90_ANALOG_MODEM : V90_DIGITAL_MODEM;
	}
	return (enum v90_modem_role)-1;
}

static enum pcm_protocol child_protocol(const struct pcm_phase2 *phase2)
{
	switch (phase2->stage) {
	case PCM_PHASE2_RANGING:
		return phase2->state.ranging.info0_exchange.protocol;
	case PCM_PHASE2_INITIAL_PROBE:
		return phase2->state.initial_probe.ranging.info0_exchange
			.protocol;
	case PCM_PHASE2_PROBE_TURNAROUND:
		return phase2->state.turnaround.initial_probe.ranging
			.info0_exchange.protocol;
	case PCM_PHASE2_RECIPROCAL_PROBE:
		return phase2->state.reciprocal_probe.turnaround.initial_probe
			.ranging.info0_exchange.protocol;
	case PCM_PHASE2_INFO1:
		return phase2->state.info1.protocol;
	}
	return (enum pcm_protocol)-1;
}

static enum pcm_phase2_failure initial_probe_failure(
	enum phase2_initial_probe_failure failure)
{
	switch (failure) {
	case PHASE2_INITIAL_PROBE_NO_FAILURE:
		return PCM_PHASE2_FAILURE_NONE;
	case PHASE2_INITIAL_PROBE_REMOTE_NOT_FOUND:
		return PCM_PHASE2_FAILURE_REMOTE_PROBE_NOT_FOUND;
	case PHASE2_INITIAL_PROBE_LOCAL_TRANSMIT_LIMIT:
		return PCM_PHASE2_FAILURE_LOCAL_PROBE_LIMIT;
	case PHASE2_INITIAL_PROBE_REMOTE_TONE_TIMEOUT:
		return PCM_PHASE2_FAILURE_REMOTE_TONE_TIMEOUT;
	}
	return PCM_PHASE2_FAILURE_INTERNAL;
}

static enum pcm_phase2_failure active_failure(
	const struct pcm_phase2 *phase2)
{
	switch (phase2->stage) {
	case PCM_PHASE2_RANGING:
		return phase2->state.ranging.control.phase ==
			       PHASE2_RANGING_TIMED_OUT ?
			       PCM_PHASE2_FAILURE_RANGING_TIMEOUT :
			       PCM_PHASE2_FAILURE_NONE;
	case PCM_PHASE2_INITIAL_PROBE:
		return initial_probe_failure(
			phase2->state.initial_probe.control.failure);
	case PCM_PHASE2_PROBE_TURNAROUND:
		return phase2->state.turnaround.control.failure ==
			       PHASE2_PROBE_TURNAROUND_REMOTE_REVERSAL_TIMEOUT ?
			       PCM_PHASE2_FAILURE_REMOTE_REVERSAL_TIMEOUT :
			       PCM_PHASE2_FAILURE_NONE;
	case PCM_PHASE2_RECIPROCAL_PROBE:
		switch (phase2->state.reciprocal_probe.control.failure) {
		case PHASE2_RECIPROCAL_PROBE_NO_FAILURE:
			return PCM_PHASE2_FAILURE_NONE;
		case PHASE2_RECIPROCAL_PROBE_REMOTE_NOT_FOUND:
			return PCM_PHASE2_FAILURE_REMOTE_PROBE_NOT_FOUND;
		case PHASE2_RECIPROCAL_PROBE_LOCAL_TRANSMIT_LIMIT:
			return PCM_PHASE2_FAILURE_LOCAL_PROBE_LIMIT;
		}
		break;
	case PCM_PHASE2_INFO1:
		return phase2->state.info1.control.failure ==
			       PHASE2_INFO1_PEER_TIMEOUT ?
			       PCM_PHASE2_FAILURE_INFO1_TIMEOUT :
			       PCM_PHASE2_FAILURE_NONE;
	}
	return PCM_PHASE2_FAILURE_INTERNAL;
}

bool pcm_phase2_is_valid(const struct pcm_phase2 *phase2)
{
	enum pcm_phase2_failure child_failure;
	bool child_valid;

	if (phase2 == NULL ||
	    !pcm_modem_role_is_valid(phase2->local_role) ||
	    !pcm_protocol_is_valid(phase2->info0_protocol) ||
	    !failure_is_valid(phase2->failure) ||
	    !local_info0_is_valid(phase2) || !peer_info0_is_valid(phase2) ||
	    !startup_is_valid(phase2) || !remote_probe_state_is_valid(phase2) ||
	    (phase2->stage != PCM_PHASE2_RANGING &&
	     !phase2->peer_info0_stored))
		return false;
	switch (phase2->stage) {
	case PCM_PHASE2_RANGING:
		child_valid = v90_phase2_ranging_is_valid(
			&phase2->state.ranging);
		break;
	case PCM_PHASE2_INITIAL_PROBE:
		child_valid = v90_phase2_initial_probe_is_valid(
			&phase2->state.initial_probe);
		break;
	case PCM_PHASE2_PROBE_TURNAROUND:
		child_valid = v90_phase2_probe_turnaround_is_valid(
			&phase2->state.turnaround);
		break;
	case PCM_PHASE2_RECIPROCAL_PROBE:
		child_valid = v90_phase2_reciprocal_probe_is_valid(
			&phase2->state.reciprocal_probe);
		break;
	case PCM_PHASE2_INFO1:
		child_valid = phase2->peer_info0_stored &&
			      phase2->remote_probe_stored &&
			      phase2->state.info1.protocol ==
				      phase2->startup.protocol &&
			      pcm_phase2_info1_is_valid(&phase2->state.info1);
		break;
	default:
		return false;
	}
	if (!child_valid || child_role(phase2) != phase2->local_role ||
	    child_protocol(phase2) !=
		    (phase2->stage == PCM_PHASE2_INFO1 ?
			    phase2->startup.protocol : phase2->info0_protocol))
		return false;
	child_failure = active_failure(phase2);
	return phase2->failure == PCM_PHASE2_FAILURE_INTERNAL ?
		       child_failure == PCM_PHASE2_FAILURE_NONE :
		       phase2->failure == child_failure;
}

bool pcm_phase2_has_failed(const struct pcm_phase2 *phase2)
{
	return pcm_phase2_is_valid(phase2) &&
	       phase2->failure != PCM_PHASE2_FAILURE_NONE;
}

static bool ready_for_info1(const struct pcm_phase2 *phase2)
{
	if (phase2->failure != PCM_PHASE2_FAILURE_NONE ||
	    phase2->stage != PCM_PHASE2_RECIPROCAL_PROBE ||
	    !phase2->remote_probe_stored)
		return false;
	return phase2->local_role == V90_DIGITAL_MODEM ?
		       phase2->state.reciprocal_probe.control.phase ==
			       PHASE2_RECIPROCAL_PROBE_SIGNAL_B_COMPLETE :
		       phase2->state.reciprocal_probe.control.phase ==
			       PHASE2_RECIPROCAL_PROBE_SIGNAL_A_TONE;
}

bool pcm_phase2_is_ready_for_info1(const struct pcm_phase2 *phase2)
{
	return pcm_phase2_is_valid(phase2) && ready_for_info1(phase2);
}

static bool ready_for_phase3(const struct pcm_phase2 *phase2)
{
	return phase2->failure == PCM_PHASE2_FAILURE_NONE &&
	       phase2->stage == PCM_PHASE2_INFO1 &&
	       phase2->state.info1.control.phase == PHASE2_INFO1_COMPLETE;
}

bool pcm_phase2_is_ready_for_phase3(const struct pcm_phase2 *phase2)
{
	return pcm_phase2_is_valid(phase2) && ready_for_phase3(phase2);
}

static enum pcm_phase2_result initialize(
	struct pcm_phase2 *phase2, const union pcm_info0_record *local_info0,
	const struct pcm_phase2_info0_exchange *exchange)
{
	struct pcm_phase2 initialized = {
		.state.ranging.info0_exchange = *exchange,
		.local_info0 = *local_info0,
		.stage = PCM_PHASE2_RANGING,
		.local_role = exchange->local_role,
		.info0_protocol = exchange->protocol,
	};

	if (phase2_ranging_control_init(
		    &initialized.state.ranging.control,
		    &initialized.state.ranging.info0_exchange.transport) !=
		    PHASE2_RANGING_OK ||
	    !pcm_phase2_is_valid(&initialized))
		return PCM_PHASE2_INVALID_ARGUMENT;
	*phase2 = initialized;
	return PCM_PHASE2_OK;
}

enum pcm_phase2_result pcm_phase2_init_v90_analog(
	struct pcm_phase2 *phase2, int16_t nominal_amplitude,
	const struct v90_info0 *local_info0a)
{
	struct pcm_phase2_info0_exchange exchange;
	union pcm_info0_record local = { 0 };

	if (phase2 == NULL || !v90_info0_is_valid(local_info0a) ||
	    pcm_phase2_info0_exchange_init_v90_analog(
		    &exchange, nominal_amplitude, local_info0a) !=
		    PCM_PHASE2_INFO0_EXCHANGE_OK)
		return PCM_PHASE2_INVALID_ARGUMENT;
	local.analog = *local_info0a;
	local.analog.acknowledges_peer_info0 = false;
	return initialize(phase2, &local, &exchange);
}

enum pcm_phase2_result pcm_phase2_init_v90_digital(
	struct pcm_phase2 *phase2, int16_t nominal_amplitude,
	const struct v90_info0d *local_info0d)
{
	struct pcm_phase2_info0_exchange exchange;
	union pcm_info0_record local = { 0 };

	if (phase2 == NULL || !v90_info0d_is_valid(local_info0d) ||
	    pcm_phase2_info0_exchange_init_v90_digital(
		    &exchange, nominal_amplitude, local_info0d) !=
		    PCM_PHASE2_INFO0_EXCHANGE_OK)
		return PCM_PHASE2_INVALID_ARGUMENT;
	local.digital = *local_info0d;
	local.digital.common.acknowledges_peer_info0 = false;
	return initialize(phase2, &local, &exchange);
}

enum pcm_phase2_result pcm_phase2_init_v92_analog(
	struct pcm_phase2 *phase2, int16_t nominal_amplitude,
	const struct v92_info0a *local_info0a)
{
	struct pcm_phase2_info0_exchange exchange;
	union pcm_info0_record local = { 0 };

	if (phase2 == NULL || !v92_info0a_is_valid(local_info0a) ||
	    local_info0a->short_phase2_requested ||
	    pcm_phase2_info0_exchange_init_v92_analog(
		    &exchange, nominal_amplitude, local_info0a) !=
		    PCM_PHASE2_INFO0_EXCHANGE_OK)
		return PCM_PHASE2_INVALID_ARGUMENT;
	local.v92_analog = *local_info0a;
	local.v92_analog.v90.acknowledges_peer_info0 = false;
	return initialize(phase2, &local, &exchange);
}

enum pcm_phase2_result pcm_phase2_init_v92_digital(
	struct pcm_phase2 *phase2, int16_t nominal_amplitude,
	const struct v92_info0d *local_info0d)
{
	struct pcm_phase2_info0_exchange exchange;
	union pcm_info0_record local = { 0 };

	if (phase2 == NULL || !v92_info0d_is_valid(local_info0d) ||
	    local_info0d->short_phase2_requested ||
	    pcm_phase2_info0_exchange_init_v92_digital(
		    &exchange, nominal_amplitude, local_info0d) !=
		    PCM_PHASE2_INFO0_EXCHANGE_OK)
		return PCM_PHASE2_INVALID_ARGUMENT;
	local.v92_digital = *local_info0d;
	local.v92_digital.v90.common.acknowledges_peer_info0 = false;
	return initialize(phase2, &local, &exchange);
}

enum pcm_phase2_result pcm_phase2_peer_info0a(
	const struct pcm_phase2 *phase2, struct v90_info0 *peer_info0a)
{
	if (!pcm_phase2_is_valid(phase2) || peer_info0a == NULL)
		return PCM_PHASE2_INVALID_ARGUMENT;
	if (phase2->local_role != V90_DIGITAL_MODEM ||
	    !phase2->peer_info0_stored)
		return PCM_PHASE2_INVALID_STATE;
	*peer_info0a = phase2->info0_protocol == PCM_PROTOCOL_V90 ?
			       phase2->peer_info0.analog :
			       phase2->peer_info0.v92_analog.v90;
	return PCM_PHASE2_OK;
}

enum pcm_phase2_result pcm_phase2_peer_info0d(
	const struct pcm_phase2 *phase2, struct v90_info0d *peer_info0d)
{
	if (!pcm_phase2_is_valid(phase2) || peer_info0d == NULL)
		return PCM_PHASE2_INVALID_ARGUMENT;
	if (phase2->local_role != V90_ANALOG_MODEM ||
	    !phase2->peer_info0_stored)
		return PCM_PHASE2_INVALID_STATE;
	*peer_info0d = phase2->info0_protocol == PCM_PROTOCOL_V90 ?
			       phase2->peer_info0.digital :
			       phase2->peer_info0.v92_digital.v90;
	return PCM_PHASE2_OK;
}

enum pcm_phase2_result pcm_phase2_peer_v92_info0a(
	const struct pcm_phase2 *phase2, struct v92_info0a *peer_info0a)
{
	if (!pcm_phase2_is_valid(phase2) || peer_info0a == NULL)
		return PCM_PHASE2_INVALID_ARGUMENT;
	if (phase2->info0_protocol != PCM_PROTOCOL_V92 ||
	    phase2->local_role != V90_DIGITAL_MODEM ||
	    !phase2->peer_info0_stored)
		return PCM_PHASE2_INVALID_STATE;
	*peer_info0a = phase2->peer_info0.v92_analog;
	return PCM_PHASE2_OK;
}

enum pcm_phase2_result pcm_phase2_peer_v92_info0d(
	const struct pcm_phase2 *phase2, struct v92_info0d *peer_info0d)
{
	if (!pcm_phase2_is_valid(phase2) || peer_info0d == NULL)
		return PCM_PHASE2_INVALID_ARGUMENT;
	if (phase2->info0_protocol != PCM_PROTOCOL_V92 ||
	    phase2->local_role != V90_ANALOG_MODEM ||
	    !phase2->peer_info0_stored)
		return PCM_PHASE2_INVALID_STATE;
	*peer_info0d = phase2->peer_info0.v92_digital;
	return PCM_PHASE2_OK;
}

enum pcm_phase2_result pcm_phase2_startup_selection(
	const struct pcm_phase2 *phase2,
	struct pcm_startup_selection *selection)
{
	if (!pcm_phase2_is_valid(phase2) || selection == NULL)
		return PCM_PHASE2_INVALID_ARGUMENT;
	if (!phase2->peer_info0_stored)
		return PCM_PHASE2_INVALID_STATE;
	*selection = phase2->startup;
	return PCM_PHASE2_OK;
}

static enum pcm_phase2_result enter_info1(
	struct pcm_phase2 *phase2,
	const struct pcm_phase2_info1_exchange *exchange)
{
	struct pcm_phase2 next;

	next = *phase2;
	next.state.info1 = *exchange;
	next.stage = PCM_PHASE2_INFO1;
	if (!pcm_phase2_is_valid(&next))
		return PCM_PHASE2_INVALID_STATE;
	*phase2 = next;
	return PCM_PHASE2_OK;
}

enum pcm_phase2_result pcm_phase2_start_v90_digital_info1(
	struct pcm_phase2 *phase2, const struct v34_info1c *local_info1d)
{
	struct pcm_phase2_info1_exchange exchange;

	if (!pcm_phase2_is_valid(phase2) ||
	    !v34_info1c_is_valid(local_info1d))
		return PCM_PHASE2_INVALID_ARGUMENT;
	if (phase2->startup.protocol != PCM_PROTOCOL_V90 ||
	    phase2->local_role != V90_DIGITAL_MODEM ||
	    !ready_for_info1(phase2))
		return PCM_PHASE2_INVALID_STATE;
	if (pcm_phase2_info1_init_v90_digital(
		    &exchange, &phase2->state.reciprocal_probe,
		    local_info1d) != PHASE2_INFO1_OK)
		return PCM_PHASE2_INVALID_STATE;
	return enter_info1(phase2, &exchange);
}

enum pcm_phase2_result pcm_phase2_start_v92_digital_info1(
	struct pcm_phase2 *phase2, const struct v92_info1d *local_info1d)
{
	struct pcm_phase2_info1_exchange exchange;

	if (!pcm_phase2_is_valid(phase2) ||
	    !v92_info1d_is_valid(local_info1d))
		return PCM_PHASE2_INVALID_ARGUMENT;
	if (phase2->startup.protocol != PCM_PROTOCOL_V92 ||
	    phase2->local_role != V90_DIGITAL_MODEM ||
	    !ready_for_info1(phase2))
		return PCM_PHASE2_INVALID_STATE;
	if (pcm_phase2_info1_init_v92_digital(
		    &exchange, &phase2->state.reciprocal_probe,
		    local_info1d) != PHASE2_INFO1_OK)
		return PCM_PHASE2_INVALID_STATE;
	return enter_info1(phase2, &exchange);
}

enum pcm_phase2_result pcm_phase2_start_analog_info1(
	struct pcm_phase2 *phase2)
{
	struct pcm_phase2_info1_exchange exchange;

	if (!pcm_phase2_is_valid(phase2))
		return PCM_PHASE2_INVALID_ARGUMENT;
	if (phase2->local_role != V90_ANALOG_MODEM ||
	    !ready_for_info1(phase2))
		return PCM_PHASE2_INVALID_STATE;
	if ((phase2->startup.protocol == PCM_PROTOCOL_V90 ?
		     pcm_phase2_info1_init_v90_analog(
			     &exchange,
			     &phase2->state.reciprocal_probe) :
		     pcm_phase2_info1_init_v92_analog(
			     &exchange,
			     &phase2->state.reciprocal_probe)) !=
	    PHASE2_INFO1_OK)
		return PCM_PHASE2_INVALID_STATE;
	return enter_info1(phase2, &exchange);
}

enum pcm_phase2_result pcm_phase2_reply_conventional(
	struct pcm_phase2 *phase2, const struct v90_info1a *local_info1a)
{
	struct pcm_phase2 next;

	if (!pcm_phase2_is_valid(phase2) ||
	    !v90_info1a_is_valid(local_info1a))
		return PCM_PHASE2_INVALID_ARGUMENT;
	if (phase2->failure != PCM_PHASE2_FAILURE_NONE ||
	    phase2->stage != PCM_PHASE2_INFO1 ||
	    phase2->local_role != V90_ANALOG_MODEM ||
	    phase2->state.info1.control.phase !=
		    PHASE2_INFO1_SIGNAL_A_REPLY_NEEDED)
		return PCM_PHASE2_INVALID_STATE;
	next = *phase2;
	if (pcm_phase2_info1_reply_conventional(
		    &next.state.info1, local_info1a) != PHASE2_INFO1_OK)
		return PCM_PHASE2_INVALID_STATE;
	if (!pcm_phase2_is_valid(&next))
		return PCM_PHASE2_INVALID_STATE;
	*phase2 = next;
	return PCM_PHASE2_OK;
}

enum pcm_phase2_result pcm_phase2_reply_pcm(
	struct pcm_phase2 *phase2, const struct v92_pcm_info1a *local_info1a)
{
	struct pcm_phase2 next;

	if (!pcm_phase2_is_valid(phase2) ||
	    !v92_pcm_info1a_is_valid(local_info1a))
		return PCM_PHASE2_INVALID_ARGUMENT;
	if (phase2->startup.protocol != PCM_PROTOCOL_V92 ||
	    phase2->failure != PCM_PHASE2_FAILURE_NONE ||
	    phase2->stage != PCM_PHASE2_INFO1 ||
	    phase2->local_role != V90_ANALOG_MODEM ||
	    phase2->state.info1.control.phase !=
		    PHASE2_INFO1_SIGNAL_A_REPLY_NEEDED)
		return PCM_PHASE2_INVALID_STATE;
	next = *phase2;
	if (pcm_phase2_info1_reply_pcm(
		    &next.state.info1, local_info1a) != PHASE2_INFO1_OK ||
	    !pcm_phase2_is_valid(&next))
		return PCM_PHASE2_INVALID_STATE;
	*phase2 = next;
	return PCM_PHASE2_OK;
}

enum pcm_phase2_result pcm_phase2_peer_info1d(
	const struct pcm_phase2 *phase2, struct v34_info1c *peer_info1d)
{
	if (!pcm_phase2_is_valid(phase2) || peer_info1d == NULL)
		return PCM_PHASE2_INVALID_ARGUMENT;
	if (phase2->failure != PCM_PHASE2_FAILURE_NONE ||
	    phase2->stage != PCM_PHASE2_INFO1 ||
	    phase2->local_role != V90_ANALOG_MODEM ||
	    !phase2->state.info1.digital_record_stored)
		return PCM_PHASE2_INVALID_STATE;
	*peer_info1d = phase2->state.info1.protocol == PCM_PROTOCOL_V90 ?
			       phase2->state.info1.digital.record.v90 :
			       phase2->state.info1.digital.record.v92.v34;
	return PCM_PHASE2_OK;
}

enum pcm_phase2_result pcm_phase2_peer_info1a(
	const struct pcm_phase2 *phase2, struct v90_info1a *peer_info1a)
{
	if (!pcm_phase2_is_valid(phase2) || peer_info1a == NULL)
		return PCM_PHASE2_INVALID_ARGUMENT;
	if (phase2->failure != PCM_PHASE2_FAILURE_NONE ||
	    phase2->stage != PCM_PHASE2_INFO1 ||
	    phase2->local_role != V90_DIGITAL_MODEM ||
	    !phase2->state.info1.analog_record_stored ||
	    phase2->state.info1.analog.upstream != PCM_UPSTREAM_V34)
		return PCM_PHASE2_INVALID_STATE;
	*peer_info1a = phase2->state.info1.analog.record.conventional;
	return PCM_PHASE2_OK;
}

enum pcm_phase2_result pcm_phase2_peer_v92_info1d(
	const struct pcm_phase2 *phase2, struct v92_info1d *peer_info1d)
{
	if (!pcm_phase2_is_valid(phase2) || peer_info1d == NULL)
		return PCM_PHASE2_INVALID_ARGUMENT;
	if (phase2->failure != PCM_PHASE2_FAILURE_NONE ||
	    phase2->stage != PCM_PHASE2_INFO1 ||
	    phase2->local_role != V90_ANALOG_MODEM ||
	    phase2->state.info1.protocol != PCM_PROTOCOL_V92 ||
	    !phase2->state.info1.digital_record_stored)
		return PCM_PHASE2_INVALID_STATE;
	*peer_info1d = phase2->state.info1.digital.record.v92;
	return PCM_PHASE2_OK;
}

enum pcm_phase2_result pcm_phase2_peer_pcm_info1a(
	const struct pcm_phase2 *phase2, struct v92_pcm_info1a *peer_info1a)
{
	if (!pcm_phase2_is_valid(phase2) || peer_info1a == NULL)
		return PCM_PHASE2_INVALID_ARGUMENT;
	if (phase2->failure != PCM_PHASE2_FAILURE_NONE ||
	    phase2->stage != PCM_PHASE2_INFO1 ||
	    phase2->local_role != V90_DIGITAL_MODEM ||
	    !phase2->state.info1.analog_record_stored ||
	    phase2->state.info1.analog.upstream != PCM_UPSTREAM_PCM)
		return PCM_PHASE2_INVALID_STATE;
	*peer_info1a = phase2->state.info1.analog.record.pcm;
	return PCM_PHASE2_OK;
}

static void fail_internal(
	struct pcm_phase2 *phase2, struct pcm_phase2_event *event)
{
	phase2->failure = PCM_PHASE2_FAILURE_INTERNAL;
	event->kind = PCM_PHASE2_EVENT_FAILED;
	event->failure = phase2->failure;
}

static bool select_startup(struct pcm_phase2 *phase2)
{
	struct pcm_startup_selection selected;

	if (!derive_startup(phase2, &selected))
		return false;
	phase2->startup = selected;
	return true;
}

static bool enter_initial_probe(
	struct pcm_phase2 *phase2, struct pcm_phase2_event *event)
{
	const struct pcm_phase2_info0_exchange *exchange =
		&phase2->state.ranging.info0_exchange;
	struct v90_phase2_initial_probe next;

	if (!exchange->peer_info0_received ||
	    v90_phase2_initial_probe_init(
		    &next, &phase2->state.ranging) !=
		    PHASE2_INITIAL_PROBE_OK)
		return false;
	phase2->peer_info0 = exchange->peer_info0;
	phase2->peer_info0_stored = true;
	phase2->state.initial_probe = next;
	phase2->stage = PCM_PHASE2_INITIAL_PROBE;
	event->kind = PCM_PHASE2_EVENT_INITIAL_PROBE_STARTED;
	return true;
}

static bool enter_turnaround(
	struct pcm_phase2 *phase2, struct pcm_phase2_event *event)
{
	struct v90_phase2_probe_turnaround next;

	if (v90_phase2_probe_turnaround_init(
		    &next, &phase2->state.initial_probe) !=
	    PHASE2_PROBE_TURNAROUND_OK)
		return false;
	phase2->state.turnaround = next;
	phase2->stage = PCM_PHASE2_PROBE_TURNAROUND;
	event->kind = PCM_PHASE2_EVENT_PROBE_TURNAROUND_STARTED;
	return true;
}

static bool enter_reciprocal_probe(
	struct pcm_phase2 *phase2, struct pcm_phase2_event *event)
{
	struct v90_phase2_reciprocal_probe next;

	if (v90_phase2_reciprocal_probe_init(
		    &next, &phase2->state.turnaround) !=
	    PHASE2_RECIPROCAL_PROBE_OK)
		return false;
	phase2->state.reciprocal_probe = next;
	phase2->stage = PCM_PHASE2_RECIPROCAL_PROBE;
	event->kind = PCM_PHASE2_EVENT_RECIPROCAL_PROBE_STARTED;
	return true;
}

static bool sample_ranging(
	struct pcm_phase2 *phase2, int16_t received_sample,
	int16_t *transmit_sample, struct pcm_phase2_event *event)
{
	struct v90_phase2_ranging_event child;
	enum phase2_ranging_event_kind ready =
		phase2->local_role == V90_ANALOG_MODEM ?
			PHASE2_PROBE_TRANSMIT_READY :
			PHASE2_PROBE_RECEIVE_READY;

	if (v90_phase2_ranging_sample(
		    &phase2->state.ranging, received_sample,
		    transmit_sample, &child) != V90_PHASE2_RANGING_OK)
		return false;
	if (child.exchange_event.peer_info0_event == PCM_PEER_INFO0_RECEIVED) {
		phase2->peer_info0 = child.exchange_event.peer_info0;
		phase2->peer_info0_stored = true;
		if (!select_startup(phase2))
			return false;
	}
	switch (child.kind) {
	case PHASE2_RANGING_NO_EVENT:
		return true;
	case PHASE2_ROUND_TRIP_DELAY_MEASURED:
		event->kind = PCM_PHASE2_EVENT_ROUND_TRIP_DELAY;
		event->round_trip_delay_samples =
			child.round_trip_delay_samples;
		return true;
	case PHASE2_PROBE_RECEIVE_READY:
	case PHASE2_PROBE_TRANSMIT_READY:
		return child.kind == ready && enter_initial_probe(phase2, event);
	case PHASE2_RANGING_TIMEOUT:
		phase2->failure = PCM_PHASE2_FAILURE_RANGING_TIMEOUT;
		event->kind = PCM_PHASE2_EVENT_FAILED;
		event->failure = phase2->failure;
		return true;
	}
	return false;
}

static bool sample_initial_probe(
	struct pcm_phase2 *phase2, int16_t received_sample,
	int16_t *transmit_sample, struct pcm_phase2_event *event)
{
	struct phase2_initial_probe_event child;
	enum phase2_initial_probe_event_kind ready =
		phase2->local_role == V90_ANALOG_MODEM ?
			PHASE2_INITIAL_PROBE_EVENT_LOCAL_PROBE_COMPLETE :
			PHASE2_INITIAL_PROBE_EVENT_TONE_PAIR_ACTIVE;

	if (v90_phase2_initial_probe_sample(
		    &phase2->state.initial_probe, received_sample,
		    transmit_sample, &child) != PHASE2_INITIAL_PROBE_OK)
		return false;
	switch (child.kind) {
	case PHASE2_INITIAL_PROBE_EVENT_NONE:
		return true;
	case PHASE2_INITIAL_PROBE_EVENT_REMOTE_PROBE_COMPLETE:
		if (phase2->local_role != V90_DIGITAL_MODEM ||
		    v90_phase2_initial_probe_measure(
			    &phase2->state.initial_probe,
			    &phase2->remote_probe) != PHASE2_INITIAL_PROBE_OK ||
		    !v34_phase2_probe_measurement_is_valid(
			    &phase2->remote_probe))
			return false;
		phase2->remote_probe_stored = true;
		event->kind = PCM_PHASE2_EVENT_REMOTE_INITIAL_PROBE_MEASURED;
		return true;
	case PHASE2_INITIAL_PROBE_EVENT_LOCAL_PROBE_COMPLETE:
	case PHASE2_INITIAL_PROBE_EVENT_TONE_PAIR_ACTIVE:
		return child.kind == ready && enter_turnaround(phase2, event);
	case PHASE2_INITIAL_PROBE_EVENT_FAILED:
		phase2->failure = initial_probe_failure(child.failure);
		event->kind = PCM_PHASE2_EVENT_FAILED;
		event->failure = phase2->failure;
		return true;
	}
	return false;
}

static bool sample_turnaround(
	struct pcm_phase2 *phase2, int16_t received_sample,
	int16_t *transmit_sample, struct pcm_phase2_event *event)
{
	struct phase2_probe_turnaround_event child;
	enum phase2_probe_turnaround_event_kind ready =
		phase2->local_role == V90_ANALOG_MODEM ?
			PHASE2_PROBE_TURNAROUND_EVENT_PROBE_RECEIVE_READY :
			PHASE2_PROBE_TURNAROUND_EVENT_PROBE_TRANSMIT_READY;

	if (v90_phase2_probe_turnaround_sample(
		    &phase2->state.turnaround, received_sample,
		    transmit_sample, &child) != PHASE2_PROBE_TURNAROUND_OK)
		return false;
	switch (child.kind) {
	case PHASE2_PROBE_TURNAROUND_EVENT_NONE:
		return true;
	case PHASE2_PROBE_TURNAROUND_EVENT_PROBE_TRANSMIT_READY:
	case PHASE2_PROBE_TURNAROUND_EVENT_PROBE_RECEIVE_READY:
		return child.kind == ready &&
		       enter_reciprocal_probe(phase2, event);
	case PHASE2_PROBE_TURNAROUND_EVENT_FAILED:
		phase2->failure = PCM_PHASE2_FAILURE_REMOTE_REVERSAL_TIMEOUT;
		event->kind = PCM_PHASE2_EVENT_FAILED;
		event->failure = phase2->failure;
		return true;
	}
	return false;
}

static bool sample_reciprocal_probe(
	struct pcm_phase2 *phase2, int16_t received_sample,
	int16_t *transmit_sample, struct pcm_phase2_event *event)
{
	struct phase2_reciprocal_probe_event child;

	if (v90_phase2_reciprocal_probe_sample(
		    &phase2->state.reciprocal_probe, received_sample,
		    transmit_sample, &child) != PHASE2_RECIPROCAL_PROBE_OK)
		return false;
	switch (child.kind) {
	case PHASE2_RECIPROCAL_PROBE_EVENT_NONE:
		return true;
	case PHASE2_RECIPROCAL_PROBE_EVENT_REMOTE_PROBE_MEASURED:
		if (phase2->local_role != V90_ANALOG_MODEM ||
		    v90_phase2_reciprocal_probe_measure(
			    &phase2->state.reciprocal_probe,
			    &phase2->remote_probe) != PHASE2_RECIPROCAL_PROBE_OK ||
		    !v34_phase2_probe_measurement_is_valid(
			    &phase2->remote_probe))
			return false;
		phase2->remote_probe_stored = true;
		event->kind = PCM_PHASE2_EVENT_READY_FOR_INFO1;
		return true;
	case PHASE2_RECIPROCAL_PROBE_EVENT_LOCAL_PROBE_ACKNOWLEDGED:
		if (phase2->local_role != V90_DIGITAL_MODEM ||
		    !phase2->remote_probe_stored)
			return false;
		event->kind = PCM_PHASE2_EVENT_READY_FOR_INFO1;
		return true;
	case PHASE2_RECIPROCAL_PROBE_EVENT_FAILED:
		phase2->failure = child.failure ==
					  PHASE2_RECIPROCAL_PROBE_REMOTE_NOT_FOUND ?
				  PCM_PHASE2_FAILURE_REMOTE_PROBE_NOT_FOUND :
				  PCM_PHASE2_FAILURE_LOCAL_PROBE_LIMIT;
		event->kind = PCM_PHASE2_EVENT_FAILED;
		event->failure = phase2->failure;
		return true;
	}
	return false;
}

static bool sample_info1(
	struct pcm_phase2 *phase2, int16_t received_sample,
	int16_t *transmit_sample, struct pcm_phase2_event *event)
{
	struct pcm_phase2_info1_event child;

	if (pcm_phase2_info1_sample(
		    &phase2->state.info1, received_sample,
		    transmit_sample, &child) != PHASE2_INFO1_OK)
		return false;
	switch (child.kind) {
	case PCM_PHASE2_INFO1_EVENT_NONE:
		return true;
	case PCM_PHASE2_INFO1_EVENT_LOCAL_INFO1D_SENT:
		event->kind = PCM_PHASE2_EVENT_LOCAL_INFO1D_SENT;
		return true;
	case PCM_PHASE2_INFO1_EVENT_PEER_INFO1D_RECEIVED:
		event->kind = PCM_PHASE2_EVENT_PEER_INFO1D_RECEIVED;
		return true;
	case PCM_PHASE2_INFO1_EVENT_LOCAL_INFO1A_SENT:
		event->kind = PCM_PHASE2_EVENT_LOCAL_INFO1A_SENT;
		return true;
	case PCM_PHASE2_INFO1_EVENT_PEER_INFO1A_RECEIVED:
		event->kind = PCM_PHASE2_EVENT_PEER_INFO1A_RECEIVED;
		return true;
	case PCM_PHASE2_INFO1_EVENT_PEER_FRAME_REJECTED:
		event->kind = PCM_PHASE2_EVENT_PEER_INFO1_REJECTED;
		return true;
	case PCM_PHASE2_INFO1_EVENT_FAILED:
		phase2->failure = PCM_PHASE2_FAILURE_INFO1_TIMEOUT;
		event->kind = PCM_PHASE2_EVENT_FAILED;
		event->failure = phase2->failure;
		return true;
	}
	return false;
}

enum pcm_phase2_result pcm_phase2_sample(
	struct pcm_phase2 *phase2, int16_t received_sample,
	int16_t *transmit_sample, struct pcm_phase2_event *event)
{
	struct pcm_phase2_event emitted = { 0 };
	int16_t generated = 0;
	bool accepted;

	if (!pcm_phase2_is_valid(phase2) || transmit_sample == NULL ||
	    event == NULL)
		return PCM_PHASE2_INVALID_ARGUMENT;
	if (phase2->failure != PCM_PHASE2_FAILURE_NONE) {
		*transmit_sample = 0;
		*event = emitted;
		return PCM_PHASE2_OK;
	}
	switch (phase2->stage) {
	case PCM_PHASE2_RANGING:
		accepted = sample_ranging(
			phase2, received_sample, &generated, &emitted);
		break;
	case PCM_PHASE2_INITIAL_PROBE:
		accepted = sample_initial_probe(
			phase2, received_sample, &generated, &emitted);
		break;
	case PCM_PHASE2_PROBE_TURNAROUND:
		accepted = sample_turnaround(
			phase2, received_sample, &generated, &emitted);
		break;
	case PCM_PHASE2_RECIPROCAL_PROBE:
		accepted = sample_reciprocal_probe(
			phase2, received_sample, &generated, &emitted);
		break;
	case PCM_PHASE2_INFO1:
		accepted = sample_info1(
			phase2, received_sample, &generated, &emitted);
		break;
	default:
		return PCM_PHASE2_INVALID_ARGUMENT;
	}
	if (!accepted || !pcm_phase2_is_valid(phase2)) {
		fail_internal(phase2, &emitted);
		if (!pcm_phase2_is_valid(phase2))
			return PCM_PHASE2_INVALID_STATE;
	}
	*transmit_sample = generated;
	*event = emitted;
	return PCM_PHASE2_OK;
}

enum pcm_phase2_result pcm_phase2_measure_remote_probe(
	const struct pcm_phase2 *phase2,
	struct v34_phase2_probe_receiver_measurement *measurement)
{
	if (!pcm_phase2_is_valid(phase2) || measurement == NULL)
		return PCM_PHASE2_INVALID_ARGUMENT;
	if (!ready_for_info1(phase2) && phase2->stage != PCM_PHASE2_INFO1)
		return PCM_PHASE2_INVALID_STATE;
	*measurement = phase2->remote_probe;
	return PCM_PHASE2_OK;
}

const char *pcm_phase2_result_name(enum pcm_phase2_result result)
{
	switch (result) {
	case PCM_PHASE2_OK:
		return "ok";
	case PCM_PHASE2_INVALID_ARGUMENT:
		return "invalid argument";
	case PCM_PHASE2_INVALID_STATE:
		return "invalid state";
	}
	return "unknown";
}

const char *pcm_phase2_failure_name(enum pcm_phase2_failure failure)
{
	switch (failure) {
	case PCM_PHASE2_FAILURE_NONE:
		return "none";
	case PCM_PHASE2_FAILURE_RANGING_TIMEOUT:
		return "ranging timeout";
	case PCM_PHASE2_FAILURE_REMOTE_PROBE_NOT_FOUND:
		return "remote probe not found";
	case PCM_PHASE2_FAILURE_LOCAL_PROBE_LIMIT:
		return "local probe limit";
	case PCM_PHASE2_FAILURE_REMOTE_TONE_TIMEOUT:
		return "remote tone timeout";
	case PCM_PHASE2_FAILURE_REMOTE_REVERSAL_TIMEOUT:
		return "remote reversal timeout";
	case PCM_PHASE2_FAILURE_INFO1_TIMEOUT:
		return "INFO1 timeout";
	case PCM_PHASE2_FAILURE_INTERNAL:
		return "internal failure";
	}
	return "unknown";
}
