// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V8_ANSWERING_H
#define SIPPYMODEM_V8_ANSWERING_H

#include "sippymodem_v8_ansam.h"
#include "sippymodem_v8_fsk.h"
#include "sippymodem_v8_menu.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V8_ANSWERING_MAX_FRAGMENT_SAMPLES = 160,
	SIPPYMODEM_V8_ANSWERING_CALL_MENU_TIMEOUT_SAMPLES =
		12 * SIPPYMODEM_V8_ANSAM_SAMPLE_RATE,
	SIPPYMODEM_V8_ANSWERING_CJ_TIMEOUT_SAMPLES =
		7 * SIPPYMODEM_V8_ANSAM_SAMPLE_RATE,
	SIPPYMODEM_V8_ANSWERING_POST_CJ_SILENCE_SAMPLES =
		SIPPYMODEM_V8_ANSAM_SAMPLE_RATE * 75 / 1000,
};

enum sippymodem_v8_answering_state {
	SIPPYMODEM_V8_ANSWERING_WAIT_CALL_MENU,
	SIPPYMODEM_V8_ANSWERING_SEND_JOINT_MENU,
	SIPPYMODEM_V8_ANSWERING_POST_CJ_SILENCE,
	SIPPYMODEM_V8_ANSWERING_COMPLETE,
	SIPPYMODEM_V8_ANSWERING_FAILED,
};

enum sippymodem_v8_answering_result {
	SIPPYMODEM_V8_ANSWERING_OK,
	SIPPYMODEM_V8_ANSWERING_INVALID_ARGUMENT,
	SIPPYMODEM_V8_ANSWERING_NOT_READY,
	SIPPYMODEM_V8_ANSWERING_CALL_MENU_TIMEOUT,
	SIPPYMODEM_V8_ANSWERING_CJ_TIMEOUT,
	SIPPYMODEM_V8_ANSWERING_NO_COMMON_MODULATION,
};

struct sippymodem_v8_answering {
	struct sippymodem_v8_ansam_generator answer_signal;
	struct sippymodem_v8_fsk fsk;
	struct sippymodem_v8_menu_detector call_menu_detector;
	struct sippymodem_v8_control_generator joint_menu_bits;
	struct sippymodem_v8_cj_detector cj_detector;
	struct sippymodem_v8_menu local_menu;
	struct sippymodem_v8_menu call_menu;
	struct sippymodem_v8_menu negotiated_menu;
	enum sippymodem_v8_modulation selected_modulation;
	enum sippymodem_v8_answering_state state;
	enum sippymodem_v8_answering_result terminal_result;
	uint32_t state_samples;
	uint8_t transmit_bit;
	bool negotiated;
};

enum sippymodem_v8_answering_result sippymodem_v8_answering_init(
	struct sippymodem_v8_answering *answering,
	const struct sippymodem_v8_menu *local_menu);
enum sippymodem_v8_answering_result sippymodem_v8_answering_process(
	struct sippymodem_v8_answering *answering, const int16_t *receive,
	int16_t *transmit, size_t count);
bool sippymodem_v8_answering_is_complete(
	const struct sippymodem_v8_answering *answering);
enum sippymodem_v8_answering_result sippymodem_v8_answering_selection(
	const struct sippymodem_v8_answering *answering,
	struct sippymodem_v8_menu *menu,
	enum sippymodem_v8_modulation *modulation);

const char *sippymodem_v8_answering_result_name(
	enum sippymodem_v8_answering_result result);

#endif
