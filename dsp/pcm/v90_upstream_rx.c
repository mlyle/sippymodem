// SPDX-License-Identifier: BSD-3-Clause

#include "v90_upstream_rx.h"

#include "memory_region.h"
#include "pcm_rate.h"
#include "v34_data_mapping.h"
#include "v90_jd.h"
#include "v90_upstream_mode.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static enum v90_upstream_rx_result begin_mapping_preparation(
	struct v90_upstream_rx *receiver)
{
	struct v34_data_direction_parameters direction;
	struct v90_cp_parameters final = receiver->data_parameters;

	final.acknowledges_peer = true;
	if (v90_upstream_mode_select(
		    &final, &receiver->local_mp,
		    &receiver->state.cp.transmitting_profile, &direction) !=
	    V90_UPSTREAM_MODE_OK ||
	    v34_data_mapping_begin_calibration(
		    &receiver->data_mapping, &receiver->calibration,
		    &receiver->state.cp.transmitting_profile, &direction, false) !=
	    V34_DATA_MAPPING_OK)
		return V90_UPSTREAM_RX_NUMERIC_FAILURE;
	return V90_UPSTREAM_RX_OK;
}

static bool downstream_rate_is_enabled(
	const struct v90_upstream_rx *receiver,
	const struct v90_cp_parameters *parameters)
{
	return parameters->selected_rate_index > 0 &&
	       (receiver->enabled_downstream_rate_mask &
		(UINT32_C(1) << (parameters->selected_rate_index - 1u))) != 0;
}

static bool data_parameters_are_equal(
	const struct v90_cp_parameters *left,
	const struct v90_cp_parameters *right)
{
	struct v90_cp_parameters normalized_left = *left;
	struct v90_cp_parameters normalized_right = *right;

	normalized_left.acknowledges_peer = false;
	normalized_right.acknowledges_peer = false;
	return v90_cp_parameters_are_equal(&normalized_left, &normalized_right);
}

static enum v90_upstream_rx_result validate_data_parameters(
	const struct v90_upstream_rx *receiver,
	const struct v90_cp_parameters *parameters)
{
	struct v90_cp_parameters final = *parameters;
	struct v90_pcm_mapping mapping;
	struct v34_data_direction_parameters direction;
	enum v90_upstream_mode_result result;

	if (!v90_cp_parameters_are_valid(parameters) ||
	    parameters->kind != V90_CP_DATA ||
	    parameters->selected_rate_index == 0 ||
	    parameters->request_silence)
		return V90_UPSTREAM_RX_INVALID_PARAMETERS;
	if (v90_pcm_mapping_init_transmit(
		    &mapping, parameters, receiver->training_mapping.law) !=
	    V90_PCM_MAPPING_OK)
		return V90_UPSTREAM_RX_INVALID_PARAMETERS;
	final.acknowledges_peer = true;
	result = v90_upstream_mode_select(
		&final, &receiver->local_mp,
		&receiver->state.cp.transmitting_profile, &direction);
	switch (result) {
	case V90_UPSTREAM_MODE_OK:
		return V90_UPSTREAM_RX_OK;
	case V90_UPSTREAM_MODE_NO_COMMON_RATE:
		return V90_UPSTREAM_RX_NO_COMMON_RATE;
	case V90_UPSTREAM_MODE_INVALID_PARAMETERS:
		return V90_UPSTREAM_RX_INVALID_PARAMETERS;
	case V90_UPSTREAM_MODE_INVALID_ARGUMENT:
		break;
	}
	return V90_UPSTREAM_RX_NUMERIC_FAILURE;
}

