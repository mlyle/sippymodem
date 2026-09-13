// SPDX-License-Identifier: BSD-3-Clause

#include "v92_data_quality.h"

#include "v92_pcm_mapping.h"

#include <math.h>
#include <stddef.h>

bool v92_data_quality_is_valid(const struct v92_data_quality *quality)
{
	return quality != NULL && isfinite(quality->squared_error) &&
	       quality->squared_error >= 0.0 &&
	       quality->frames < V92_DATA_QUALITY_WINDOW_FRAMES &&
	       (quality->frames != 0u ||
		(quality->squared_error == 0.0 && !quality->uncertain_window)) &&
	       quality->bad_windows < V92_DATA_QUALITY_BAD_WINDOWS &&
	       quality->good_windows < V92_DATA_QUALITY_REARM_WINDOWS &&
	       (quality->good_windows == 0u ||
		(quality->automatic_retrains != 0u && quality->bad_windows == 0u)) &&
	       quality->automatic_retrains <= V92_DATA_QUALITY_MAX_RETRAINS;
}

bool v92_data_quality_restart(struct v92_data_quality *quality)
{
	if (!v92_data_quality_is_valid(quality))
		return false;
	*quality = (struct v92_data_quality){
		.automatic_retrains = quality->automatic_retrains,
	};
	return true;
}

enum v92_data_quality_result v92_data_quality_retrain(struct v92_data_quality *quality)
{
	if (!v92_data_quality_is_valid(quality))
		return V92_DATA_QUALITY_INVALID_ARGUMENT;
	if (quality->automatic_retrains == V92_DATA_QUALITY_MAX_RETRAINS)
		return V92_DATA_QUALITY_EXHAUSTED;
	*quality = (struct v92_data_quality){
		.automatic_retrains = quality->automatic_retrains + 1u,
	};
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
		next.bad_windows = next.squared_error > limit || next.uncertain_window ?
			next.bad_windows + 1u : 0u;
		recover |= next.bad_windows == V92_DATA_QUALITY_BAD_WINDOWS;
		if (next.bad_windows || ambiguous) {
			next.good_windows = 0u;
		} else if (next.automatic_retrains &&
			   ++next.good_windows == V92_DATA_QUALITY_REARM_WINDOWS) {
			next.automatic_retrains = 0u;
			next.good_windows = 0u;
		}
		next.frames = 0u;
		next.squared_error = 0.0;
		next.uncertain_window = false;
	}
	if (recover)
		return v92_data_quality_retrain(quality);
	*quality = next;
	return V92_DATA_QUALITY_OK;
}
