// SPDX-License-Identifier: BSD-3-Clause

#include "v8_calling.h"

#include <string.h>

static bool menu_is_usable(const struct sippymodem_v8_menu *menu)
{
	uint8_t information[SIPPYMODEM_V8_MAX_INFORMATION_OCTETS];
	size_t information_count;

	return menu != NULL && menu->modulations != 0 &&
	       sippymodem_v8_menu_encode(
		       menu, information, sizeof(information),
		       &information_count) == SIPPYMODEM_V8_MENU_OK;
}

static bool calling_is_valid(const struct sippymodem_v8_calling *calling)
{
	unsigned int selected;
	bool selection_is_single;

	if (calling == NULL || !menu_is_usable(&calling->local_menu) ||
	    (unsigned int)calling->state > SIPPYMODEM_V8_CALLING_FAILED ||
	    (unsigned int)calling->terminal_result >
		    SIPPYMODEM_V8_CALLING_NO_COMMON_MODULATION ||
	    calling->transmit_bit > 1 ||
	    calling->cj_bit_index > SIPPYMODEM_V8_CJ_BITS)
		return false;
	if ((calling->state == SIPPYMODEM_V8_CALLING_FAILED) !=
	    (calling->terminal_result != SIPPYMODEM_V8_CALLING_OK))
		return false;
	selected = (unsigned int)calling->selected_modulation;
	selection_is_single =
		selected != 0 &&
		(selected & ~((unsigned int)SIPPYMODEM_V8_MODULATION_ALL)) == 0 &&
		(selected & (selected - 1u)) == 0;
	if (calling->negotiated) {
		if (!menu_is_usable(&calling->negotiated_menu) ||
		    !selection_is_single ||
		    (calling->negotiated_menu.modulations & selected) == 0 ||
		    (calling->negotiated_menu.modulations &
		     ~calling->local_menu.modulations) != 0 ||
		    (calling->negotiated_menu.lapm &&
		     !calling->local_menu.lapm))
			return false;
	} else if (calling->negotiated_menu.call_function != 0 ||
		   calling->negotiated_menu.modulations != 0 ||
		   calling->negotiated_menu.modulation_octets != 0 ||
		   calling->negotiated_menu.lapm || selected != 0 ||
		   calling->cj_bit_index != 0) {
		return false;
	}
	switch (calling->state) {
	case SIPPYMODEM_V8_CALLING_WAIT_ANSWER_SIGNAL:
		return !calling->negotiated && calling->state_samples <
		       SIPPYMODEM_V8_CALLING_ANSWER_TIMEOUT_SAMPLES;
	case SIPPYMODEM_V8_CALLING_ECHO_CONTROL_DWELL:
		return !calling->negotiated && calling->state_samples <
		       SIPPYMODEM_V8_ECHO_CONTROL_DWELL_SAMPLES;
	case SIPPYMODEM_V8_CALLING_EXCHANGE_MENUS:
		return !calling->negotiated && calling->state_samples <
		       SIPPYMODEM_V8_CALLING_MENU_TIMEOUT_SAMPLES;
	case SIPPYMODEM_V8_CALLING_SEND_CJ:
		return calling->negotiated && calling->state_samples == 0;
	case SIPPYMODEM_V8_CALLING_POST_CJ_SILENCE:
		return calling->negotiated &&
		       calling->cj_bit_index == SIPPYMODEM_V8_CJ_BITS &&
		       calling->state_samples <
			       SIPPYMODEM_V8_CALLING_POST_CJ_SILENCE_SAMPLES;
	case SIPPYMODEM_V8_CALLING_COMPLETE:
		return calling->negotiated &&
		       calling->cj_bit_index == SIPPYMODEM_V8_CJ_BITS &&
		       calling->state_samples == 0;
	case SIPPYMODEM_V8_CALLING_FAILED:
		return !calling->negotiated && calling->state_samples == 0 &&
		       (calling->terminal_result ==
				SIPPYMODEM_V8_CALLING_ANSWER_TIMEOUT ||
			calling->terminal_result ==
				SIPPYMODEM_V8_CALLING_MENU_TIMEOUT ||
			calling->terminal_result ==
				SIPPYMODEM_V8_CALLING_NO_COMMON_MODULATION);
	}
	return false;
}

