// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_EQUALIZER_HANDOFF_H
#define V34_EQUALIZER_HANDOFF_H

#include "qam_equalizer.h"

#include <stdbool.h>

/* Transfer an equalizer across a delayed training boundary. The source has
 * already accepted QAM_EQUALIZER_DELAY inputs from the following interval;
 * the next input therefore produces its first output without replay. */
struct v34_equalizer_handoff {
	struct qam_equalizer equalizer;
};

bool v34_equalizer_handoff_is_valid(
	const struct v34_equalizer_handoff *handoff);

#endif /* V34_EQUALIZER_HANDOFF_H */
