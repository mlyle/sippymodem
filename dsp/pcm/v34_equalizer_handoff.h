// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_EQUALIZER_HANDOFF_H
#define V34_EQUALIZER_HANDOFF_H

#include "qam_equalizer.h"

#include <stdbool.h>

/* Phase 4 detects the final E symbol only after the equalizer delay has
 * accepted the first QAM_EQUALIZER_DELAY B1 inputs. This value transfers that
 * exact history to the data receiver; the next input therefore produces the
 * first B1 output rather than another training output. */
struct v34_equalizer_handoff {
	struct qam_equalizer equalizer;
};

bool v34_equalizer_handoff_is_valid(
	const struct v34_equalizer_handoff *handoff);

#endif /* V34_EQUALIZER_HANDOFF_H */
