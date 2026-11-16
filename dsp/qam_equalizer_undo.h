// SPDX-License-Identifier: BSD-3-Clause

#ifndef QAM_EQUALIZER_UNDO_H
#define QAM_EQUALIZER_UNDO_H

#include "qam_equalizer.h"

#include <stddef.h>
#include <string.h>

/* A push changes history and the control tail, never coefficients. Under
 * exclusive ownership, save only the outgoing history point and control
 * bytes. Restore after a successful push followed by a rejected adaptation;
 * adaptation itself must retain its transactional rejection contract.
 * Geometry must stay unchanged and snapshot/state storage must be separate. */
struct qam_equalizer_push_undo {
	struct qam_point oldest;
	unsigned char control[sizeof(struct qam_equalizer) -
		offsetof(struct qam_equalizer, last_output)];
};

_Static_assert(offsetof(struct qam_equalizer, last_output) ==
	sizeof(((struct qam_equalizer *)0)->coefficients) +
	sizeof(((struct qam_equalizer *)0)->history),
	"equalizer control must follow coefficients and history");

static inline void qam_equalizer_save_push(
	const struct qam_equalizer *equalizer, struct qam_equalizer_push_undo *undo)
{
	undo->oldest = equalizer->history[equalizer->tap_count - 1u];
	memcpy(undo->control, (const unsigned char *)equalizer +
		offsetof(struct qam_equalizer, last_output), sizeof(undo->control));
}

static inline void qam_equalizer_undo_push(
	struct qam_equalizer *equalizer, const struct qam_equalizer_push_undo *undo)
{
	memmove(equalizer->history, equalizer->history + 1,
		(equalizer->tap_count - 1u) * sizeof(undo->oldest));
	equalizer->history[equalizer->tap_count - 1u] = undo->oldest;
	memcpy((unsigned char *)equalizer +
		offsetof(struct qam_equalizer, last_output), undo->control, sizeof(undo->control));
}

#endif
