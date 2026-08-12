// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE4_LINE_FIXTURE_H
#define V90_PHASE4_LINE_FIXTURE_H

#include <v90_phase3_return_line_rx.h>
#include <v90_phase4_cp_line_tx.h>

#include <stdbool.h>

/* Generate the exact analogue Phase 3 return handoff while the first CPt is
 * already in flight. This models the equalizer's delayed points without a
 * checked-in waveform vector. */
bool v90_phase4_line_fixture_init(
	enum g711_law law, enum v34_constellation_size constellation,
	const struct v90_cp_parameters *training,
	struct v90_phase4_cp_line_tx *transmit,
	struct v90_phase3_return_line_handoff *receive);

#endif /* V90_PHASE4_LINE_FIXTURE_H */
