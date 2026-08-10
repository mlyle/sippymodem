// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v8_answering.h"

#include <string.h>

enum {
	V8_ANSWER_SIGNAL_AVERAGE_AMPLITUDE = 10000,
};

static bool menu_is_usable(const struct sippymodem_v8_menu *menu)
{
	uint8_t information[SIPPYMODEM_V8_MAX_INFORMATION_OCTETS];
	size_t information_count;

	return menu != NULL && menu->modulations != 0 &&
	       sippymodem_v8_menu_encode(
		       menu, information, sizeof(information),
		       &information_count) == SIPPYMODEM_V8_MENU_OK;
}

static bool selection_is_valid(
	const struct sippymodem_v8_answering *answering)
{
	unsigned int selected = (unsigned int)answering->selected_modulation;

	return menu_is_usable(&answering->call_menu) &&
	       menu_is_usable(&answering->negotiated_menu) && selected != 0 &&
	       (selected & ~((unsigned int)SIPPYMODEM_V8_MODULATION_ALL)) == 0 &&
	       (selected & (selected - 1u)) == 0 &&
	       (answering->negotiated_menu.modulations & selected) != 0 &&
	       (answering->negotiated_menu.modulations &
		~answering->local_menu.modulations) == 0 &&
	       (answering->negotiated_menu.modulations &
		~answering->call_menu.modulations) == 0 &&
	       (!answering->negotiated_menu.lapm ||
		(answering->local_menu.lapm && answering->call_menu.lapm));
}

static bool answering_is_valid(
	const struct sippymodem_v8_answering *answering)
{
	if (answering == NULL || !menu_is_usable(&answering->local_menu) ||
	    (unsigned int)answering->state > SIPPYMODEM_V8_ANSWERING_FAILED ||
	    (unsigned int)answering->terminal_result >
		    SIPPYMODEM_V8_ANSWERING_NO_COMMON_MODULATION ||
	    answering->transmit_bit > 1 ||
	    (answering->state == SIPPYMODEM_V8_ANSWERING_FAILED) !=
		    (answering->terminal_result != SIPPYMODEM_V8_ANSWERING_OK))
		return false;
	if (answering->negotiated) {
		if (!selection_is_valid(answering))
			return false;
	} else if (answering->call_menu.call_function != 0 ||
		   answering->call_menu.modulations != 0 ||
		   answering->call_menu.modulation_octets != 0 ||
		   answering->call_menu.lapm ||
		   answering->negotiated_menu.call_function != 0 ||
		   answering->negotiated_menu.modulations != 0 ||
		   answering->negotiated_menu.modulation_octets != 0 ||
		   answering->negotiated_menu.lapm ||
		   answering->selected_modulation != 0) {
		return false;
	}
	switch (answering->state) {
	case SIPPYMODEM_V8_ANSWERING_WAIT_CALL_MENU:
		return !answering->negotiated && answering->state_samples <
		       SIPPYMODEM_V8_ANSWERING_CALL_MENU_TIMEOUT_SAMPLES;
	case SIPPYMODEM_V8_ANSWERING_SEND_JOINT_MENU:
		return answering->negotiated && answering->state_samples <
		       SIPPYMODEM_V8_ANSWERING_CJ_TIMEOUT_SAMPLES;
	case SIPPYMODEM_V8_ANSWERING_POST_CJ_SILENCE:
		return answering->negotiated && answering->state_samples <
		       SIPPYMODEM_V8_ANSWERING_POST_CJ_SILENCE_SAMPLES;
	case SIPPYMODEM_V8_ANSWERING_COMPLETE:
		return answering->negotiated && answering->state_samples == 0;
	case SIPPYMODEM_V8_ANSWERING_FAILED:
		return answering->state_samples == 0 &&
		       ((answering->terminal_result ==
				 SIPPYMODEM_V8_ANSWERING_CJ_TIMEOUT &&
			 answering->negotiated) ||
			(!answering->negotiated &&
			 (answering->terminal_result ==
				  SIPPYMODEM_V8_ANSWERING_CALL_MENU_TIMEOUT ||
			  answering->terminal_result ==
				  SIPPYMODEM_V8_ANSWERING_NO_COMMON_MODULATION)));
	}
	return false;
}