bool v90_upstream_rx_is_valid(const struct v90_upstream_rx *receiver)
{
	if (receiver == NULL ||
	    !v90_phase4_cp_rx_is_valid(&receiver->control_symbols) ||
	    !v90_pcm_mapping_is_valid(&receiver->training_mapping) ||
	    receiver->training_mapping.kind != V90_CP_TRAINING ||
	    !v90_mp_record_is_initial(&receiver->local_mp) ||
	    (receiver->mapping_prepared &&
	     (!receiver->data_cp_received || receiver->mp_selection_pending)) ||
	    (receiver->preparation_request != V90_UPSTREAM_RX_OK &&
	     receiver->preparation_request != V90_UPSTREAM_RX_REQUEST_MP_PRIME &&
	     receiver->preparation_request != V90_UPSTREAM_RX_REQUEST_RT) ||
	    (!receiver->mapping_prepared && receiver->data_cp_received &&
	     receiver->preparation_request == V90_UPSTREAM_RX_OK) ||
	    (receiver->mapping_prepared && receiver->preparation_request != V90_UPSTREAM_RX_OK) ||
	    !pcm_downstream_rate_mask_is_usable(
		    receiver->enabled_downstream_rate_mask) ||
	    (receiver->stage != V90_UPSTREAM_RX_STAGE_CP &&
	     receiver->stage != V90_UPSTREAM_RX_STAGE_CPS &&
	     receiver->stage != V90_UPSTREAM_RX_STAGE_RECONDITION &&
	     !receiver->data_cp_received) ||
	    (receiver->data_cp_received &&
	     (!v90_cp_parameters_are_valid(&receiver->data_parameters) ||
	      receiver->data_parameters.kind != V90_CP_DATA ||
	      receiver->data_parameters.acknowledges_peer ||
	      receiver->data_parameters.request_silence ||
	      receiver->data_parameters.selected_rate_index == 0 ||
	      !downstream_rate_is_enabled(
		      receiver, &receiver->data_parameters))))
		return false;

	switch (receiver->stage) {
	case V90_UPSTREAM_RX_STAGE_CPS:
	case V90_UPSTREAM_RX_STAGE_RECONDITION:
		if (receiver->data_cp_received ||
		    !v90_cp_parameters_are_valid(&receiver->data_parameters) ||
		    receiver->data_parameters.kind != V90_CP_DATA ||
		    !receiver->data_parameters.request_silence ||
		    receiver->data_parameters.acknowledges_peer ||
		    receiver->data_parameters.selected_rate_index == 0u)
			return false;
		/* CP reception stays continuous through CPs-prime and SCR. */
		/* fall through */
	case V90_UPSTREAM_RX_STAGE_CP:
		return v90_phase4_cp_line_rx_is_valid(&receiver->state.cp) &&
		       (!receiver->data_cp_received ||
			(receiver->state.cp.boundary_complete &&
			 receiver->state.cp.symbols.consecutive_ones < V34_PHASE4_E_BITS)) &&
		       ((receiver->data_cp_received &&
			 receiver->state.cp.symbols.consecutive_ones != 0u) ||
			v90_phase4_cp_rx_equal(&receiver->control_symbols,
					       &receiver->state.cp.symbols)) &&
		       !v90_phase4_cp_line_rx_has_pending_record(
			       &receiver->state.cp);
	case V90_UPSTREAM_RX_STAGE_E:
		return v90_phase4_e_line_rx_is_valid(&receiver->state.e) &&
		       data_parameters_are_equal(
			       &receiver->data_parameters,
			       &receiver->state.e.cp) &&
		       receiver->state.e.cp.acknowledges_peer &&
		       (receiver->failed ||
			!v90_phase4_e_line_rx_is_complete(
				&receiver->state.e));
	case V90_UPSTREAM_RX_STAGE_B1:
		return v34_data_line_receiver_is_valid(&receiver->state.data) &&
		       (receiver->failed ||
			v34_data_line_receiver_is_receiving_b1(
				&receiver->state.data));
	case V90_UPSTREAM_RX_STAGE_COMPLETE:
		return !receiver->failed &&
		       v34_data_line_receiver_is_valid(&receiver->state.data) &&
		       !v34_data_line_receiver_has_failed(
			       &receiver->state.data) &&
		       !v34_data_line_receiver_is_receiving_b1(
			       &receiver->state.data);
	}
	return false;
}

bool v90_upstream_rx_has_failed(const struct v90_upstream_rx *receiver)
{
	return v90_upstream_rx_is_valid(receiver) && receiver->failed;
}

bool v90_upstream_rx_is_complete(const struct v90_upstream_rx *receiver)
{
	return v90_upstream_rx_is_valid(receiver) && !receiver->failed &&
	       receiver->stage == V90_UPSTREAM_RX_STAGE_COMPLETE;
}

