// SPDX-License-Identifier: BSD-3-Clause

#include "v8_answering.h"

#include <string.h>

enum {
	V8_ANSWER_SIGNAL_AVERAGE_AMPLITUDE = 16000,
};

static bool selection_is_valid(
	const struct v8_answering *answering)
{
	unsigned int selected = (unsigned int)answering->selected_modulation;

	return v8_menu_is_usable(&answering->call_menu) &&
	       v8_menu_is_usable(&answering->negotiated_menu) && selected != 0 &&
	       (selected & ~((unsigned int)V8_MODULATION_ALL)) == 0 &&
	       (selected & (selected - 1u)) == 0 &&
	       (answering->negotiated_menu.modulations & selected) != 0 &&
	       (answering->negotiated_menu.modulations &
		~answering->local_menu.modulations) == 0 &&
	       (answering->negotiated_menu.modulations &
		~answering->call_menu.modulations) == 0 &&
	       (!answering->negotiated_menu.lapm ||
		(answering->local_menu.lapm && answering->call_menu.lapm));
}

static bool qc_response_is_usable(const struct v8_menu *menu,
	const struct v92_qc1_message *response)
{
	return v92_qc1_bit_count(response) != 0 && response->acknowledgment &&
	       response->lapm == menu->lapm && menu->pcm_present &&
	       (menu->pcm_availability & (response->sender == V90_DIGITAL_MODEM ?
		V8_PCM_DIGITAL : V8_PCM_ANALOG)) != 0 &&
	       (response->sender != V90_ANALOG_MODEM ||
		response->signal.qts_index != 15);
}

static bool qc_request_is_usable(const struct v8_answering *answering,
	const struct v92_qc1_message *request)
{
	return v92_qc1_bit_count(request) != 0 && !request->acknowledgment &&
	       request->sender != answering->qc_response.sender &&
	       (request->sender != V90_ANALOG_MODEM ||
		request->signal.qts_index != 15);
}

static bool answering_is_valid(
	const struct v8_answering *answering)
{
	if (answering == NULL || !v8_menu_is_usable(&answering->local_menu) ||
	    (unsigned int)answering->state > V8_ANSWERING_FAILED ||
	    (unsigned int)answering->terminal_result >
		    V8_ANSWERING_NO_COMMON_MODULATION ||
	    answering->transmit_bit > 1 ||
	    answering->quiet_samples > V8_ANSWERING_QUIET_SAMPLES ||
	    answering->cj_fallback_bits >=
		    V8_ANSWERING_CJ_FALLBACK_BITS ||
	    (!answering->cj_fallback_armed &&
	     answering->cj_fallback_bits != 0) ||
	    (answering->state != V8_ANSWERING_SEND_JOINT_MENU &&
	     (answering->cj_fallback_armed ||
	      answering->cj_fallback_bits != 0)) ||
	    (answering->state == V8_ANSWERING_FAILED) !=
		    (answering->terminal_result != V8_ANSWERING_OK))
		return false;
	if (answering->qc_enabled && !qc_response_is_usable(
		    &answering->local_menu, &answering->qc_response))
		return false;
	if (answering->state == V8_ANSWERING_SEND_QCA1 ||
	    answering->state == V8_ANSWERING_QC_COMPLETE) {
		if (!answering->qc_enabled || answering->negotiated ||
		    !qc_request_is_usable(answering, &answering->qc_request) ||
		    answering->qc_bit_index >
			    v92_qc1_bit_count(&answering->qc_response))
			return false;
	} else if (answering->qc_bit_index != 0) {
		return false;
	}
	if (answering->negotiated) {
		if (!selection_is_valid(answering))
			return false;
	} else if (answering->call_menu.call_function != 0 ||
		   answering->call_menu.modulations != 0 ||
		   answering->call_menu.modulation_octets != 0 ||
		   answering->call_menu.pcm_availability != 0 ||
		   answering->call_menu.pstn_access != 0 ||
		   answering->call_menu.pcm_present ||
		   answering->call_menu.pstn_access_present ||
		   answering->call_menu.lapm ||
		   answering->negotiated_menu.call_function != 0 ||
		   answering->negotiated_menu.modulations != 0 ||
		   answering->negotiated_menu.modulation_octets != 0 ||
		   answering->negotiated_menu.pcm_availability != 0 ||
		   answering->negotiated_menu.pstn_access != 0 ||
		   answering->negotiated_menu.pcm_present ||
		   answering->negotiated_menu.pstn_access_present ||
		   answering->negotiated_menu.lapm ||
		   answering->selected_modulation != 0) {
		return false;
	}
	switch (answering->state) {
	case V8_ANSWERING_WAIT_CALL_MENU:
		return !answering->negotiated && answering->state_samples <
		       V8_ANSWERING_CALL_MENU_TIMEOUT_SAMPLES;
	case V8_ANSWERING_SEND_JOINT_MENU:
		return answering->negotiated && answering->state_samples <
		       V8_ANSWERING_CJ_TIMEOUT_SAMPLES;
	case V8_ANSWERING_SEND_QCA1:
		return answering->state_samples < V8_FSK_TRANSMIT_FILTER_TAPS;
	case V8_ANSWERING_QC_COMPLETE:
		return answering->state_samples == 0 && answering->qc_bit_index ==
			v92_qc1_bit_count(&answering->qc_response);
	case V8_ANSWERING_COMPLETE:
		return answering->negotiated && answering->state_samples == 0;
	case V8_ANSWERING_FAILED:
		return answering->state_samples == 0 &&
		       ((answering->terminal_result ==
				 V8_ANSWERING_CJ_TIMEOUT &&
			 answering->negotiated) ||
			(!answering->negotiated &&
			 (answering->terminal_result ==
				  V8_ANSWERING_CALL_MENU_TIMEOUT ||
			  answering->terminal_result ==
				  V8_ANSWERING_NO_COMMON_MODULATION)));
	}
	return false;
}

