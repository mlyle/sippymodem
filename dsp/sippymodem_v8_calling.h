// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V8_CALLING_H
#define SIPPYMODEM_V8_CALLING_H

#include "sippymodem_v8_ansam.h"
#include "sippymodem_v8_fsk.h"
#include "sippymodem_v8_menu.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V8_CALLING_MAX_FRAGMENT_SAMPLES = 160,
	SIPPYMODEM_V8_CALLING_ANSWER_TIMEOUT_SAMPLES =
		12 * SIPPYMODEM_V8_ANSAM_SAMPLE_RATE,
	SIPPYMODEM_V8_ECHO_CONTROL_DWELL_SAMPLES =
		SIPPYMODEM_V8_ANSAM_SAMPLE_RATE,
	SIPPYMODEM_V8_CALLING_MENU_TIMEOUT_SAMPLES =
		7 * SIPPYMODEM_V8_ANSAM_SAMPLE_RATE,
	SIPPYMODEM_V8_CALLING_POST_CJ_SILENCE_SAMPLES =
		SIPPYMODEM_V8_ANSAM_SAMPLE_RATE * 75 / 1000,
};

enum sippymodem_v8_calling_state {
	SIPPYMODEM_V8_CALLING_WAIT_ANSWER_SIGNAL,
	SIPPYMODEM_V8_CALLING_ECHO_CONTROL_DWELL,
	SIPPYMODEM_V8_CALLING_EXCHANGE_MENUS,
	SIPPYMODEM_V8_CALLING_SEND_CJ,
	SIPPYMODEM_V8_CALLING_POST_CJ_SILENCE,
	SIPPYMODEM_V8_CALLING_COMPLETE,
	SIPPYMODEM_V8_CALLING_FAILED,
};

enum sippymodem_v8_calling_result {
	SIPPYMODEM_V8_CALLING_OK,
	SIPPYMODEM_V8_CALLING_INVALID_ARGUMENT,
	SIPPYMODEM_V8_CALLING_NOT_READY,
	SIPPYMODEM_V8_CALLING_ANSWER_TIMEOUT,
	SIPPYMODEM_V8_CALLING_MENU_TIMEOUT,
	SIPPYMODEM_V8_CALLING_NO_COMMON_MODULATION,
};

struct sippymodem_v8_calling {
	struct sippymodem_v8_ansam_detector answer_signal;
	struct sippymodem_v8_fsk fsk;
	struct sippymodem_v8_control_generator call_menu_bits;
	struct sippymodem_v8_menu_detector joint_menu_detector;
	struct sippymodem_v8_menu local_menu;
	struct sippymodem_v8_menu negotiated_menu;
	enum sippymodem_v8_modulation selected_modulation;
	enum sippymodem_v8_calling_state state;
	enum sippymodem_v8_calling_result terminal_result;
	uint32_t state_samples;
	size_t cj_bit_index;
	uint8_t transmit_bit;
	bool negotiated;
};

enum sippymodem_v8_calling_result sippymodem_v8_calling_init(
	struct sippymodem_v8_calling *calling,
	const struct sippymodem_v8_menu *local_menu);
enum sippymodem_v8_calling_result sippymodem_v8_calling_process(
	struct sippymodem_v8_calling *calling, const int16_t *receive,
	int16_t *transmit, size_t count);
bool sippymodem_v8_calling_is_complete(
	const struct sippymodem_v8_calling *calling);
enum sippymodem_v8_calling_result sippymodem_v8_calling_selection(
	const struct sippymodem_v8_calling *calling,
	struct sippymodem_v8_menu *menu,
	enum sippymodem_v8_modulation *modulation);

const char *sippymodem_v8_calling_result_name(
	enum sippymodem_v8_calling_result result);

#endif