enum v90_upstream_rx_result v90_upstream_rx_init(
	struct v90_upstream_rx *receiver,
	const struct v90_phase4_cp_line_rx *cp,
	const struct v90_pcm_mapping *training_mapping,
	const struct v90_mp_record *local_mp,
	uint32_t enabled_downstream_rate_mask)
{
	struct v90_upstream_rx initialized = {
		.stage = V90_UPSTREAM_RX_STAGE_CP,
	};

	if (receiver == NULL ||
	    !v90_phase4_cp_line_rx_is_valid(cp) ||
	    v90_phase4_cp_line_rx_has_failed(cp) ||
	    v90_phase4_cp_line_rx_has_pending_record(cp) ||
	    !v90_pcm_mapping_is_valid(training_mapping) ||
	    training_mapping->kind != V90_CP_TRAINING ||
	    !v90_mp_record_is_initial(local_mp) ||
	    !pcm_downstream_rate_mask_is_usable(enabled_downstream_rate_mask))
		return V90_UPSTREAM_RX_INVALID_ARGUMENT;
	initialized.state.cp = *cp;
	initialized.control_symbols = cp->symbols;
	initialized.training_mapping = *training_mapping;
	initialized.local_mp = *local_mp;
	initialized.enabled_downstream_rate_mask =
		enabled_downstream_rate_mask;
	if (!v90_upstream_rx_is_valid(&initialized))
		return V90_UPSTREAM_RX_INVALID_PARAMETERS;
	*receiver = initialized;
	return V90_UPSTREAM_RX_OK;
}

static enum v90_upstream_rx_result fail_receiver(
	struct v90_upstream_rx *receiver,
	struct v90_upstream_rx *next,
	enum v90_upstream_rx_result result)
{
	next->failed = true;
	if (v90_upstream_rx_is_valid(next))
		*receiver = *next;
	else
		receiver->failed = true;
	return result;
}

static enum v90_upstream_rx_result continue_cp(
	struct v90_upstream_rx *receiver)
{
	return v90_phase4_cp_line_rx_continue(&receiver->state.cp) ==
		       V90_PHASE4_CP_LINE_RX_OK ?
		       V90_UPSTREAM_RX_OK :
		       V90_UPSTREAM_RX_NUMERIC_FAILURE;
}

enum v90_upstream_rx_result v90_upstream_rx_select_mp(
	struct v90_upstream_rx *receiver, const struct v90_mp_record *mp)
{
	struct v90_upstream_rx next;

	if (!v90_upstream_rx_is_valid(receiver) || !v90_mp_record_is_initial(mp))
		return V90_UPSTREAM_RX_INVALID_ARGUMENT;
	if (receiver->failed || !receiver->mp_selection_pending ||
	    receiver->stage != V90_UPSTREAM_RX_STAGE_CP)
		return V90_UPSTREAM_RX_INVALID_STATE;
	next = *receiver;
	next.local_mp = *mp;
	if (next.data_cp_received &&
	    begin_mapping_preparation(&next) != V90_UPSTREAM_RX_OK)
		return V90_UPSTREAM_RX_NUMERIC_FAILURE;
	next.mp_selection_pending = false;
	if (!v90_upstream_rx_is_valid(&next))
		return V90_UPSTREAM_RX_INVALID_STATE;
	*receiver = next;
	return V90_UPSTREAM_RX_OK;
}