static void enter_state(
	struct v8_answering *answering,
	enum v8_answering_state state)
{
	answering->state = state;
	answering->state_samples = 0;
	if (state != V8_ANSWERING_WAIT_CALL_MENU)
		answering->quiet_samples = 0;
	if (state != V8_ANSWERING_SEND_JOINT_MENU) {
		answering->cj_fallback_armed = false;
		answering->cj_fallback_bits = 0;
	}
}

static enum v8_answering_result fail(
	struct v8_answering *answering,
	enum v8_answering_result result)
{
	enter_state(answering, V8_ANSWERING_FAILED);
	answering->terminal_result = result;
	return result;
}

enum v8_answering_result v8_answering_init(
	struct v8_answering *answering,
	const struct v8_menu *local_menu)
{
	struct v8_answering initialized = {
		.quiet_samples = V8_ANSWERING_QUIET_SAMPLES,
		.transmit_bit = 1,
	};

	if (answering == NULL || !v8_menu_is_usable(local_menu) ||
	    v8_ansam_generator_init(
		    &initialized.answer_signal,
		    V8_ANSWER_SIGNAL_AVERAGE_AMPLITUDE, true) !=
		    V8_ANSAM_OK ||
	    v8_fsk_init(
		    &initialized.fsk, V8_ANSWERING_STATION) !=
		    V8_FSK_OK ||
	    v8_menu_detector_init(
		    &initialized.call_menu_detector) != V8_MENU_OK ||
	    v8_cj_detector_init(&initialized.cj_detector) !=
		    V8_FRAMING_OK)
		return V8_ANSWERING_INVALID_ARGUMENT;
	initialized.local_menu = *local_menu;
	if (!answering_is_valid(&initialized))
		return V8_ANSWERING_INVALID_ARGUMENT;
	*answering = initialized;
	return V8_ANSWERING_OK;
}

enum v8_answering_result v8_answering_enable_quick_connect(
	struct v8_answering *answering,
	const struct v92_qc1_message *response)
{
	if (!answering_is_valid(answering) ||
	    answering->state != V8_ANSWERING_WAIT_CALL_MENU ||
	    answering->state_samples != 0 ||
	    !qc_response_is_usable(&answering->local_menu, response))
		return V8_ANSWERING_INVALID_ARGUMENT;
	if (!v92_qc1_receiver_init(&answering->qc_receiver))
		return V8_ANSWERING_INVALID_ARGUMENT;
	answering->qc_response = *response;
	answering->qc_enabled = true;
	return V8_ANSWERING_OK;
}

static enum v8_answering_result select_joint_menu(
	struct v8_answering *answering,
	const struct v8_menu *call_menu)
{
	struct v8_menu joint_menu;
	enum v8_modulation modulation;
	enum v8_menu_result selection_result;
	uint8_t information[V8_MAX_INFORMATION_OCTETS];
	size_t information_count;

	if (v8_joint_menu_select(
		    call_menu, &answering->local_menu, &joint_menu) !=
	    V8_MENU_OK)
		return V8_ANSWERING_INVALID_ARGUMENT;
	selection_result = v8_menu_selected_modulation(
		&joint_menu, &modulation);
	if (selection_result == V8_MENU_NO_COMMON_MODULATION)
		return fail(
			answering, V8_ANSWERING_NO_COMMON_MODULATION);
	if (selection_result != V8_MENU_OK ||
	    v8_menu_encode(
		    &joint_menu, information, sizeof(information),
		    &information_count) != V8_MENU_OK ||
	    v8_control_generator_init(
		    &answering->joint_menu_bits, information,
		    information_count) != V8_FRAMING_OK)
		return V8_ANSWERING_INVALID_ARGUMENT;
	answering->call_menu = *call_menu;
	answering->negotiated_menu = joint_menu;
	answering->selected_modulation = modulation;
	answering->negotiated = true;
	enter_state(answering, V8_ANSWERING_SEND_JOINT_MENU);
	return V8_ANSWERING_OK;
}

