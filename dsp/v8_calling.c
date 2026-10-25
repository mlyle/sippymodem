// SPDX-License-Identifier: BSD-3-Clause

#include "v8_calling.h"

#include <string.h>

static bool qc_request_is_usable(const struct v8_menu *menu,
	const struct v92_qc1_message *request)
{
	return v92_qc1_bit_count(request) != 0 && !request->acknowledgment &&
	       request->lapm == menu->lapm && menu->pcm_present &&
	       (menu->pcm_availability & (request->sender == V90_DIGITAL_MODEM ?
			V8_PCM_DIGITAL : V8_PCM_ANALOG)) != 0 &&
	       (request->sender != V90_ANALOG_MODEM ||
			request->signal.qts_index != 15);
}

static bool qc_response_is_usable(const struct v8_calling *calling,
	const struct v92_qc1_message *response)
{
	return v92_qc1_bit_count(response) != 0 && response->acknowledgment &&
	       response->sender != calling->qc_request.sender &&
	       (response->sender != V90_ANALOG_MODEM ||
			response->signal.qts_index != 15);
}

static bool calling_is_valid(const struct v8_calling *calling)
{
	if (calling == NULL || !v8_menu_is_usable(&calling->local_menu) ||
	    !v8_ansam_detector_is_valid(&calling->answer_signal) ||
	    (unsigned int)calling->state > V8_CALLING_FAILED ||
	    (unsigned int)calling->terminal_result >
		    V8_CALLING_NO_COMMON_MODULATION ||
	    calling->transmit_bit > 1 ||
	    calling->cj_bit_index > V8_CJ_BITS)
		return false;
	if (calling->phase2_silence && (!calling->negotiated ||
	    calling->selected_modulation != V8_MODULATION_V34_DUPLEX))
		return false;
	if ((calling->state == V8_CALLING_FAILED) !=
	    (calling->terminal_result != V8_CALLING_OK))
		return false;
	if ((calling->qc_enabled || calling->qc_bit_index != 0) &&
	    (!qc_request_is_usable(&calling->local_menu, &calling->qc_request) ||
	     calling->qc_bit_index > v92_qc1_bit_count(&calling->qc_request)))
		return false;
	if (calling->qc_bit_index != 0 && calling->state != V8_CALLING_SEND_QC1 &&
	    calling->qc_bit_index != v92_qc1_bit_count(&calling->qc_request))
		return false;
	if (calling->negotiated) {
		if (!v8_menu_selection_is_valid(&calling->local_menu,
			    &calling->negotiated_menu, calling->selected_modulation))
			return false;
	} else if (!v8_menu_is_unset(&calling->negotiated_menu) ||
		   calling->selected_modulation != 0 ||
		   calling->cj_bit_index != 0) {
		return false;
	}
	switch (calling->state) {
	case V8_CALLING_WAIT_ANSWER_SIGNAL:
		return !calling->negotiated && calling->qc_bit_index == 0 &&
		       calling->state_samples < V8_CALLING_ANSWER_TIMEOUT_SAMPLES;
	case V8_CALLING_EXCHANGE_MENUS:
		return !calling->negotiated &&
		       (!calling->qc_enabled || calling->qc_bit_index ==
			       v92_qc1_bit_count(&calling->qc_request)) &&
		       calling->state_samples < V8_CALLING_MENU_TIMEOUT_SAMPLES;
	case V8_CALLING_SEND_QC1:
		return calling->qc_enabled && !calling->negotiated &&
		       calling->state_samples == 0;
	case V8_CALLING_QC_TX_TAIL:
	case V8_CALLING_QC_COMPLETE:
		return calling->qc_enabled && !calling->negotiated &&
		       calling->qc_bit_index == v92_qc1_bit_count(&calling->qc_request) &&
		       qc_response_is_usable(calling, &calling->qc_response) &&
		       (calling->state == V8_CALLING_QC_COMPLETE ?
			       calling->state_samples == 0 :
			       calling->state_samples < V8_FSK_TRANSMIT_FILTER_TAPS);
	case V8_CALLING_SEND_CJ:
		return calling->negotiated && calling->state_samples <
		       (calling->phase2_silence ? V8_FSK_TRANSMIT_FILTER_TAPS :
			V8_CALLING_CJ_TAIL_SAMPLES);
	case V8_CALLING_COMPLETE:
		return calling->negotiated &&
		       calling->cj_bit_index == V8_CJ_BITS &&
		       calling->state_samples == 0;
	case V8_CALLING_FAILED:
		return !calling->negotiated && calling->state_samples == 0 &&
		       (calling->terminal_result ==
				V8_CALLING_ANSWER_TIMEOUT ||
			calling->terminal_result ==
				V8_CALLING_MENU_TIMEOUT ||
			calling->terminal_result ==
				V8_CALLING_NO_COMMON_MODULATION);
	}
	return false;
}