static enum v90_upstream_rx_result accept_cp_record(
	struct v90_upstream_rx *receiver,
	const struct v90_cp_receive_event *event,
	struct v90_cp_parameters *emitted,
	bool *event_ready)
{
	struct v90_cp_parameters parameters;
	enum v90_upstream_rx_result result;

	if (!v90_phase4_cp_line_rx_pending_record_is_valid(
		    &receiver->state.cp))
		return continue_cp(receiver);
	if (v90_phase4_cp_line_rx_pending_parameters(
		    &receiver->state.cp, &parameters) !=
		    V90_PHASE4_CP_LINE_RX_OK ||
	    !v90_cp_parameters_are_equal(&parameters, &event->parameters))
		return V90_UPSTREAM_RX_NUMERIC_FAILURE;
	/* V.90 9.7: zero rate is a hangup, not a data selection. Its
	 * constellation and capability fields do not constrain cleardown. */
	if (parameters.selected_rate_index == 0u) {
		result = continue_cp(receiver);
		return result == V90_UPSTREAM_RX_OK ?
			V90_UPSTREAM_RX_CLEARDOWN : result;
	}
	if (parameters.kind == V90_CP_TRAINING) {
		if (receiver->stage != V90_UPSTREAM_RX_STAGE_CP ||
		    receiver->data_cp_received)
			return V90_UPSTREAM_RX_SEQUENCE_MISMATCH;
		/* Ri has already acknowledged the original CPt and its mapping
		 * owns our running training transmitter. Some peers send a final
		 * CPt with revised constellations before their data CP. Consume
		 * that record without changing the mapping or acknowledging data;
		 * the call's existing training deadline still bounds this wait. */
		return continue_cp(receiver);
	}
	/* CPs controls echo reconditioning, not a data-rate selection. In
	 * particular its constellations need not admit a data mapping. */
	if (parameters.request_silence) {
		if (receiver->data_cp_received)
			return V90_UPSTREAM_RX_SEQUENCE_MISMATCH;
		if (receiver->stage == V90_UPSTREAM_RX_STAGE_CP) {
			receiver->data_parameters = parameters;
			receiver->data_parameters.acknowledges_peer = false;
			receiver->stage = V90_UPSTREAM_RX_STAGE_CPS;
			result = V90_UPSTREAM_RX_REQUEST_MP_PRIME;
		} else {
			if (!data_parameters_are_equal(
				    &parameters, &receiver->data_parameters))
				return V90_UPSTREAM_RX_SEQUENCE_MISMATCH;
			if (receiver->stage == V90_UPSTREAM_RX_STAGE_RECONDITION ||
			    !parameters.acknowledges_peer)
				return continue_cp(receiver);
			receiver->stage = V90_UPSTREAM_RX_STAGE_RECONDITION;
			result = V90_UPSTREAM_RX_REQUEST_SILENCE;
		}
		if (continue_cp(receiver) != V90_UPSTREAM_RX_OK)
			return V90_UPSTREAM_RX_NUMERIC_FAILURE;
		*emitted = parameters;
		*event_ready = true;
		return result;
	}
	if (receiver->stage == V90_UPSTREAM_RX_STAGE_CPS)
		return V90_UPSTREAM_RX_SEQUENCE_MISMATCH;
	result = validate_data_parameters(receiver, &parameters);
	if (result != V90_UPSTREAM_RX_OK)
		return result;
	/* Jd advertises the digital transmitter's enabled downstream rates.
	 * Consume an unadvertised CP without acknowledging or retaining it, then
	 * keep the continuous receiver searching for an admissible repetition. */
	if (!downstream_rate_is_enabled(receiver, &parameters))
		return continue_cp(receiver);
	if (!receiver->data_cp_received) {
		bool reconditioned =
			receiver->stage == V90_UPSTREAM_RX_STAGE_RECONDITION;

		/* CP prime repeats the same data parameters and additionally says
		 * that the analogue modem has received MP. Receiver acquisition may
		 * miss every preceding CP while the peer legitimately advances after
		 * receiving our independently transmitted MP, so accept CP prime as
		 * the first valid data record and retain its parameters without the
		 * transient acknowledgement bit. */
		receiver->data_cp_received = true;
		receiver->data_parameters = parameters;
		receiver->data_parameters.acknowledges_peer = false;
		receiver->stage = V90_UPSTREAM_RX_STAGE_CP;
		result = continue_cp(receiver);
		if (result != V90_UPSTREAM_RX_OK)
			return result;
		if (!receiver->mp_selection_pending &&
		    begin_mapping_preparation(receiver) != V90_UPSTREAM_RX_OK)
			return V90_UPSTREAM_RX_NUMERIC_FAILURE;
		receiver->preparation_request = reconditioned ? V90_UPSTREAM_RX_REQUEST_RT :
			V90_UPSTREAM_RX_REQUEST_MP_PRIME;
		return V90_UPSTREAM_RX_OK;
	}
	if (!data_parameters_are_equal(
		    &parameters, &receiver->data_parameters))
		return V90_UPSTREAM_RX_SEQUENCE_MISMATCH;
	if (!parameters.acknowledges_peer || !receiver->mapping_prepared)
		return continue_cp(receiver);
	{
		struct v90_phase4_e_line_rx e;

		if (v90_phase4_e_line_rx_init(
			    &e, &receiver->state.cp) !=
		    V90_PHASE4_E_LINE_RX_OK)
			return V90_UPSTREAM_RX_NUMERIC_FAILURE;
		receiver->state = (union v90_upstream_rx_state){ 0 };
		receiver->state.e = e;
		receiver->stage = V90_UPSTREAM_RX_STAGE_E;
	}
	*emitted = parameters;
	*event_ready = true;
	return V90_UPSTREAM_RX_REQUEST_ED;
}

