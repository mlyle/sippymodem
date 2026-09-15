// SPDX-License-Identifier: BSD-3-Clause

#ifndef V8_ANSWERING_H
#define V8_ANSWERING_H

#include "v8_ansam.h"
#include "v8_fsk.h"
#include "v8_menu.h"
#include "v92_qc1.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V8_ANSWERING_MAX_FRAGMENT_SAMPLES = 160,
	/* The direct media owner retains the complete 387 ms answer-station
	 * quiet interval before ANSam. */
	V8_ANSWERING_QUIET_SAMPLES = 3096,
	V8_ANSWERING_COARSE_GAIN_SAMPLES = V8_ANSAM_SAMPLE_RATE / 4,
	V8_ANSWERING_CALL_MENU_TIMEOUT_SAMPLES =
		12 * V8_ANSAM_SAMPLE_RATE,
	V8_ANSWERING_CJ_TIMEOUT_SAMPLES =
		7 * V8_ANSAM_SAMPLE_RATE,
	/* V.8 permits another termination criterion when CJ is damaged.  After
	 * two framed zero octets, wait through the remaining octet and one
	 * guard bit before accepting that bounded fallback. */
	V8_ANSWERING_CJ_FALLBACK_BITS =
		V8_FRAMED_OCTET_BITS + 1,
};

enum v8_answering_state {
	V8_ANSWERING_WAIT_CALL_MENU,
	V8_ANSWERING_SEND_JOINT_MENU,
	V8_ANSWERING_SEND_QCA1,
	V8_ANSWERING_QC_COMPLETE,
	V8_ANSWERING_COMPLETE,
	V8_ANSWERING_FAILED,
};

enum v8_answering_result {
	V8_ANSWERING_OK,
	V8_ANSWERING_INVALID_ARGUMENT,
	V8_ANSWERING_NOT_READY,
	V8_ANSWERING_CALL_MENU_TIMEOUT,
	V8_ANSWERING_CJ_TIMEOUT,
	V8_ANSWERING_NO_COMMON_MODULATION,
};

struct v8_answering {
	struct v8_ansam_generator answer_signal;
	struct v8_fsk fsk;
	struct v8_menu_detector call_menu_detector;
	struct v8_control_generator joint_menu_bits;
	struct v8_cj_detector cj_detector;
	struct v8_menu local_menu;
	struct v8_menu call_menu;
	struct v8_menu negotiated_menu;
	struct v92_qc1_receiver qc_receiver;
	struct v92_qc1_message qc_response;
	struct v92_qc1_message qc_request;
	enum v8_modulation selected_modulation;
	enum v8_answering_state state;
	enum v8_answering_result terminal_result;
	uint32_t state_samples;
	uint16_t quiet_samples;
	uint8_t transmit_bit;
	uint8_t cj_fallback_bits;
	uint8_t qc_bit_index;
	bool cj_fallback_armed;
	bool negotiated;
	bool qc_enabled;
};

enum v8_answering_result v8_answering_init(
	struct v8_answering *answering,
	const struct v8_menu *local_menu);
/* Opt in before processing any samples, after the owner admits retained
 * history for this peer. Copy the local QCA1 response; borrow no history.
 * Only complementary analogue/digital QC1 requests are acknowledged. Other
 * requests, or no opt-in, retain ordinary CM/JM negotiation.
 * QC_COMPLETE ends the QCA1 transmit-filter tail, not the full short Phase 1.
 * Its owner must use qc_request/qc_response for the following signal exchange
 * and its fallback deadline. Process single samples for an exact handoff. */
enum v8_answering_result v8_answering_enable_quick_connect(
	struct v8_answering *answering,
	const struct v92_qc1_message *response);
/* COMPLETE transfers the next sample to the selected datapump. That owner
 * emits the post-CJ silence while already listening for its peer. QC_COMPLETE
 * instead hands off to the short-Phase-1 signal exchange described above. */
enum v8_answering_result v8_answering_process(
	struct v8_answering *answering, const int16_t *receive,
	int16_t *transmit, size_t count);
bool v8_answering_is_complete(
	const struct v8_answering *answering);
enum v8_answering_result v8_answering_selection(
	const struct v8_answering *answering,
	struct v8_menu *menu,
	enum v8_modulation *modulation);

const char *v8_answering_result_name(
	enum v8_answering_result result);

#endif
