// SPDX-License-Identifier: BSD-3-Clause
#include "v8_handoff.h"
#include "memory_region.h"

#include <string.h>

static bool same_menu(const struct v8_menu *a, const struct v8_menu *b)
{
	uint8_t first[V8_MAX_INFORMATION_OCTETS], second[V8_MAX_INFORMATION_OCTETS];
	size_t first_count, second_count;
	return v8_menu_encode(a, first, sizeof(first), &first_count) == V8_MENU_OK &&
	       v8_menu_encode(b, second, sizeof(second), &second_count) == V8_MENU_OK &&
	       first_count == second_count && !memcmp(first, second, first_count);
}

bool v8_handoff_retry_init(struct v8_handoff_retry *retry,
	const struct v8_menu *local, const struct v8_menu *joint)
{
	struct v8_handoff_retry initialized = { 0 };

	if (retry == NULL || !v8_menu_selection_is_valid(local, joint,
			V8_MODULATION_V34_DUPLEX) ||
	    v8_fsk_init(&initialized.state.monitor.fsk, V8_CALLING_STATION) != V8_FSK_OK ||
	    v8_menu_detector_init(&initialized.state.monitor.menu) != V8_MENU_OK)
		return false;
	initialized.local_menu = *local;
	initialized.joint_menu = *joint;
	initialized.remaining = V8_HANDOFF_WINDOW_SAMPLES;
	*retry = initialized;
	return true;
}

enum v8_handoff_result v8_handoff_retry_sample(struct v8_handoff_retry *retry,
	int16_t received, bool available, int16_t *transmit)
{
	if (retry == NULL || transmit == NULL ||
	    memory_regions_overlap(retry, sizeof(*retry), transmit, sizeof(*transmit)) ||
	    retry->remaining > V8_HANDOFF_WINDOW_SAMPLES)
		return V8_HANDOFF_INVALID;
	if (retry->remaining == 0)
		return V8_HANDOFF_FINISHED;
	--retry->remaining;
	if (!retry->transmitting) {
		uint8_t bit;
		bool ready, confirmed = false;
		struct v8_menu joint;

		if (!available) {
			if (v8_fsk_init(&retry->state.monitor.fsk, V8_CALLING_STATION) != V8_FSK_OK ||
			    v8_menu_detector_init(&retry->state.monitor.menu) != V8_MENU_OK)
				return V8_HANDOFF_INVALID;
			return V8_HANDOFF_LISTEN;
		}
		if (v8_fsk_receive_sample(&retry->state.monitor.fsk, received, &bit, &ready) != V8_FSK_OK ||
		    (ready && v8_menu_detect_bit(&retry->state.monitor.menu, bit, &joint, &confirmed) != V8_MENU_OK))
			return V8_HANDOFF_INVALID;
		if (!confirmed || !same_menu(&retry->joint_menu, &joint))
			return V8_HANDOFF_LISTEN;
		/* ANSam and two complete, matching JM sequences were already received.
		 * Reuse normal CM/CJ framing and retain the original capabilities. */
		if (v8_calling_init(&retry->state.calling, &retry->local_menu) != V8_CALLING_OK)
			return V8_HANDOFF_INVALID;
		retry->state.calling.state = V8_CALLING_EXCHANGE_MENUS;
		retry->transmitting = true;
		retry->remaining = V8_HANDOFF_WINDOW_SAMPLES;
		*transmit = 0;
		return V8_HANDOFF_SEND;
	}
	struct v8_calling *calling = &retry->state.calling;
	if (!available)
		v8_menu_detector_reset(&calling->joint_menu_detector);
	if (calling->state == V8_CALLING_SEND_CJ && !calling->phase2_silence &&
	    v8_calling_delegate_phase2_silence(calling) != V8_CALLING_OK)
		return V8_HANDOFF_INVALID;
	enum v8_calling_result result = v8_calling_process(calling, &received, transmit, 1);
	if (result == V8_CALLING_INVALID_ARGUMENT)
		return V8_HANDOFF_INVALID;
	if (result != V8_CALLING_OK) {
		retry->remaining = 0;
		return V8_HANDOFF_FINISHED;
	}
	if (v8_calling_is_complete(calling)) {
		retry->remaining = 0;
		return same_menu(&retry->joint_menu, &calling->negotiated_menu) ?
			V8_HANDOFF_RESTART : V8_HANDOFF_FINISHED;
	}
	return retry->remaining == 0 ? V8_HANDOFF_FINISHED : V8_HANDOFF_SEND;
}