static enum v90_upstream_rx_result start_b1(
	struct v90_upstream_rx *receiver,
	const struct v90_phase4_e_line_rx *e)
{
	struct v34_data_line_receiver data;

	struct v34_data_line_receiver_configuration configuration = {
		.transmitting_profile = receiver->data_mapping.transmitting_profile,
		.geometry = receiver->data_mapping.schedule.geometry,
		.normalizer = receiver->data_mapping.normalizer,
		.trellis_selection = receiver->data_mapping.encoder.convolutional_encoder.selection,
		.expanded_shaping = receiver->data_mapping.encoder.parameters.expanded_shaping,
		.detect_rate_renegotiation = true,
	};
	memcpy(configuration.precoder_coefficients,
		receiver->data_mapping.encoder.precoder.coefficients,
		sizeof(configuration.precoder_coefficients));
	if (!receiver->mapping_prepared ||
	    v90_phase4_e_line_rx_start_data(e, &data, &configuration) != V90_PHASE4_E_LINE_RX_OK)
		return V90_UPSTREAM_RX_NUMERIC_FAILURE;
	receiver->state = (union v90_upstream_rx_state){ 0 };
	receiver->state.data = data;
	receiver->stage = V90_UPSTREAM_RX_STAGE_B1;
	return V90_UPSTREAM_RX_E_COMPLETE;
}

static enum v90_upstream_rx_result receive_cp_sample(
	struct v90_upstream_rx *receiver, int16_t sample,
	struct v90_cp_parameters *emitted, bool *event_ready)
{
	struct v90_cp_receive_event event = { 0 };
	enum v90_phase4_cp_line_rx_result result =
		v90_phase4_cp_line_rx_sample(
			&receiver->state.cp, sample, &event, NULL);

	/* CPt and echo-reconditioning SCR are not E. Only a normal data CP
	 * authorizes the 20-one marker in the continuous CP decoder. */
	if (receiver->data_cp_received &&
	    (result == V90_PHASE4_CP_LINE_RX_SYMBOL ||
	     result == V90_PHASE4_CP_LINE_RX_RECORD_READY) &&
	    receiver->state.cp.symbols.consecutive_ones == V34_PHASE4_E_BITS) {
		/* V.90 9.4.1.4: E acknowledges MP even if no CP-prime survived.
		 * Keep the physical receiver at the exact E/B1 symbol boundary. */
		const struct v90_phase4_cp_line_rx *cp = &receiver->state.cp;
		struct v90_phase4_e_line_rx e = {
			.line = cp->line,
			.symbols = {
				.descrambler = cp->symbols.descrambler,
				.transmitting_role = cp->symbols.transmitting_role,
				.constellation = cp->symbols.constellation,
				.previous_clockwise_quarter_turns =
					cp->symbols.previous_clockwise_quarter_turns,
				.received_bits = cp->symbols.consecutive_ones,
			},
			.transmitting_profile = cp->transmitting_profile,
			.cp = receiver->data_parameters,
			.decoder_power_q15 = cp->decoder_power_q15,
			.adaptation = cp->adaptation,
		};

		if (!receiver->mapping_prepared)
			return V90_UPSTREAM_RX_SEQUENCE_MISMATCH;
		e.cp.acknowledges_peer = true;
		return start_b1(receiver, &e);
	}