static enum v8_answering_result wait_call_menu_sample(
	struct v8_answering *answering, int16_t receive,
	int16_t *transmit)
{
	struct v8_menu call_menu;
	uint8_t bit;
	bool ready;
	bool confirmed = false;
	int16_t unfiltered;

	if (answering->quiet_samples != 0) {
		*transmit = 0;
		answering->quiet_samples--;
	} else if (v8_ansam_generate(
			   &answering->answer_signal, &unfiltered, 1) !=
			   V8_ANSAM_OK ||
		   v8_fsk_filter_transmit_sample(
			   &answering->fsk, unfiltered, transmit) !=
			   V8_FSK_OK) {
		return V8_ANSWERING_INVALID_ARGUMENT;
	}
	if (v8_fsk_receive_sample(
		    &answering->fsk, receive, &bit, &ready) !=
		    V8_FSK_OK)
		return V8_ANSWERING_INVALID_ARGUMENT;
	if (ready && answering->qc_enabled) {
		struct v92_qc1_message request;
		bool complete;

		/* QC1 and CM use the same continuous V.21 receive stream. */
		if (!v92_qc1_receive_bit(&answering->qc_receiver, bit,
			    &request, &complete))
			return V8_ANSWERING_INVALID_ARGUMENT;
		if (complete && qc_request_is_usable(answering, &request)) {
			answering->qc_request = request;
			enter_state(answering, V8_ANSWERING_SEND_QCA1);
			return V8_ANSWERING_OK;
		}
	}
	if (ready &&
	    v8_menu_detect_bit(
		    &answering->call_menu_detector, bit, &call_menu,
		    &confirmed) != V8_MENU_OK)
		return V8_ANSWERING_INVALID_ARGUMENT;
	if (confirmed)
		return select_joint_menu(answering, &call_menu);
	answering->state_samples++;
	if (answering->state_samples == V8_ANSWERING_COARSE_GAIN_SAMPLES &&
	    v8_line_conditioner_set_gain_mode(
		    &answering->fsk.line, V8_LINE_GAIN_FINE) !=
		    V8_LINE_CONDITIONER_OK)
		return V8_ANSWERING_INVALID_ARGUMENT;
	if (answering->state_samples ==
	    V8_ANSWERING_CALL_MENU_TIMEOUT_SAMPLES)
		return fail(
			answering, V8_ANSWERING_CALL_MENU_TIMEOUT);
	return V8_ANSWERING_OK;
}

static enum v8_answering_result send_joint_menu_sample(
	struct v8_answering *answering, int16_t receive,
	int16_t *transmit)
{
	uint8_t bit;
	bool bit_consumed;
	bool bit_ready;
	bool cj_detected = false;

	if (v8_fsk_can_accept_bit(&answering->fsk)) {
		bool sequence_complete;

		if (v8_control_next_bit(
			    &answering->joint_menu_bits, &answering->transmit_bit,
			    &sequence_complete) != V8_FRAMING_OK)
			return V8_ANSWERING_INVALID_ARGUMENT;
	}
	if (v8_fsk_transmit_sample(
		    &answering->fsk, answering->transmit_bit, transmit,
		    &bit_consumed) != V8_FSK_OK ||
	    v8_fsk_receive_sample(
		    &answering->fsk, receive, &bit, &bit_ready) !=
		    V8_FSK_OK)
		return V8_ANSWERING_INVALID_ARGUMENT;
	if (bit_ready) {
		if (v8_cj_detect_bit(
			    &answering->cj_detector, bit, &cj_detected) !=
		    V8_FRAMING_OK)
			return V8_ANSWERING_INVALID_ARGUMENT;
		if (cj_detected) {
			enter_state(
				answering,
				V8_ANSWERING_COMPLETE);
			return V8_ANSWERING_OK;
		}
		if (!answering->cj_fallback_armed &&
		    answering->cj_detector.matched_octets ==
			    V8_CJ_OCTETS - 1u) {
			answering->cj_fallback_armed = true;
		} else if (answering->cj_fallback_armed &&
			   ++answering->cj_fallback_bits ==
				   V8_ANSWERING_CJ_FALLBACK_BITS) {
			enter_state(
				answering,
				V8_ANSWERING_COMPLETE);
			return V8_ANSWERING_OK;
		}
	}
	answering->state_samples++;
	if (answering->state_samples ==
	    V8_ANSWERING_CJ_TIMEOUT_SAMPLES)
		return fail(answering, V8_ANSWERING_CJ_TIMEOUT);
	return V8_ANSWERING_OK;
}