static void enter_state(
	struct sippymodem_v8_answering *answering,
	enum sippymodem_v8_answering_state state)
{
	answering->state = state;
	answering->state_samples = 0;
}

static enum sippymodem_v8_answering_result fail(
	struct sippymodem_v8_answering *answering,
	enum sippymodem_v8_answering_result result)
{
	answering->state = SIPPYMODEM_V8_ANSWERING_FAILED;
	answering->terminal_result = result;
	answering->state_samples = 0;
	return result;
}

enum sippymodem_v8_answering_result sippymodem_v8_answering_init(
	struct sippymodem_v8_answering *answering,
	const struct sippymodem_v8_menu *local_menu)
{
	struct sippymodem_v8_answering initialized = {
		.transmit_bit = 1,
	};

	if (answering == NULL || !menu_is_usable(local_menu) ||
	    sippymodem_v8_ansam_generator_init(
		    &initialized.answer_signal,
		    V8_ANSWER_SIGNAL_AVERAGE_AMPLITUDE, true) !=
		    SIPPYMODEM_V8_ANSAM_OK ||
	    sippymodem_v8_fsk_init(
		    &initialized.fsk, SIPPYMODEM_V8_ANSWERING_STATION) !=
		    SIPPYMODEM_V8_FSK_OK ||
	    sippymodem_v8_menu_detector_init(
		    &initialized.call_menu_detector) != SIPPYMODEM_V8_MENU_OK ||
	    sippymodem_v8_cj_detector_init(&initialized.cj_detector) !=
		    SIPPYMODEM_V8_FRAMING_OK)
		return SIPPYMODEM_V8_ANSWERING_INVALID_ARGUMENT;
	initialized.local_menu = *local_menu;
	if (!answering_is_valid(&initialized))
		return SIPPYMODEM_V8_ANSWERING_INVALID_ARGUMENT;
	*answering = initialized;
	return SIPPYMODEM_V8_ANSWERING_OK;
}

static enum sippymodem_v8_answering_result select_joint_menu(
	struct sippymodem_v8_answering *answering,
	const struct sippymodem_v8_menu *call_menu)
{
	struct sippymodem_v8_menu joint_menu;
	enum sippymodem_v8_modulation modulation;
	enum sippymodem_v8_menu_result selection_result;
	uint8_t information[SIPPYMODEM_V8_MAX_INFORMATION_OCTETS];
	size_t information_count;

	if (sippymodem_v8_joint_menu_select(
		    call_menu, &answering->local_menu, &joint_menu) !=
	    SIPPYMODEM_V8_MENU_OK)
		return SIPPYMODEM_V8_ANSWERING_INVALID_ARGUMENT;
	selection_result = sippymodem_v8_menu_selected_modulation(
		&joint_menu, &modulation);
	if (selection_result == SIPPYMODEM_V8_MENU_NO_COMMON_MODULATION)
		return fail(
			answering, SIPPYMODEM_V8_ANSWERING_NO_COMMON_MODULATION);
	if (selection_result != SIPPYMODEM_V8_MENU_OK ||
	    sippymodem_v8_menu_encode(
		    &joint_menu, information, sizeof(information),
		    &information_count) != SIPPYMODEM_V8_MENU_OK ||
	    sippymodem_v8_control_generator_init(
		    &answering->joint_menu_bits, information,
		    information_count) != SIPPYMODEM_V8_FRAMING_OK)
		return SIPPYMODEM_V8_ANSWERING_INVALID_ARGUMENT;
	answering->call_menu = *call_menu;
	answering->negotiated_menu = joint_menu;
	answering->selected_modulation = modulation;
	answering->negotiated = true;
	enter_state(answering, SIPPYMODEM_V8_ANSWERING_SEND_JOINT_MENU);
	return SIPPYMODEM_V8_ANSWERING_OK;
}