static void enter_state(
	struct v8_calling *calling,
	enum v8_calling_state state)
{
	calling->state = state;
	calling->state_samples = 0;
}

enum v8_calling_result v8_calling_init(
	struct v8_calling *calling,
	const struct v8_menu *local_menu)
{
	struct v8_calling initialized = { 0 };
	uint8_t information[V8_MAX_INFORMATION_OCTETS];
	size_t information_count;

	if (calling == NULL || !v8_menu_is_usable(local_menu) ||
	    v8_menu_encode(
		    local_menu, information, sizeof(information),
		    &information_count) != V8_MENU_OK ||
	    v8_ansam_detector_init(&initialized.answer_signal) !=
		    V8_ANSAM_OK ||
	    v8_fsk_init(
		    &initialized.fsk, V8_CALLING_STATION) !=
		    V8_FSK_OK ||
	    v8_control_generator_init(
		    &initialized.call_menu_bits, information,
		    information_count) != V8_FRAMING_OK ||
	    v8_menu_detector_init(
		    &initialized.joint_menu_detector) != V8_MENU_OK)
		return V8_CALLING_INVALID_ARGUMENT;
	initialized.local_menu = *local_menu;
	initialized.transmit_bit = 1;
	if (!calling_is_valid(&initialized))
		return V8_CALLING_INVALID_ARGUMENT;
	*calling = initialized;
	return V8_CALLING_OK;
}

enum v8_calling_result v8_calling_enable_quick_connect(
	struct v8_calling *calling, const struct v92_qc1_message *request)
{
	if (!calling_is_valid(calling) ||
	    calling->state != V8_CALLING_WAIT_ANSWER_SIGNAL ||
	    calling->state_samples != 0 ||
	    !qc_request_is_usable(&calling->local_menu, request))
		return V8_CALLING_INVALID_ARGUMENT;
	if (!v92_qc1_receiver_init(&calling->qc_receiver))
		return V8_CALLING_INVALID_ARGUMENT;
	calling->qc_request = *request;
	calling->qc_enabled = true;
	return V8_CALLING_OK;
}

static enum v8_calling_result fail(
	struct v8_calling *calling,
	enum v8_calling_result result)
{
	calling->state = V8_CALLING_FAILED;
	calling->terminal_result = result;
	calling->state_samples = 0;
	return result;
}

static enum v8_calling_result drain_qc_sample(
	struct v8_calling *calling, int16_t *transmit)
{
	if (v8_fsk_filter_transmit_sample(&calling->fsk, 0, transmit) != V8_FSK_OK)
		return V8_CALLING_INVALID_ARGUMENT;
	if (++calling->state_samples == V8_FSK_TRANSMIT_FILTER_TAPS)
		enter_state(calling, V8_CALLING_QC_COMPLETE);
	return V8_CALLING_OK;
}

