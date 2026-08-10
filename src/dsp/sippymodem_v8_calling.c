// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v8_calling.h"

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
	if (calling == NULL || !menu_is_usable(&calling->local_menu) ||
	    calling->state > SIPPYMODEM_V8_CALLING_FAILED ||
	    calling->terminal_result > SIPPYMODEM_V8_CALLING_ANSWER_TIMEOUT)
		return false;
	if ((calling->state == SIPPYMODEM_V8_CALLING_FAILED) !=
	    (calling->terminal_result != SIPPYMODEM_V8_CALLING_OK))
		return false;
	switch (calling->state) {
	case SIPPYMODEM_V8_CALLING_WAIT_ANSWER_SIGNAL:
		return calling->state_samples <
		       SIPPYMODEM_V8_CALLING_ANSWER_TIMEOUT_SAMPLES;
	case SIPPYMODEM_V8_CALLING_ECHO_CONTROL_DWELL:
		return calling->state_samples <
		       SIPPYMODEM_V8_ECHO_CONTROL_DWELL_SAMPLES;
	case SIPPYMODEM_V8_CALLING_READY_TO_EXCHANGE:
		return calling->state_samples == 0;
	case SIPPYMODEM_V8_CALLING_FAILED:
		return calling->terminal_result ==
		       SIPPYMODEM_V8_CALLING_ANSWER_TIMEOUT;
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

	if (calling == NULL || !menu_is_usable(local_menu) ||
	    sippymodem_v8_ansam_detector_init(&initialized.answer_signal) !=
		    SIPPYMODEM_V8_ANSAM_OK)
		return SIPPYMODEM_V8_CALLING_INVALID_ARGUMENT;
	initialized.local_menu = *local_menu;
	if (!calling_is_valid(&initialized))
		return SIPPYMODEM_V8_CALLING_INVALID_ARGUMENT;
	*calling = initialized;
	return SIPPYMODEM_V8_CALLING_OK;
}

static enum sippymodem_v8_calling_result process_one(
	struct sippymodem_v8_calling *calling, int16_t receive)
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
			calling->state = SIPPYMODEM_V8_CALLING_FAILED;
			calling->terminal_result =
				SIPPYMODEM_V8_CALLING_ANSWER_TIMEOUT;
			calling->state_samples = 0;
			return calling->terminal_result;
		}
		return SIPPYMODEM_V8_CALLING_OK;
	}
	if (calling->state == SIPPYMODEM_V8_CALLING_ECHO_CONTROL_DWELL) {
		calling->state_samples++;
		if (calling->state_samples ==
		    SIPPYMODEM_V8_ECHO_CONTROL_DWELL_SAMPLES)
			enter_state(
				calling,
				SIPPYMODEM_V8_CALLING_READY_TO_EXCHANGE);
		return SIPPYMODEM_V8_CALLING_OK;
	}
	if (calling->state == SIPPYMODEM_V8_CALLING_READY_TO_EXCHANGE)
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
		result = process_one(&next, receive[processed]);
		if (result != SIPPYMODEM_V8_CALLING_OK)
			break;
	}
	if (result == SIPPYMODEM_V8_CALLING_INVALID_ARGUMENT ||
	    !calling_is_valid(&next))
		return SIPPYMODEM_V8_CALLING_INVALID_ARGUMENT;
	*calling = next;
	memcpy(transmit, generated, count * sizeof(generated[0]));
	return result;
}

bool sippymodem_v8_calling_is_ready(
	const struct sippymodem_v8_calling *calling)
{
	return calling_is_valid(calling) &&
	       calling->state == SIPPYMODEM_V8_CALLING_READY_TO_EXCHANGE;
}

const char *sippymodem_v8_calling_result_name(
	enum sippymodem_v8_calling_result result)
{
	switch (result) {
	case SIPPYMODEM_V8_CALLING_OK:
		return "ok";
	case SIPPYMODEM_V8_CALLING_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V8_CALLING_ANSWER_TIMEOUT:
		return "answer signal timeout";
	}
	return "unknown V.8 calling result";
}
