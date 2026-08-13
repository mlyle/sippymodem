// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE4_H
#define V90_PHASE4_H

#include "v90_cp.h"

enum {
	/* V.90 clause 9.4.1.2 minimum before the first MP sequence. */
	V90_PHASE4_TRN2D_MINIMUM_SYMBOLS = 2040,
	V90_PHASE4_TRN2D_MINIMUM_FRAMES =
		V90_PHASE4_TRN2D_MINIMUM_SYMBOLS / V90_CP_FRAME_INTERVALS,
	/* V.90 clause 8.6 defines Ed as exactly two data frames. */
	V90_PHASE4_ED_FRAMES = 2,
	/* B1d is the first 48 frames of the fresh data-coding epoch. */
	V90_PHASE4_B1D_FRAMES = 48,
};

#endif /* V90_PHASE4_H */