static bool receive_menu_sample(struct v8_calling *calling, int16_t sample,
	uint8_t *bit, bool *ready)
{
	struct v8_line_conditioned_block block;
	bool block_ready, ansam;

	if (!calling->qc_enabled || calling->qc_request.sender != V90_DIGITAL_MODEM)
		return v8_fsk_receive_sample(&calling->fsk, sample, bit, ready) ==
			V8_FSK_OK;
	/* V.92 9.2.2.1: continued ANSam declines QC, silence does not.
	 * ANSam and QCA1/JM must observe one continuous line front end. */
	*bit = 0;
	*ready = false;
	if (v8_line_conditioner_process_sample(&calling->fsk.line, sample,
		      &block, &block_ready) != V8_LINE_CONDITIONER_OK)
		return false;
	if (!block_ready)
		return true;
	if (v8_ansam_observe_block(&calling->answer_signal, &calling->fsk.line,
		      &block, &ansam) != V8_ANSAM_OK ||
	    v8_fsk_receive_block(&calling->fsk, &block, bit, ready) != V8_FSK_OK)
		return false;
	if (ansam)
		calling->qc_enabled = false;
	return true;
}

static enum v8_calling_result exchange_menu_sample(
	struct v8_calling *calling, int16_t receive,
	int16_t *transmit)
{
	struct v8_menu remote_menu;
	struct v8_menu joint_menu;
	enum v8_modulation modulation;
	uint8_t received_bit;
	bool bit_consumed;
	bool bit_ready;
	bool confirmed = false;

	if (!receive_menu_sample(calling, receive, &received_bit, &bit_ready))
		return V8_CALLING_INVALID_ARGUMENT;
	if (bit_ready && calling->qc_enabled) {
		struct v92_qc1_message response;
		bool complete;

		if (!v92_qc1_receive_bit(&calling->qc_receiver, received_bit,
			      &response, &complete))
			return V8_CALLING_INVALID_ARGUMENT;
		if (complete && qc_response_is_usable(calling, &response)) {
			/* Stop CM now, without completing its current octet. */
			calling->qc_response = response;
			enter_state(calling, V8_CALLING_QC_TX_TAIL);
			return drain_qc_sample(calling, transmit);
		}
	}
	if (v8_fsk_can_accept_bit(&calling->fsk)) {
		bool sequence_complete;

		if (v8_control_next_bit(
			    &calling->call_menu_bits, &calling->transmit_bit,
			    &sequence_complete) != V8_FRAMING_OK)
			return V8_CALLING_INVALID_ARGUMENT;
	}
	if (v8_fsk_transmit_sample(
		    &calling->fsk, calling->transmit_bit, transmit,
		    &bit_consumed) != V8_FSK_OK)
		return V8_CALLING_INVALID_ARGUMENT;
	if (bit_ready &&
	    v8_menu_detect_bit(
		    &calling->joint_menu_detector, received_bit, &remote_menu,
		    &confirmed) != V8_MENU_OK)
		return V8_CALLING_INVALID_ARGUMENT;
	if (confirmed) {
		enum v8_menu_result selection_result;

		calling->qc_enabled = false;
		if (v8_joint_menu_select(
			    &calling->local_menu, &remote_menu, &joint_menu) !=
		    V8_MENU_OK)
			return V8_CALLING_INVALID_ARGUMENT;
		selection_result = v8_menu_selected_modulation(
			&calling->local_menu, &joint_menu, &modulation);
		if (selection_result == V8_MENU_NO_COMMON_MODULATION)
			return fail(
				calling,
				V8_CALLING_NO_COMMON_MODULATION);
		if (selection_result != V8_MENU_OK)
			return V8_CALLING_INVALID_ARGUMENT;
		calling->negotiated_menu = joint_menu;
		calling->selected_modulation = modulation;
		calling->negotiated = true;
		enter_state(calling, V8_CALLING_SEND_CJ);
		return V8_CALLING_OK;
	}
	calling->state_samples++;
	if (calling->state_samples ==
	    V8_CALLING_MENU_TIMEOUT_SAMPLES)
		return fail(calling, V8_CALLING_MENU_TIMEOUT);
	return V8_CALLING_OK;
}

static enum v8_calling_result send_qc_sample(
	struct v8_calling *calling, int16_t receive, int16_t *transmit)
{
	uint8_t bit;
	bool ready, consumed;

