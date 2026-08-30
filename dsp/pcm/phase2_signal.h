// SPDX-License-Identifier: BSD-3-Clause

#ifndef PHASE2_SIGNAL_H
#define PHASE2_SIGNAL_H

#include <stdbool.h>

/* Signal A uses the 2400 Hz carrier and 1800 Hz guard. Signal B uses the
 * unguarded 1200 Hz carrier. V.34 assigns these by station role; PCM modes
 * assign them by analogue/digital role. */
enum phase2_signal_role {
	PHASE2_SIGNAL_A,
	PHASE2_SIGNAL_B,
};

static inline bool phase2_signal_role_is_valid(
	enum phase2_signal_role role)
{
	return role == PHASE2_SIGNAL_A || role == PHASE2_SIGNAL_B;
}

#endif
