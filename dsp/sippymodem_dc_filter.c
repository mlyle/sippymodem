// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_dc_filter.h"

#include <limits.h>

enum {
	DC_WARMUP_SAMPLES = 5760,
	DC_CALIBRATION_SAMPLES = 9600,
	DC_TRACKING_SAMPLES = 19200,
	DC_OFFSET_LIMIT = 3000,
	DC_TRACKING_OLD_WEIGHT_Q15 = 29491,
	DC_TRACKING_NEW_WEIGHT_Q15 = 3277,
	DC_TRACKING_ROUND_Q15 = 16384,
	DC_TRACKING_SCALE = 32768,
};

static void reset_accumulator(struct sippymodem_dc_filter *filter)
{
	filter->sample_sum = 0;
	filter->sample_count = 0;
}

static int16_t saturate_sample(int32_t sample)
{
	if (sample > INT16_MAX)
		return INT16_MAX;
	if (sample < INT16_MIN)
		return INT16_MIN;
	return (int16_t)sample;
}

static int32_t divide_floor(int64_t value, int32_t divisor)
{
	if (value >= 0)
		return (int32_t)(value / divisor);
	return (int32_t)-((-value + divisor - 1) / divisor);
}

static void advance_estimator(
	struct sippymodem_dc_filter *filter, int16_t sample)
{
	switch (filter->phase) {
	case SIPPYMODEM_DC_FILTER_WARMUP:
		if (++filter->sample_count == DC_WARMUP_SAMPLES) {
			filter->phase = SIPPYMODEM_DC_FILTER_CALIBRATION;
			reset_accumulator(filter);
		}
		break;
	case SIPPYMODEM_DC_FILTER_CALIBRATION:
		filter->sample_sum += sample;
		if (++filter->sample_count == DC_CALIBRATION_SAMPLES) {
			filter->offset = (int16_t)(filter->sample_sum /
						  (int64_t)filter->sample_count);
			filter->phase = SIPPYMODEM_DC_FILTER_TRACKING;
			reset_accumulator(filter);
		}
		break;
	case SIPPYMODEM_DC_FILTER_TRACKING:
		filter->sample_sum += sample;
		if (++filter->sample_count == DC_TRACKING_SAMPLES) {
			int32_t average = (int32_t)(
				filter->sample_sum / (int64_t)filter->sample_count);
			int64_t weighted =
				(int64_t)filter->offset *
					DC_TRACKING_OLD_WEIGHT_Q15 +
				(int64_t)average *
					DC_TRACKING_NEW_WEIGHT_Q15 +
				DC_TRACKING_ROUND_Q15;

			filter->offset = saturate_sample(
				divide_floor(weighted, DC_TRACKING_SCALE));
			reset_accumulator(filter);
		}
		break;
	}
}

void sippymodem_dc_filter_init(struct sippymodem_dc_filter *filter)
{
	if (filter == NULL)
		return;
	*filter = (struct sippymodem_dc_filter){
		.phase = SIPPYMODEM_DC_FILTER_WARMUP,
	};
}

enum sippymodem_dc_filter_result sippymodem_dc_filter_process(
	struct sippymodem_dc_filter *filter, int16_t *samples, size_t count)
{
	if (filter == NULL || (samples == NULL && count != 0))
		return SIPPYMODEM_DC_FILTER_INVALID_ARGUMENT;
	for (size_t index = 0; index < count; ++index) {
		int16_t sample = samples[index];
		int16_t offset = filter->offset;

		advance_estimator(filter, sample);
		samples[index] = saturate_sample((int32_t)sample - offset);
	}
	return filter->offset >= DC_OFFSET_LIMIT ||
		       filter->offset <= -DC_OFFSET_LIMIT ?
		       SIPPYMODEM_DC_FILTER_OFFSET_LIMIT :
		       SIPPYMODEM_DC_FILTER_OK;
}

int16_t sippymodem_dc_filter_offset(
	const struct sippymodem_dc_filter *filter)
{
	return filter != NULL ? filter->offset : 0;
}

const char *sippymodem_dc_filter_result_name(
	enum sippymodem_dc_filter_result result)
{
	switch (result) {
	case SIPPYMODEM_DC_FILTER_OK:
		return "ok";
	case SIPPYMODEM_DC_FILTER_OFFSET_LIMIT:
		return "DC offset exceeds the supported limit";
	case SIPPYMODEM_DC_FILTER_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown DC filter result";
}