static enum sippymodem_v8_answering_result wait_call_menu_sample(
	struct sippymodem_v8_answering *answering, int16_t receive,
	int16_t *transmit)
{
	struct sippymodem_v8_menu call_menu;
	uint8_t bit;
	bool ready;
	bool confirmed = false;

	if (sippymodem_v8_ansam_generate(
		    &answering->answer_signal, transmit, 1) !=
		    SIPPYMODEM_V8_ANSAM_OK ||
	    sippymodem_v8_fsk_receive_sample(
		    &answering->fsk, receive, &bit, &ready) !=
		    SIPPYMODEM_V8_FSK_OK)
		return SIPPYMODEM_V8_ANSWERING_INVALID_ARGUMENT;
	if (ready &&
	    sippymodem_v8_menu_detect_bit(
		    &answering->call_menu_detector, bit, &call_menu,
		    &confirmed) != SIPPYMODEM_V8_MENU_OK)
		return SIPPYMODEM_V8_ANSWERING_INVALID_ARGUMENT;
	if (confirmed)
		return select_joint_menu(answering, &call_menu);
	answering->state_samples++;
	if (answering->state_samples ==
	    SIPPYMODEM_V8_ANSWERING_CALL_MENU_TIMEOUT_SAMPLES)
		return fail(
			answering, SIPPYMODEM_V8_ANSWERING_CALL_MENU_TIMEOUT);
	return SIPPYMODEM_V8_ANSWERING_OK;
}

static enum sippymodem_v8_answering_result send_joint_menu_sample(
	struct sippymodem_v8_answering *answering, int16_t receive,
	int16_t *transmit)
{
	uint8_t bit;
	bool bit_consumed;
	bool bit_ready;
	bool cj_detected = false;

	if (sippymodem_v8_fsk_can_accept_bit(&answering->fsk)) {
		bool sequence_complete;

		if (sippymodem_v8_control_next_bit(
			    &answering->joint_menu_bits, &answering->transmit_bit,
			    &sequence_complete) != SIPPYMODEM_V8_FRAMING_OK)
			return SIPPYMODEM_V8_ANSWERING_INVALID_ARGUMENT;
	}
	if (sippymodem_v8_fsk_transmit_sample(
		    &answering->fsk, answering->transmit_bit, transmit,
		    &bit_consumed) != SIPPYMODEM_V8_FSK_OK ||
	    sippymodem_v8_fsk_receive_sample(
		    &answering->fsk, receive, &bit, &bit_ready) !=
		    SIPPYMODEM_V8_FSK_OK)
		return SIPPYMODEM_V8_ANSWERING_INVALID_ARGUMENT;
	if (bit_ready &&
	    sippymodem_v8_cj_detect_bit(
		    &answering->cj_detector, bit, &cj_detected) !=
		    SIPPYMODEM_V8_FRAMING_OK)
		return SIPPYMODEM_V8_ANSWERING_INVALID_ARGUMENT;
	if (cj_detected) {
		enter_state(answering, SIPPYMODEM_V8_ANSWERING_POST_CJ_SILENCE);
		return SIPPYMODEM_V8_ANSWERING_OK;
	}
	answering->state_samples++;
	if (answering->state_samples ==
	    SIPPYMODEM_V8_ANSWERING_CJ_TIMEOUT_SAMPLES)
		return fail(answering, SIPPYMODEM_V8_ANSWERING_CJ_TIMEOUT);
	return SIPPYMODEM_V8_ANSWERING_OK;
}