static void enter_state(
	struct sippymodem_v8_calling *calling,
	enum sippymodem_v8_calling_state state)
{
	calling->state = state;
	calling->state_samples = 0;
}

enum sippymodem_v8_calling_result sippymodem_v8_calling_init(
	struct sippymodem_v8_calling *calling,
	const struct sippymodem_v8_menu *local_menu)
{
	struct sippymodem_v8_calling initialized = { 0 };
	uint8_t information[SIPPYMODEM_V8_MAX_INFORMATION_OCTETS];
	size_t information_count;

	if (calling == NULL || !menu_is_usable(local_menu) ||
	    sippymodem_v8_menu_encode(
		    local_menu, information, sizeof(information),
		    &information_count) != SIPPYMODEM_V8_MENU_OK ||
	    sippymodem_v8_ansam_detector_init(&initialized.answer_signal) !=
		    SIPPYMODEM_V8_ANSAM_OK ||
	    sippymodem_v8_fsk_init(
		    &initialized.fsk, SIPPYMODEM_V8_CALLING_STATION) !=
		    SIPPYMODEM_V8_FSK_OK ||
	    sippymodem_v8_control_generator_init(
		    &initialized.call_menu_bits, information,
		    information_count) != SIPPYMODEM_V8_FRAMING_OK ||
	    sippymodem_v8_menu_detector_init(
		    &initialized.joint_menu_detector) != SIPPYMODEM_V8_MENU_OK)
		return SIPPYMODEM_V8_CALLING_INVALID_ARGUMENT;
	initialized.local_menu = *local_menu;
	initialized.transmit_bit = 1;
	if (!calling_is_valid(&initialized))
		return SIPPYMODEM_V8_CALLING_INVALID_ARGUMENT;
	*calling = initialized;
	return SIPPYMODEM_V8_CALLING_OK;
}

static enum sippymodem_v8_calling_result fail(
	struct sippymodem_v8_calling *calling,
	enum sippymodem_v8_calling_result result)
{
	calling->state = SIPPYMODEM_V8_CALLING_FAILED;
	calling->terminal_result = result;
	calling->state_samples = 0;
	return result;
}

static enum sippymodem_v8_calling_result exchange_menu_sample(
	struct sippymodem_v8_calling *calling, int16_t receive,
	int16_t *transmit)
{
	struct sippymodem_v8_menu remote_menu;
	struct sippymodem_v8_menu joint_menu;
	enum sippymodem_v8_modulation modulation;
	uint8_t received_bit;
	bool bit_consumed;
	bool bit_ready;
	bool confirmed = false;

	if (sippymodem_v8_fsk_can_accept_bit(&calling->fsk)) {
		bool sequence_complete;

		if (sippymodem_v8_control_next_bit(
			    &calling->call_menu_bits, &calling->transmit_bit,
			    &sequence_complete) != SIPPYMODEM_V8_FRAMING_OK)
			return SIPPYMODEM_V8_CALLING_INVALID_ARGUMENT;
	}
	if (sippymodem_v8_fsk_transmit_sample(
		    &calling->fsk, calling->transmit_bit, transmit,
		    &bit_consumed) != SIPPYMODEM_V8_FSK_OK ||
	    sippymodem_v8_fsk_receive_sample(
		    &calling->fsk, receive, &received_bit, &bit_ready) !=
		    SIPPYMODEM_V8_FSK_OK)
		return SIPPYMODEM_V8_CALLING_INVALID_ARGUMENT;
	if (bit_ready &&
	    sippymodem_v8_menu_detect_bit(
		    &calling->joint_menu_detector, received_bit, &remote_menu,
		    &confirmed) != SIPPYMODEM_V8_MENU_OK)
		return SIPPYMODEM_V8_CALLING_INVALID_ARGUMENT;
	if (confirmed) {
		enum sippymodem_v8_menu_result selection_result;

		if (remote_menu.modulations == 0)
			return fail(
				calling,
				SIPPYMODEM_V8_CALLING_NO_COMMON_MODULATION);
		if (sippymodem_v8_joint_menu_select(
			    &remote_menu, &calling->local_menu, &joint_menu) !=
		    SIPPYMODEM_V8_MENU_OK)
			return SIPPYMODEM_V8_CALLING_INVALID_ARGUMENT;
		selection_result = sippymodem_v8_menu_selected_modulation(
			&joint_menu, &modulation);
		if (selection_result == SIPPYMODEM_V8_MENU_NO_COMMON_MODULATION)
			return fail(
				calling,
				SIPPYMODEM_V8_CALLING_NO_COMMON_MODULATION);
		if (selection_result != SIPPYMODEM_V8_MENU_OK)
			return SIPPYMODEM_V8_CALLING_INVALID_ARGUMENT;
		calling->negotiated_menu = joint_menu;
		calling->selected_modulation = modulation;
		calling->negotiated = true;
		enter_state(calling, SIPPYMODEM_V8_CALLING_SEND_CJ);
		return SIPPYMODEM_V8_CALLING_OK;
	}
	calling->state_samples++;
	if (calling->state_samples ==
	    SIPPYMODEM_V8_CALLING_MENU_TIMEOUT_SAMPLES)
		return fail(calling, SIPPYMODEM_V8_CALLING_MENU_TIMEOUT);
	return SIPPYMODEM_V8_CALLING_OK;
}

