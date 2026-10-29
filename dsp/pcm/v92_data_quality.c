// SPDX-License-Identifier: BSD-3-Clause

#include "v92_data_quality.h"

#include "v92_pcm_mapping.h"

#include <math.h>
#include <stddef.h>

/* PCM upstream always runs at 8000 symbols/s. Keep the shortest whole-window
 * persistence that covers 1.8 seconds when the measurement geometry changes. */
_Static_assert(V92_DATA_QUALITY_BAD_WINDOWS * V92_DATA_QUALITY_WINDOW_FRAMES *
	V92_PCM_MAPPING_FRAME_SYMBOLS >= 1800 * 8, "quality persistence is too short");
_Static_assert((V92_DATA_QUALITY_BAD_WINDOWS - 1) * V92_DATA_QUALITY_WINDOW_FRAMES *
	V92_PCM_MAPPING_FRAME_SYMBOLS < 1800 * 8, "quality persistence has an extra window");

bool v92_data_quality_is_valid(const struct v92_data_quality *quality)
{
	return quality != NULL && isfinite(quality->squared_error) &&
	       quality->squared_error >= 0.0 &&
	       quality->frames < V92_DATA_QUALITY_WINDOW_FRAMES &&
	       (quality->frames != 0u ||
		(quality->squared_error == 0.0 && !quality->uncertain_window)) &&
	       quality->recovery.bad_windows < V92_DATA_QUALITY_BAD_WINDOWS &&
	       quality->recovery.good_windows < V92_DATA_QUALITY_REARM_WINDOWS &&
	       (quality->recovery.good_windows == 0u ||
		(quality->recovery.attempts != 0u && quality->recovery.bad_windows == 0u)) &&
	       quality->recovery.attempts <= V92_DATA_QUALITY_MAX_RETRAINS;
}

bool v92_data_quality_restart(struct v92_data_quality *quality)
{
	if (!v92_data_quality_is_valid(quality))
		return false;
	*quality = (struct v92_data_quality){
		.recovery.attempts = quality->recovery.attempts,
	};
	return true;
}

enum v92_data_quality_result v92_data_quality_retrain(struct v92_data_quality *quality)
{
	if (!v92_data_quality_is_valid(quality))
		return V92_DATA_QUALITY_INVALID_ARGUMENT;
	struct v92_data_quality next = { .recovery = quality->recovery };

	if (!recovery_take(&next.recovery, V92_DATA_QUALITY_MAX_RETRAINS))
		return V92_DATA_QUALITY_EXHAUSTED;
	*quality = next;
	return V92_DATA_QUALITY_RETRAIN;
}

enum v92_data_quality_result v92_data_quality_observe(
	struct v92_data_quality *quality, double squared_error, bool ambiguous,
	bool uncertain)
{
	const double limit = (double)V92_DATA_QUALITY_WINDOW_FRAMES *
		V92_PCM_MAPPING_FRAME_SYMBOLS * V92_DATA_MAX_RMS_ERROR *
		V92_DATA_MAX_RMS_ERROR;
	struct v92_data_quality next;
	bool recover = ambiguous;

	if (!v92_data_quality_is_valid(quality) ||
	    !isfinite(squared_error) || squared_error < 0.0)
		return V92_DATA_QUALITY_INVALID_ARGUMENT;
	next = *quality;
	next.squared_error += squared_error;
	if (!isfinite(next.squared_error))
		return V92_DATA_QUALITY_INVALID_ARGUMENT;
	next.uncertain_window |= uncertain;
	if (++next.frames == V92_DATA_QUALITY_WINDOW_FRAMES) {
		bool bad = next.squared_error > limit || next.uncertain_window;

		recovery_window(&next.recovery, bad, !bad && !ambiguous,
			V92_DATA_QUALITY_BAD_WINDOWS, V92_DATA_QUALITY_REARM_WINDOWS);
		recover |= next.recovery.bad_windows == V92_DATA_QUALITY_BAD_WINDOWS;
		next.frames = 0u;
		next.squared_error = 0.0;
		next.uncertain_window = false;
	}
	if (recover)
		return v92_data_quality_retrain(quality);
	*quality = next;
	return V92_DATA_QUALITY_OK;
}