	/* Preserve the CP state preceding a possible E run. CP frames end in
	 * zeros on a whole-symbol boundary; neither their 17-one prefix nor
	 * their zero-delimited fields can complete the 20-one E detector. */
	if ((result == V90_PHASE4_CP_LINE_RX_OK ||
	     result == V90_PHASE4_CP_LINE_RX_SYMBOL ||
	     result == V90_PHASE4_CP_LINE_RX_RECORD_READY) &&
	    (!receiver->data_cp_received || receiver->state.cp.symbols.consecutive_ones == 0u))
		receiver->control_symbols = receiver->state.cp.symbols;

	switch (result) {
	case V90_PHASE4_CP_LINE_RX_OK:
	case V90_PHASE4_CP_LINE_RX_SYMBOL:
		return V90_UPSTREAM_RX_OK;
	case V90_PHASE4_CP_LINE_RX_RECORD_READY:
		return accept_cp_record(
			receiver, &event, emitted, event_ready);
	case V90_PHASE4_CP_LINE_RX_FAILED:
		return V90_UPSTREAM_RX_FAILED;
	case V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT:
	case V90_PHASE4_CP_LINE_RX_INVALID_STATE:
	case V90_PHASE4_CP_LINE_RX_NUMERIC_FAILURE:
		break;
	}
	return V90_UPSTREAM_RX_NUMERIC_FAILURE;
}

static enum v90_upstream_rx_result receive_e_sample(
	struct v90_upstream_rx *receiver, int16_t sample)
{
	struct v90_phase4_e_line_rx e = receiver->state.e;
	enum v90_phase4_e_line_rx_result result =
		v90_phase4_e_line_rx_sample(&e, sample);

	switch (result) {
	case V90_PHASE4_E_LINE_RX_OK:
	case V90_PHASE4_E_LINE_RX_SYMBOL:
		receiver->state.e = e;
		return V90_UPSTREAM_RX_OK;
	case V90_PHASE4_E_LINE_RX_E_COMPLETE:
		return start_b1(receiver, &e);
	case V90_PHASE4_E_LINE_RX_FAILED:
		return V90_UPSTREAM_RX_FAILED;
	case V90_PHASE4_E_LINE_RX_INVALID_ARGUMENT:
	case V90_PHASE4_E_LINE_RX_INVALID_STATE:
	case V90_PHASE4_E_LINE_RX_NUMERIC_FAILURE:
	case V90_PHASE4_E_LINE_RX_COMPLETE:
		break;
	}
	return V90_UPSTREAM_RX_NUMERIC_FAILURE;
}

static enum v90_upstream_rx_result receive_b1_sample(
	struct v90_upstream_rx *receiver, int16_t sample)
{
	struct v34_survivor_decision current = { 0 };
	struct v34_received_mapping_frame received = { 0 };
	enum v34_data_receive_result result =
		v34_data_line_receiver_sample(
			&receiver->state.data, sample, &current, &received);

	switch (result) {
	case V34_DATA_RECEIVE_OK:
	case V34_DATA_RECEIVE_INTERVAL_READY:
	case V34_DATA_RECEIVE_B1_IN_PROGRESS:
		return V90_UPSTREAM_RX_OK;
	case V34_DATA_RECEIVE_B1_COMPLETE:
		receiver->stage = V90_UPSTREAM_RX_STAGE_COMPLETE;
		return V90_UPSTREAM_RX_B1_COMPLETE;
	case V34_DATA_RECEIVE_FAILED:
		return V90_UPSTREAM_RX_FAILED;
	case V34_DATA_RECEIVE_OUT_OF_RANGE:
		return V90_UPSTREAM_RX_OUT_OF_RANGE;
	case V34_DATA_RECEIVE_INVALID_ARGUMENT:
	case V34_DATA_RECEIVE_INVALID_STATE:
	case V34_DATA_RECEIVE_NUMERIC_FAILURE:
	case V34_DATA_RECEIVE_MAPPING_FRAME_READY:
	case V34_DATA_RECEIVE_RATE_RENEGOTIATION:
		break;
	}
	return V90_UPSTREAM_RX_NUMERIC_FAILURE;
}

