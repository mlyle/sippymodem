// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_PHASE3_PROFILE_H
#define PCM_PHASE3_PROFILE_H

#include "pcm_info1.h"
#include "v34_phase3_profile.h"

/* Derive the analogue modem's conventional V.34 upstream profile from a
 * completed V.90 or V.92 INFO1 selection. PCM-upstream selections belong to
 * the separate V.92 Phase 3 path. Rejected calls leave profile unchanged. */
enum v34_phase3_profile_result pcm_phase3_conventional_profile(
	const struct pcm_info1_selection *selection,
	struct v34_phase3_profile *profile);

#endif /* PCM_PHASE3_PROFILE_H */
