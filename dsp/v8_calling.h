// SPDX-License-Identifier: BSD-3-Clause

#ifndef V8_CALLING_H
#define V8_CALLING_H

#include "v8_ansam.h"
#include "v8_fsk.h"
#include "v8_menu.h"
#include "v92_qc1.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V8_CALLING_MAX_FRAGMENT_SAMPLES = 160,
	V8_CALLING_ANSWER_TIMEOUT_SAMPLES =
		12 * V8_ANSAM_SAMPLE_RATE,
	V8_CALLING_MENU_TIMEOUT_SAMPLES =
		7 * V8_ANSAM_SAMPLE_RATE,
	V8_CALLING_CJ_FILTER_DRAIN_SAMPLES = V8_FSK_TRANSMIT_FILTER_TAPS,
};

enum v8_calling_state {
	V8_CALLING_WAIT_ANSWER_SIGNAL,
	V8_CALLING_EXCHANGE_MENUS,
	V8_CALLING_SEND_CJ,
	V8_CALLING_SEND_QC1,
	V8_CALLING_QC_TX_TAIL,
	V8_CALLING_QC_COMPLETE,
	V8_CALLING_COMPLETE,
	V8_CALLING_FAILED,
};

enum v8_calling_result {
	V8_CALLING_OK,
	V8_CALLING_INVALID_ARGUMENT,
	V8_CALLING_NOT_READY,
	V8_CALLING_ANSWER_TIMEOUT,
	V8_CALLING_MENU_TIMEOUT,
	V8_CALLING_NO_COMMON_MODULATION,
};

struct v8_calling {
	struct v8_ansam_detector answer_signal;
	struct v8_fsk fsk;
	struct v8_control_generator call_menu_bits;
	struct v8_menu_detector joint_menu_detector;
	struct v8_menu local_menu;
	struct v8_menu negotiated_menu;
	struct v92_qc1_receiver qc_receiver;
	struct v92_qc1_message qc_request;
	struct v92_qc1_message qc_response;
	enum v8_modulation selected_modulation;
	enum v8_calling_state state;
	enum v8_calling_result terminal_result;
	uint32_t state_samples;
	size_t cj_bit_index;
	size_t qc_bit_index;
	uint8_t transmit_bit;
	bool negotiated;
	bool qc_enabled;
};

enum v8_calling_result v8_calling_init(
	struct v8_calling *calling,
	const struct v8_menu *local_menu);
/* Opt in before processing samples with role-local signal parameters.
 * Short Phase 2 needs separately admitted retained training. QC_COMPLETE
 * hands off to the remaining short Phase 1; it is not full menu negotiation.
 * Use single-sample processing for an exact QC handoff. */
enum v8_calling_result v8_calling_enable_quick_connect(
	struct v8_calling *calling, const struct v92_qc1_message *request);
/* COMPLETE transfers the next sample to the selected datapump after the CJ
 * tail has left the V.21 transmit filter. */
enum v8_calling_result v8_calling_process(
	struct v8_calling *calling, const int16_t *receive,
	int16_t *transmit, size_t count);
bool v8_calling_is_complete(
	const struct v8_calling *calling);
enum v8_calling_result v8_calling_selection(
	const struct v8_calling *calling,
	struct v8_menu *menu,
	enum v8_modulation *modulation);

const char *v8_calling_result_name(
	enum v8_calling_result result);

#endif