	if (v8_fsk_can_accept_bit(&calling->fsk)) {
		if (calling->qc_bit_index == v92_qc1_bit_count(&calling->qc_request)) {
			/* Refusal needs fresh ANSam evidence after QC1d. */
			if (v8_ansam_detector_init(&calling->answer_signal) != V8_ANSAM_OK)
				return V8_CALLING_INVALID_ARGUMENT;
			enter_state(calling, V8_CALLING_EXCHANGE_MENUS);
			return exchange_menu_sample(calling, receive, transmit);
		}
		if (!v92_qc1_bit(&calling->qc_request, calling->qc_bit_index,
			      &calling->transmit_bit))
			return V8_CALLING_INVALID_ARGUMENT;
		calling->qc_bit_index++;
	}
	if (v8_fsk_receive_sample(&calling->fsk, receive, &bit, &ready) != V8_FSK_OK ||
	    v8_fsk_transmit_sample(&calling->fsk, calling->transmit_bit,
		      transmit, &consumed) != V8_FSK_OK)
		return V8_CALLING_INVALID_ARGUMENT;
	return V8_CALLING_OK;
}

static enum v8_calling_result send_cj_sample(
	struct v8_calling *calling, int16_t *transmit)
{
	bool bit_consumed;

	if (calling->cj_bit_index == V8_CJ_BITS &&
	    v8_fsk_can_accept_bit(&calling->fsk)) {
		if (v8_fsk_filter_transmit_sample(
			    &calling->fsk, 0, transmit) != V8_FSK_OK)
			return V8_CALLING_INVALID_ARGUMENT;
		calling->state_samples++;
		if (calling->state_samples ==
		    (calling->phase2_silence ? V8_FSK_TRANSMIT_FILTER_TAPS :
			V8_CALLING_CJ_TAIL_SAMPLES))
			enter_state(calling, V8_CALLING_COMPLETE);
		return V8_CALLING_OK;
	}
	if (v8_fsk_can_accept_bit(&calling->fsk) &&
	    calling->cj_bit_index < V8_CJ_BITS) {
		/* V.8 8.1.2: finish CM's current framed octet before CJ so an
		 * asynchronous peer cannot consume CJ as the remainder of CM. */
		if (calling->cj_bit_index == 0 &&
		    calling->call_menu_bits.bit_index % V8_FRAMED_OCTET_BITS != 0) {
			bool sequence_complete;

			if (v8_control_next_bit(
				    &calling->call_menu_bits, &calling->transmit_bit,
				    &sequence_complete) != V8_FRAMING_OK)
				return V8_CALLING_INVALID_ARGUMENT;
		} else {
			if (v8_cj_bit(
				    calling->cj_bit_index, &calling->transmit_bit) !=
			    V8_FRAMING_OK)
				return V8_CALLING_INVALID_ARGUMENT;
			calling->cj_bit_index++;
		}
	}
	if (v8_fsk_transmit_sample(
		    &calling->fsk, calling->transmit_bit, transmit,
		    &bit_consumed) != V8_FSK_OK)
		return V8_CALLING_INVALID_ARGUMENT;
	return V8_CALLING_OK;
}

enum v8_calling_result v8_calling_delegate_phase2_silence(
	struct v8_calling *calling)
{
	if (!calling_is_valid(calling) || calling->state != V8_CALLING_SEND_CJ ||
	    calling->state_samples != 0 ||
	    calling->selected_modulation != V8_MODULATION_V34_DUPLEX)
		return V8_CALLING_INVALID_ARGUMENT;
	calling->phase2_silence = true;
	return V8_CALLING_OK;
}

