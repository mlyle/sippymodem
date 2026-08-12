// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase4_line_handoff.h"

enum v34_phase4_line_receiver_result
v34_phase4_line_receiver_init_from_phase3(
	struct v34_phase4_line_receiver *receiver,
	const struct v34_phase3_line_handoff *handoff,
	enum v34_constellation_size phase4_constellation,
	double adaptation)
{
	if (!v34_phase3_line_handoff_is_valid(handoff))
		return V34_PHASE4_LINE_RECEIVER_INVALID_ARGUMENT;
	return v34_phase4_line_receiver_init(
		receiver, &handoff->demodulator,
		&handoff->phase3.j.equalizer.equalizer,
		&handoff->transmitting_profile,
		phase4_constellation,
		adaptation);
}