static enum sippymodem_v8_calling_result send_cj_sample(
	struct sippymodem_v8_calling *calling, int16_t *transmit)
{
	bool bit_consumed;

	if (sippymodem_v8_fsk_can_accept_bit(&calling->fsk) &&
	    calling->cj_bit_index < SIPPYMODEM_V8_CJ_BITS) {
		if (sippymodem_v8_cj_bit(
			    calling->cj_bit_index, &calling->transmit_bit) !=
		    SIPPYMODEM_V8_FRAMING_OK)
			return SIPPYMODEM_V8_CALLING_INVALID_ARGUMENT;
		calling->cj_bit_index++;
	}
	if (sippymodem_v8_fsk_transmit_sample(
		    &calling->fsk, calling->transmit_bit, transmit,
		    &bit_consumed) != SIPPYMODEM_V8_FSK_OK)
		return SIPPYMODEM_V8_CALLING_INVALID_ARGUMENT;
	if (bit_consumed &&
	    calling->cj_bit_index == SIPPYMODEM_V8_CJ_BITS)
		enter_state(calling, SIPPYMODEM_V8_CALLING_POST_CJ_SILENCE);
	return SIPPYMODEM_V8_CALLING_OK;
}

static enum sippymodem_v8_calling_result process_one(
	struct sippymodem_v8_calling *calling, int16_t receive,
	int16_t *transmit)
{
	if (calling->state == SIPPYMODEM_V8_CALLING_WAIT_ANSWER_SIGNAL) {
		bool detected;

		if (sippymodem_v8_ansam_detect(
			    &calling->answer_signal, &receive, 1, &detected) !=
		    SIPPYMODEM_V8_ANSAM_OK)
			return SIPPYMODEM_V8_CALLING_INVALID_ARGUMENT;
		calling->state_samples++;
		if (detected) {
			enter_state(
				calling,
				SIPPYMODEM_V8_CALLING_ECHO_CONTROL_DWELL);
		} else if (calling->state_samples ==
			   SIPPYMODEM_V8_CALLING_ANSWER_TIMEOUT_SAMPLES) {
			return fail(
				calling, SIPPYMODEM_V8_CALLING_ANSWER_TIMEOUT);
		}
		return SIPPYMODEM_V8_CALLING_OK;
	}
	if (calling->state == SIPPYMODEM_V8_CALLING_ECHO_CONTROL_DWELL) {
		calling->state_samples++;
		if (calling->state_samples ==
		    SIPPYMODEM_V8_ECHO_CONTROL_DWELL_SAMPLES)
			enter_state(
				calling,
				SIPPYMODEM_V8_CALLING_EXCHANGE_MENUS);
		return SIPPYMODEM_V8_CALLING_OK;
	}
	if (calling->state == SIPPYMODEM_V8_CALLING_EXCHANGE_MENUS)
		return exchange_menu_sample(calling, receive, transmit);
	if (calling->state == SIPPYMODEM_V8_CALLING_SEND_CJ)
		return send_cj_sample(calling, transmit);
	if (calling->state == SIPPYMODEM_V8_CALLING_POST_CJ_SILENCE) {
		calling->state_samples++;
		if (calling->state_samples ==
		    SIPPYMODEM_V8_CALLING_POST_CJ_SILENCE_SAMPLES)
			enter_state(calling, SIPPYMODEM_V8_CALLING_COMPLETE);
		return SIPPYMODEM_V8_CALLING_OK;
	}
	if (calling->state == SIPPYMODEM_V8_CALLING_COMPLETE)
		return SIPPYMODEM_V8_CALLING_OK;
	if (calling->state == SIPPYMODEM_V8_CALLING_FAILED)
		return calling->terminal_result;
	return SIPPYMODEM_V8_CALLING_INVALID_ARGUMENT;
}

