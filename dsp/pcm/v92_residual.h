// SPDX-License-Identifier: BSD-3-Clause
#ifndef V92_RESIDUAL_H
#define V92_RESIDUAL_H

#include "v92_pcm_frame.h"

/* A small causal inverse for residual ISI after the peer's CPd prefilter.
 * The l1 bound is a sufficient stability condition independent of delay.
 * Fits use only acquisition; later B1u and payload cannot change them. */
enum { V92_RESIDUAL_LAGS = V92_PCM_MAPPING_FRAME_SYMBOLS };
struct v92_residual {
	double history[V92_RESIDUAL_LAGS], coefficient[2], offset;
	uint8_t delay[2], count, cursor;
	bool active;
};

bool v92_residual_is_valid(const struct v92_residual *filter);
/* points/raw contain all acquisition frames. Only the final traceback span
 * is fitted; its preceding predictions provide delayed features. Rejection
 * leaves outputs unchanged. Optional score receives the acquisition model
 * selection cost; smaller is better. Gain is the inverse channel gain and
 * must remain inside the original training-cell bounds. Initial inverse history
 * is primed from the same acquisition observations, without held-out input. */
bool v92_residual_fit(struct v92_residual *filter,
	const int32_t *points, const int32_t *raw, double minimum, double maximum,
	double *gain, double *score);

/* Re-express a fresh fit for points translated by one exact integer offset
 * across all current/delayed fitting inputs. The DC term absorbs the shift;
 * coefficients and gain remain unchanged. Reprime its inverse history.
 * This does not equate coding histories or constrain held-out decisions.
 * Invalid, nontranslated or out-of-bound calls leave the fit unchanged. */
bool v92_residual_translate(struct v92_residual *filter,
	const int32_t *original, const int32_t *translated, const int32_t *raw,
	double gain);

/* Exclusively owned, validated filter. Returns the input-domain correction,
 * preserving the original raw sample for wire evidence and quality scoring.
 * Missing media or an out-of-range inverse fails without advancing history. */
bool v92_residual_sample(struct v92_residual *filter,
	int32_t raw, int polarity, bool available, int32_t *correction);

#endif /* V92_RESIDUAL_H */