enum v90_upstream_rx_result v90_upstream_rx_sample(
	struct v90_upstream_rx *receiver, int16_t sample,
	struct v90_cp_parameters *event)
{
	struct v90_upstream_rx next;
	struct v90_cp_parameters emitted = { 0 };
	bool event_ready = false;
	enum v90_upstream_rx_result result;

	if (!v90_upstream_rx_is_valid(receiver) || event == NULL ||
	    memory_regions_overlap(
		    receiver, sizeof(*receiver), event, sizeof(*event)))
		return V90_UPSTREAM_RX_INVALID_ARGUMENT;
	if (receiver->failed)
		return V90_UPSTREAM_RX_FAILED;
	if (receiver->stage == V90_UPSTREAM_RX_STAGE_COMPLETE)
		return V90_UPSTREAM_RX_COMPLETE;
	next = *receiver;
	switch (next.stage) {
	case V90_UPSTREAM_RX_STAGE_CP:
	case V90_UPSTREAM_RX_STAGE_CPS:
	case V90_UPSTREAM_RX_STAGE_RECONDITION:
		result = receive_cp_sample(
			&next, sample, &emitted, &event_ready);
		break;
	case V90_UPSTREAM_RX_STAGE_E:
		result = receive_e_sample(&next, sample);
		break;
	case V90_UPSTREAM_RX_STAGE_B1:
		result = receive_b1_sample(&next, sample);
		break;
	case V90_UPSTREAM_RX_STAGE_COMPLETE:
		return V90_UPSTREAM_RX_COMPLETE;
	default:
		return V90_UPSTREAM_RX_INVALID_ARGUMENT;
	}
	if (result == V90_UPSTREAM_RX_OK && next.data_cp_received &&
	    !next.mp_selection_pending && !next.mapping_prepared) {
		enum v34_power_calibration_result step = v34_mapping_power_calibration_step(
			&next.calibration, 1, &next.data_mapping.normalizer);
		if (step == V34_POWER_CALIBRATION_OK) {
			next.mapping_prepared = true;
			result = next.preparation_request;
			next.preparation_request = V90_UPSTREAM_RX_OK;
			emitted = next.data_parameters;
			event_ready = true;
		} else if (step != V34_POWER_CALIBRATION_IN_PROGRESS) {
			result = V90_UPSTREAM_RX_NUMERIC_FAILURE;
		}
	}
	if (result == V90_UPSTREAM_RX_FAILED ||
	    result == V90_UPSTREAM_RX_SEQUENCE_MISMATCH ||
	    result == V90_UPSTREAM_RX_INVALID_PARAMETERS ||
	    result == V90_UPSTREAM_RX_NO_COMMON_RATE ||
	    result == V90_UPSTREAM_RX_OUT_OF_RANGE ||
	    result == V90_UPSTREAM_RX_NUMERIC_FAILURE)
		return fail_receiver(receiver, &next, result);
	if (!v90_upstream_rx_is_valid(&next))
		return fail_receiver(
			receiver, &next, V90_UPSTREAM_RX_NUMERIC_FAILURE);
	*receiver = next;
	if (event_ready)
		*event = emitted;
	return result;
}

enum v90_upstream_rx_result v90_upstream_rx_handoff(
	const struct v90_upstream_rx *receiver,
	struct v34_data_line_receiver *data_receiver)
{
	if (!v90_upstream_rx_is_valid(receiver) || data_receiver == NULL ||
	    memory_regions_overlap(
		    receiver, sizeof(*receiver), data_receiver,
		    sizeof(*data_receiver)))
		return V90_UPSTREAM_RX_INVALID_ARGUMENT;
	if (receiver->failed)
		return V90_UPSTREAM_RX_FAILED;
	if (receiver->stage != V90_UPSTREAM_RX_STAGE_COMPLETE)
		return V90_UPSTREAM_RX_INVALID_STATE;
	*data_receiver = receiver->state.data;
	return V90_UPSTREAM_RX_OK;
}

