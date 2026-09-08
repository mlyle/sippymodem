// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE4_H
#define V90_PHASE4_H

#include "v90_cp.h"

enum {
	/* V.90 clause 9.4.1.2 minimum before the first MP sequence. */
	V90_PHASE4_TRN2D_MINIMUM_SYMBOLS = 2040,
	V90_PHASE4_TRN2D_MINIMUM_FRAMES =
		V90_PHASE4_TRN2D_MINIMUM_SYMBOLS / V90_CP_FRAME_INTERVALS,
	/* Keep the initial V.90 training epoch for one second.  This remains
	 * inside the two-second MP deadline while giving the analogue receiver
	 * time to condition before the first parameter sequence. */
	V90_PHASE4_TRN2D_INITIAL_SYMBOLS = 8000,
	V90_PHASE4_TRN2D_INITIAL_FRAMES =
		(V90_PHASE4_TRN2D_INITIAL_SYMBOLS +
		 V90_CP_FRAME_INTERVALS - 1) /
		V90_CP_FRAME_INTERVALS,
	/* While CP is still absent, allow 1.815 seconds of conditioning. Leave
	 * room for spectral look-ahead before the two-second MP deadline. */
	V90_PHASE4_TRN2D_WAIT_CP_SYMBOLS = 14520,
	V90_PHASE4_TRN2D_WAIT_CP_FRAMES =
		V90_PHASE4_TRN2D_WAIT_CP_SYMBOLS / V90_CP_FRAME_INTERVALS,
	/* V.90 clause 8.6 defines Ed as exactly two data frames. */
	V90_PHASE4_ED_FRAMES = 2,
	/* B1d is the first 48 frames of the fresh data-coding epoch. */
	V90_PHASE4_B1D_FRAMES = 48,
};

#endif /* V90_PHASE4_H */
