// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_STARTUP_H
#define PCM_STARTUP_H

#include "pcm_protocol.h"
#include "v92_info0.h"

#include <stdbool.h>

enum pcm_phase2_mode {
	PCM_PHASE2_FULL,
	PCM_PHASE2_SHORT,
};

enum pcm_startup_result {
	PCM_STARTUP_OK,
	PCM_STARTUP_INVALID_ARGUMENT,
	PCM_STARTUP_INVALID_CAPABILITIES,
};

/* The first common V.90/V.92 session decision. Both roles derive the same
 * value from their decoded INFO0 records. Upstream-mode selection remains at
 * the later INFO1 boundary. */
struct pcm_startup_selection {
	enum pcm_protocol protocol;
	enum pcm_phase2_mode phase2;
};

bool pcm_startup_selection_is_valid(
	const struct pcm_startup_selection *selection);

/* Select V.92 only when both endpoints advertise it. Short Phase 2 likewise
 * requires both endpoints to request it; a unilateral request retains the
 * full V.92 procedure. Rejected calls leave selection unchanged. */
enum pcm_startup_result pcm_startup_select(
	const struct v92_info0a *analog,
	const struct v92_info0d *digital,
	struct pcm_startup_selection *selection);

#endif /* PCM_STARTUP_H */
