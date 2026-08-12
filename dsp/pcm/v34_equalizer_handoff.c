// SPDX-License-Identifier: BSD-3-Clause

#include "v34_equalizer_handoff.h"

#include <stddef.h>

bool v34_equalizer_handoff_is_valid(
	const struct v34_equalizer_handoff *handoff)
{
	return handoff != NULL &&
	       qam_equalizer_is_valid(&handoff->equalizer) &&
	       handoff->equalizer.fill == QAM_EQUALIZER_DELAY + 1u &&
	       !handoff->equalizer.can_adapt;
}
