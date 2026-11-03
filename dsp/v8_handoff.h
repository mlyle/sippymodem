// SPDX-License-Identifier: BSD-3-Clause
#ifndef V8_HANDOFF_H
#define V8_HANDOFF_H

#include "v8_calling.h"

enum { V8_HANDOFF_WINDOW_SAMPLES = 2 * V8_ANSAM_SAMPLE_RATE };

enum v8_handoff_result {
	V8_HANDOFF_LISTEN,
	V8_HANDOFF_SEND,
	V8_HANDOFF_RESTART,
	V8_HANDOFF_FINISHED,
	V8_HANDOFF_INVALID,
};

/* One initial handoff, owned by its receiving datapump. A fresh repeated JM
 * can recover a missed CJ without changing the ordinary V.8 exchange. The
 * owner must stop calling this as soon as initial INFO0 succeeds. */
struct v8_handoff_retry {
	union {
		struct {
			struct v8_fsk fsk;
			struct v8_menu_detector menu;
		} monitor;
		struct v8_calling calling;
	} state;
	struct v8_menu local_menu, joint_menu;
	unsigned remaining;
	bool transmitting;
};

bool v8_handoff_retry_init(struct v8_handoff_retry *retry,
	const struct v8_menu *local, const struct v8_menu *joint);

/* Each input consumes one physical sample and the existing host deadline.
 * SEND/RESTART supply a transmit sample; RESTART asks the owner to start
 * fresh INFO0 after CJ's filter tail. The owner supplies its usual silence.
 * FINISHED cannot rearm the retry. Missing media breaks menu confirmation. */
enum v8_handoff_result v8_handoff_retry_sample(struct v8_handoff_retry *retry,
	int16_t received, bool available, int16_t *transmit);

#endif