static enum v8_calling_result process_one(
	struct v8_calling *calling, int16_t receive,
	int16_t *transmit)
{
	if (calling->state == V8_CALLING_WAIT_ANSWER_SIGNAL) {
		bool detected;

		if (v8_ansam_detect(
			    &calling->answer_signal, &calling->fsk.line,
			    &receive, 1, &detected) !=
		    V8_ANSAM_OK)
			return V8_CALLING_INVALID_ARGUMENT;
		calling->state_samples++;
		if (detected) {
			enter_state(
				calling,
				calling->qc_enabled ? V8_CALLING_SEND_QC1 :
					V8_CALLING_EXCHANGE_MENUS);
		} else if (calling->state_samples ==
			   V8_CALLING_ANSWER_TIMEOUT_SAMPLES) {
			return fail(
				calling, V8_CALLING_ANSWER_TIMEOUT);
		}
		return V8_CALLING_OK;
	}
	if (calling->state == V8_CALLING_EXCHANGE_MENUS)
		return exchange_menu_sample(calling, receive, transmit);
	if (calling->state == V8_CALLING_SEND_CJ)
		return send_cj_sample(calling, transmit);
	if (calling->state == V8_CALLING_SEND_QC1)
		return send_qc_sample(calling, receive, transmit);
	if (calling->state == V8_CALLING_QC_TX_TAIL)
		return drain_qc_sample(calling, transmit);
	if (calling->state == V8_CALLING_COMPLETE ||
	    calling->state == V8_CALLING_QC_COMPLETE)
		return V8_CALLING_OK;
	if (calling->state == V8_CALLING_FAILED)
		return calling->terminal_result;
	return V8_CALLING_INVALID_ARGUMENT;
}

enum v8_calling_result v8_calling_process(
	struct v8_calling *calling, const int16_t *receive,
	int16_t *transmit, size_t count)
{
	struct v8_calling next;
	int16_t generated[V8_CALLING_MAX_FRAGMENT_SAMPLES] = { 0 };
	enum v8_calling_result result =
		V8_CALLING_OK;
	size_t processed = 0;

	if (!calling_is_valid(calling) ||
	    count > V8_CALLING_MAX_FRAGMENT_SAMPLES ||
	    ((receive == NULL || transmit == NULL) && count != 0))
		return V8_CALLING_INVALID_ARGUMENT;
	if (count == 0)
		return calling->terminal_result;
	next = *calling;
	for (; processed < count; ++processed) {
		result = process_one(
			&next, receive[processed], &generated[processed]);
		if (result != V8_CALLING_OK)
			break;
	}
	if (result == V8_CALLING_INVALID_ARGUMENT ||
	    !calling_is_valid(&next))
		return V8_CALLING_INVALID_ARGUMENT;
	if (result != V8_CALLING_OK)
		memset(
			&generated[processed], 0,
			(count - processed) * sizeof(generated[0]));
	*calling = next;
	memcpy(transmit, generated, count * sizeof(generated[0]));
	return result;
}

bool v8_calling_is_complete(
	const struct v8_calling *calling)
{
	return calling_is_valid(calling) &&
	       (calling->state == V8_CALLING_COMPLETE ||
		       calling->state == V8_CALLING_QC_COMPLETE);
}

enum v8_calling_result v8_calling_selection(
	const struct v8_calling *calling,
	struct v8_menu *menu,
	enum v8_modulation *modulation)
{
	if (!calling_is_valid(calling) || menu == NULL || modulation == NULL)
		return V8_CALLING_INVALID_ARGUMENT;
	if (!calling->negotiated)
		return V8_CALLING_NOT_READY;
	*menu = calling->negotiated_menu;
	*modulation = calling->selected_modulation;
	return V8_CALLING_OK;
}

const char *v8_calling_result_name(
	enum v8_calling_result result)
{
	switch (result) {
	case V8_CALLING_OK:
		return "ok";
	case V8_CALLING_INVALID_ARGUMENT:
		return "invalid argument";
	case V8_CALLING_NOT_READY:
		return "not ready";
	case V8_CALLING_ANSWER_TIMEOUT:
		return "answer signal timeout";
	case V8_CALLING_MENU_TIMEOUT:
		return "menu timeout";
	case V8_CALLING_NO_COMMON_MODULATION:
		return "no common modulation";
	}
	return "unknown V.8 calling result";
}
