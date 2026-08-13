// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE4_H
#define V90_PHASE4_H

#include "v90_cp.h"

enum {
	/* V.90 clause 9.4.1.2 minimum before the first MP sequence. */
	V90_PHASE4_TRN2D_MINIMUM_SYMBOLS = 2040,
	V90_PHASE4_TRN2D_MINIMUM_FRAMES =
		V90_PHASE4_TRN2D_MINIMUM_SYMBOLS / V90_CP_FRAME_INTERVALS,
};

#endif /* V90_PHASE4_H */