static enum v90_upstream_data_result mapping_result(
	enum v34_data_mapping_result result)
{
	switch (result) {
	case V34_DATA_MAPPING_OK:
		return V90_UPSTREAM_DATA_OK;
	case V34_DATA_MAPPING_OUT_OF_RANGE:
		return V90_UPSTREAM_DATA_OUT_OF_RANGE;
	case V34_DATA_MAPPING_INVALID_ARGUMENT:
		return V90_UPSTREAM_DATA_INVALID_PARAMETERS;
	case V34_DATA_MAPPING_NUMERIC_FAILURE:
		break;
	}
	return V90_UPSTREAM_DATA_NUMERIC_FAILURE;
}

enum v90_upstream_data_result v90_upstream_rx_start_after_e(
	struct v34_data_line_receiver *receiver,
	const struct v90_phase4_e_line_rx *completed_e,
	const struct v90_mp_record *mp)
{
	struct v34_data_direction_parameters direction;
	struct v34_data_mapping mapping;
	struct v34_data_line_receiver_configuration configuration = { 0 };
	enum v90_upstream_data_result result;

	if (receiver == NULL || completed_e == NULL || mp == NULL)
		return V90_UPSTREAM_DATA_INVALID_ARGUMENT;
	if (!v90_phase4_e_line_rx_is_complete(completed_e))
		return V90_UPSTREAM_DATA_INVALID_STATE;
	result = v90_upstream_data_mode_result(v90_upstream_mode_select(
		&completed_e->cp, mp, &completed_e->transmitting_profile,
		&direction));
	if (result != V90_UPSTREAM_DATA_OK)
		return result;
	result = mapping_result(v34_data_mapping_prepare(
		&mapping, &completed_e->transmitting_profile, &direction,
		false));
	if (result != V90_UPSTREAM_DATA_OK)
		return result;
	configuration.transmitting_profile =
		completed_e->transmitting_profile;
	configuration.geometry = mapping.schedule.geometry;
	configuration.normalizer = mapping.normalizer;
	configuration.trellis_selection = direction.trellis_selection;
	configuration.expanded_shaping = direction.expanded_shaping;
	configuration.detect_rate_renegotiation = true;
	memcpy(configuration.precoder_coefficients,
	       direction.precoder_coefficients,
	       sizeof(configuration.precoder_coefficients));
	return v90_phase4_e_line_rx_start_data(
		       completed_e, receiver, &configuration) ==
		       V90_PHASE4_E_LINE_RX_OK ?
		       V90_UPSTREAM_DATA_OK :
		       V90_UPSTREAM_DATA_NUMERIC_FAILURE;
}

const char *v90_upstream_rx_result_name(
	enum v90_upstream_rx_result result)
{
	switch (result) {
	case V90_UPSTREAM_RX_OK:
		return "ok";
	case V90_UPSTREAM_RX_REQUEST_MP_PRIME:
		return "request MP prime";
	case V90_UPSTREAM_RX_REQUEST_ED:
		return "request Ed";
	case V90_UPSTREAM_RX_REQUEST_SILENCE:
		return "request silence";
	case V90_UPSTREAM_RX_REQUEST_RT:
		return "request Rt";
	case V90_UPSTREAM_RX_E_COMPLETE:
		return "E complete";
	case V90_UPSTREAM_RX_B1_COMPLETE:
		return "B1 complete";
	case V90_UPSTREAM_RX_COMPLETE:
		return "complete";
	case V90_UPSTREAM_RX_CLEARDOWN:
		return "cleardown";
	case V90_UPSTREAM_RX_FAILED:
		return "failed";
	case V90_UPSTREAM_RX_SEQUENCE_MISMATCH:
		return "sequence mismatch";
	case V90_UPSTREAM_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_UPSTREAM_RX_INVALID_STATE:
		return "invalid state";
	case V90_UPSTREAM_RX_INVALID_PARAMETERS:
		return "invalid parameters";
	case V90_UPSTREAM_RX_NO_COMMON_RATE:
		return "no common rate";
	case V90_UPSTREAM_RX_OUT_OF_RANGE:
		return "out of range";
	case V90_UPSTREAM_RX_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
