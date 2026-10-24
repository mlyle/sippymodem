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

/* Owned linear projection before physical rounding or clipping. Geometry and
 * coefficients are validated at the boundary; inputs and retained outputs
 * must be finite. A frozen echo prediction uses the same arithmetic with its
 * own history, which need not fit the physical signed-16 sample bounds.
 * v92_residual_sample() separately enforces those bounds for receive state. */
static inline double v92_residual_output(const struct v92_residual *filter, double raw)
{
	double corrected = raw - filter->offset;
	for (unsigned j = 0; j < filter->count; j++)
		corrected -= filter->coefficient[j] * filter->history[
			(filter->cursor + V92_RESIDUAL_LAGS - filter->delay[j]) % V92_RESIDUAL_LAGS];
	return corrected;
}

static inline void v92_residual_retain(struct v92_residual *filter, double corrected)
{
	filter->history[filter->cursor] = corrected;
	filter->cursor = (filter->cursor + 1) % V92_RESIDUAL_LAGS;
}

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