static enum sippymodem_v8_answering_result process_one(
	struct sippymodem_v8_answering *answering, int16_t receive,
	int16_t *transmit)
{
	if (answering->state == SIPPYMODEM_V8_ANSWERING_WAIT_CALL_MENU)
		return wait_call_menu_sample(answering, receive, transmit);
	if (answering->state == SIPPYMODEM_V8_ANSWERING_SEND_JOINT_MENU)
		return send_joint_menu_sample(answering, receive, transmit);
	if (answering->state == SIPPYMODEM_V8_ANSWERING_POST_CJ_SILENCE) {
		answering->state_samples++;
		if (answering->state_samples ==
		    SIPPYMODEM_V8_ANSWERING_POST_CJ_SILENCE_SAMPLES)
			enter_state(answering, SIPPYMODEM_V8_ANSWERING_COMPLETE);
		return SIPPYMODEM_V8_ANSWERING_OK;
	}
	if (answering->state == SIPPYMODEM_V8_ANSWERING_COMPLETE)
		return SIPPYMODEM_V8_ANSWERING_OK;
	if (answering->state == SIPPYMODEM_V8_ANSWERING_FAILED)
		return answering->terminal_result;
	return SIPPYMODEM_V8_ANSWERING_INVALID_ARGUMENT;
}

enum sippymodem_v8_answering_result sippymodem_v8_answering_process(
	struct sippymodem_v8_answering *answering, const int16_t *receive,
	int16_t *transmit, size_t count)
{
	struct sippymodem_v8_answering next;
	int16_t generated[SIPPYMODEM_V8_ANSWERING_MAX_FRAGMENT_SAMPLES] = { 0 };
	enum sippymodem_v8_answering_result result =
		SIPPYMODEM_V8_ANSWERING_OK;
	size_t processed = 0;

	if (!answering_is_valid(answering) ||
	    count > SIPPYMODEM_V8_ANSWERING_MAX_FRAGMENT_SAMPLES ||
	    ((receive == NULL || transmit == NULL) && count != 0))
		return SIPPYMODEM_V8_ANSWERING_INVALID_ARGUMENT;
	if (count == 0)
		return answering->terminal_result;
	next = *answering;
	for (; processed < count; ++processed) {
		result = process_one(
			&next, receive[processed], &generated[processed]);
		if (result != SIPPYMODEM_V8_ANSWERING_OK)
			break;
	}
	if (result == SIPPYMODEM_V8_ANSWERING_INVALID_ARGUMENT ||
	    !answering_is_valid(&next))
		return SIPPYMODEM_V8_ANSWERING_INVALID_ARGUMENT;
	if (result != SIPPYMODEM_V8_ANSWERING_OK)
		memset(
			&generated[processed], 0,
			(count - processed) * sizeof(generated[0]));
	*answering = next;
	memcpy(transmit, generated, count * sizeof(generated[0]));
	return result;
}

bool sippymodem_v8_answering_is_complete(
	const struct sippymodem_v8_answering *answering)
{
	return answering_is_valid(answering) &&
	       answering->state == SIPPYMODEM_V8_ANSWERING_COMPLETE;
}

enum sippymodem_v8_answering_result sippymodem_v8_answering_selection(
	const struct sippymodem_v8_answering *answering,
	struct sippymodem_v8_menu *menu,
	enum sippymodem_v8_modulation *modulation)
{
	if (!answering_is_valid(answering) || menu == NULL || modulation == NULL)
		return SIPPYMODEM_V8_ANSWERING_INVALID_ARGUMENT;
	if (!answering->negotiated)
		return SIPPYMODEM_V8_ANSWERING_NOT_READY;
	*menu = answering->negotiated_menu;
	*modulation = answering->selected_modulation;
	return SIPPYMODEM_V8_ANSWERING_OK;
}

const char *sippymodem_v8_answering_result_name(
	enum sippymodem_v8_answering_result result)
{
	switch (result) {
	case SIPPYMODEM_V8_ANSWERING_OK:
		return "ok";
	case SIPPYMODEM_V8_ANSWERING_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V8_ANSWERING_NOT_READY:
		return "not ready";
	case SIPPYMODEM_V8_ANSWERING_CALL_MENU_TIMEOUT:
		return "call menu timeout";
	case SIPPYMODEM_V8_ANSWERING_CJ_TIMEOUT:
		return "CJ timeout";
	case SIPPYMODEM_V8_ANSWERING_NO_COMMON_MODULATION:
		return "no common modulation";
	}
	return "unknown V.8 answering result";
}