static enum v8_answering_result send_qca1_sample(
	struct v8_answering *answering, int16_t *transmit)
{
	bool consumed;

	if (v8_fsk_can_accept_bit(&answering->fsk)) {
		if (answering->qc_bit_index ==
			    v92_qc1_bit_count(&answering->qc_response)) {
			if (v8_fsk_filter_transmit_sample(&answering->fsk, 0,
				    transmit) != V8_FSK_OK)
				return V8_ANSWERING_INVALID_ARGUMENT;
			if (++answering->state_samples == V8_FSK_TRANSMIT_FILTER_TAPS)
				enter_state(answering, V8_ANSWERING_QC_COMPLETE);
			return V8_ANSWERING_OK;
		}
		if (!v92_qc1_bit(&answering->qc_response,
			    answering->qc_bit_index, &answering->transmit_bit))
			return V8_ANSWERING_INVALID_ARGUMENT;
		answering->qc_bit_index++;
	}
	return v8_fsk_transmit_sample(&answering->fsk, answering->transmit_bit,
		transmit, &consumed) == V8_FSK_OK ? V8_ANSWERING_OK :
		V8_ANSWERING_INVALID_ARGUMENT;
}

static enum v8_answering_result process_one(
	struct v8_answering *answering, int16_t receive,
	int16_t *transmit)
{
	if (answering->state == V8_ANSWERING_WAIT_CALL_MENU)
		return wait_call_menu_sample(answering, receive, transmit);
	if (answering->state == V8_ANSWERING_SEND_JOINT_MENU)
		return send_joint_menu_sample(answering, receive, transmit);
	if (answering->state == V8_ANSWERING_SEND_QCA1)
		return send_qca1_sample(answering, transmit);
	if (answering->state == V8_ANSWERING_COMPLETE ||
	    answering->state == V8_ANSWERING_QC_COMPLETE)
		return V8_ANSWERING_OK;
	if (answering->state == V8_ANSWERING_FAILED)
		return answering->terminal_result;
	return V8_ANSWERING_INVALID_ARGUMENT;
}

enum v8_answering_result v8_answering_process(
	struct v8_answering *answering, const int16_t *receive,
	int16_t *transmit, size_t count)
{
	struct v8_answering next;
	int16_t generated[V8_ANSWERING_MAX_FRAGMENT_SAMPLES] = { 0 };
	enum v8_answering_result result =
		V8_ANSWERING_OK;
	size_t processed = 0;

	if (!answering_is_valid(answering) ||
	    count > V8_ANSWERING_MAX_FRAGMENT_SAMPLES ||
	    ((receive == NULL || transmit == NULL) && count != 0))
		return V8_ANSWERING_INVALID_ARGUMENT;
	if (count == 0)
		return answering->terminal_result;
	next = *answering;
	for (; processed < count; ++processed) {
		result = process_one(
			&next, receive[processed], &generated[processed]);
		if (result != V8_ANSWERING_OK)
			break;
	}
	if (result == V8_ANSWERING_INVALID_ARGUMENT ||
	    !answering_is_valid(&next))
		return V8_ANSWERING_INVALID_ARGUMENT;
	if (result != V8_ANSWERING_OK)
		memset(
			&generated[processed], 0,
			(count - processed) * sizeof(generated[0]));
	*answering = next;
	memcpy(transmit, generated, count * sizeof(generated[0]));
	return result;
}

bool v8_answering_is_complete(
	const struct v8_answering *answering)
{
	return answering_is_valid(answering) &&
	       (answering->state == V8_ANSWERING_COMPLETE ||
		answering->state == V8_ANSWERING_QC_COMPLETE);
}

enum v8_answering_result v8_answering_selection(
	const struct v8_answering *answering,
	struct v8_menu *menu,
	enum v8_modulation *modulation)
{
	if (!answering_is_valid(answering) || menu == NULL || modulation == NULL)
		return V8_ANSWERING_INVALID_ARGUMENT;
	if (!answering->negotiated)
		return V8_ANSWERING_NOT_READY;
	*menu = answering->negotiated_menu;
	*modulation = answering->selected_modulation;
	return V8_ANSWERING_OK;
}

const char *v8_answering_result_name(
	enum v8_answering_result result)
{
	switch (result) {
	case V8_ANSWERING_OK:
		return "ok";
	case V8_ANSWERING_INVALID_ARGUMENT:
		return "invalid argument";
	case V8_ANSWERING_NOT_READY:
		return "not ready";
	case V8_ANSWERING_CALL_MENU_TIMEOUT:
		return "call menu timeout";
	case V8_ANSWERING_CJ_TIMEOUT:
		return "CJ timeout";
	case V8_ANSWERING_NO_COMMON_MODULATION:
		return "no common modulation";
	}
	return "unknown V.8 answering result";
}