enum sippymodem_v8_calling_result sippymodem_v8_calling_process(
	struct sippymodem_v8_calling *calling, const int16_t *receive,
	int16_t *transmit, size_t count)
{
	struct sippymodem_v8_calling next;
	int16_t generated[SIPPYMODEM_V8_CALLING_MAX_FRAGMENT_SAMPLES] = { 0 };
	enum sippymodem_v8_calling_result result =
		SIPPYMODEM_V8_CALLING_OK;
	size_t processed = 0;

	if (!calling_is_valid(calling) ||
	    count > SIPPYMODEM_V8_CALLING_MAX_FRAGMENT_SAMPLES ||
	    ((receive == NULL || transmit == NULL) && count != 0))
		return SIPPYMODEM_V8_CALLING_INVALID_ARGUMENT;
	if (count == 0)
		return calling->terminal_result;
	next = *calling;
	for (; processed < count; ++processed) {
		result = process_one(
			&next, receive[processed], &generated[processed]);
		if (result != SIPPYMODEM_V8_CALLING_OK)
			break;
	}
	if (result == SIPPYMODEM_V8_CALLING_INVALID_ARGUMENT ||
	    !calling_is_valid(&next))
		return SIPPYMODEM_V8_CALLING_INVALID_ARGUMENT;
	if (result != SIPPYMODEM_V8_CALLING_OK)
		memset(
			&generated[processed], 0,
			(count - processed) * sizeof(generated[0]));
	*calling = next;
	memcpy(transmit, generated, count * sizeof(generated[0]));
	return result;
}

bool sippymodem_v8_calling_is_complete(
	const struct sippymodem_v8_calling *calling)
{
	return calling_is_valid(calling) &&
	       calling->state == SIPPYMODEM_V8_CALLING_COMPLETE;
}

enum sippymodem_v8_calling_result sippymodem_v8_calling_selection(
	const struct sippymodem_v8_calling *calling,
	struct sippymodem_v8_menu *menu,
	enum sippymodem_v8_modulation *modulation)
{
	if (!calling_is_valid(calling) || menu == NULL || modulation == NULL)
		return SIPPYMODEM_V8_CALLING_INVALID_ARGUMENT;
	if (!calling->negotiated)
		return SIPPYMODEM_V8_CALLING_NOT_READY;
	*menu = calling->negotiated_menu;
	*modulation = calling->selected_modulation;
	return SIPPYMODEM_V8_CALLING_OK;
}

const char *sippymodem_v8_calling_result_name(
	enum sippymodem_v8_calling_result result)
{
	switch (result) {
	case SIPPYMODEM_V8_CALLING_OK:
		return "ok";
	case SIPPYMODEM_V8_CALLING_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V8_CALLING_NOT_READY:
		return "not ready";
	case SIPPYMODEM_V8_CALLING_ANSWER_TIMEOUT:
		return "answer signal timeout";
	case SIPPYMODEM_V8_CALLING_MENU_TIMEOUT:
		return "menu timeout";
	case SIPPYMODEM_V8_CALLING_NO_COMMON_MODULATION:
		return "no common modulation";
	}
	return "unknown V.8 calling result";
}
