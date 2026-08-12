// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE4_LINE_HANDOFF_H
#define V34_PHASE4_LINE_HANDOFF_H

#include "v34_phase3_line_receiver.h"
#include "v34_phase4_line_receiver.h"

/* Continue the exact carrier, timing, and delayed equalizer boundary detected
 * from J. The peer's J selection supplies the Phase 4 constellation. */
enum v34_phase4_line_receiver_result
v34_phase4_line_receiver_init_from_phase3(
	struct v34_phase4_line_receiver *receiver,
	const struct v34_phase3_line_handoff *handoff,
	double adaptation);

#endif /* V34_PHASE4_LINE_HANDOFF_H */
