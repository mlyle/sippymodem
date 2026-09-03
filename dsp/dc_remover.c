// SPDX-License-Identifier: BSD-3-Clause

#include "dc_remover.h"

#include "integer_math.h"
#include "memory_region.h"

#include <limits.h>
#include <stdint.h>

enum {
	DC_REMOVER_HISTORY_WEIGHT_Q15 = 0x7333,
	DC_REMOVER_INPUT_WEIGHT_Q15 = 0x0ccd,
};

static bool accumulator_is_valid(const struct dc_remover *remover)
{
	int64_t count = remover->sample_count;

	return remover->sum >= count * INT16_MIN &&
	       remover->sum <= count * INT16_MAX;
}

bool dc_remover_is_valid(const struct dc_remover *remover)
{
	if (remover == NULL || !accumulator_is_valid(remover))
		return false;
	switch (remover->stage) {
	case DC_REMOVER_WARMUP:
		return remover->sample_count < DC_REMOVER_WARMUP_SAMPLES &&
		       remover->sum == 0 && remover->level == 0;
	case DC_REMOVER_CALIBRATION:
		return remover->sample_count < DC_REMOVER_CALIBRATION_SAMPLES &&
		       remover->level == 0;
	case DC_REMOVER_TRACKING:
		return remover->sample_count < DC_REMOVER_TRACKING_SAMPLES;
	}
	return false;
}

enum dc_remover_result dc_remover_init(struct dc_remover *remover)
{
	struct dc_remover initialized = {
		.stage = DC_REMOVER_WARMUP,
	};

	if (remover == NULL)
		return DC_REMOVER_INVALID_ARGUMENT;
	*remover = initialized;
	return DC_REMOVER_OK;
}

static int16_t updated_level(int16_t previous, int64_t sum, uint32_t count)
{
	int64_t average = sum / count;
	int64_t weighted =
		(int64_t)previous * DC_REMOVER_HISTORY_WEIGHT_Q15 +
		average * DC_REMOVER_INPUT_WEIGHT_Q15 + 0x4000;

	return clamp_s16(arithmetic_shift_right_u64((uint64_t)weighted, 15));
}

enum dc_remover_result dc_remover_process(
	struct dc_remover *remover, int16_t *samples, size_t count)
{
	struct dc_remover next;
	size_t bytes;

	if (!dc_remover_is_valid(remover))
		return DC_REMOVER_INVALID_STATE;
	if (samples == NULL || count == 0 ||
	    count > SIZE_MAX / sizeof(*samples))
		return DC_REMOVER_INVALID_ARGUMENT;
	bytes = count * sizeof(*samples);
	if (memory_regions_overlap(samples, bytes, remover, sizeof(*remover)))
		return DC_REMOVER_INVALID_ARGUMENT;

	next = *remover;
	for (size_t index = 0; index < count; ++index) {
		int16_t sample = samples[index];
		int16_t subtract_level = next.level;

		switch (next.stage) {
		case DC_REMOVER_WARMUP:
			if (++next.sample_count == DC_REMOVER_WARMUP_SAMPLES) {
				next.stage = DC_REMOVER_CALIBRATION;
				next.sample_count = 0;
			}
			break;
		case DC_REMOVER_CALIBRATION:
			next.sum += sample;
			if (++next.sample_count ==
			    DC_REMOVER_CALIBRATION_SAMPLES) {
				next.level = (int16_t)(next.sum /
							 next.sample_count);
				next.stage = DC_REMOVER_TRACKING;
				next.sum = 0;
				next.sample_count = 0;
			}
			break;
		case DC_REMOVER_TRACKING:
			next.sum += sample;
			if (++next.sample_count == DC_REMOVER_TRACKING_SAMPLES) {
				next.level = updated_level(
					next.level, next.sum, next.sample_count);
				next.sum = 0;
				next.sample_count = 0;
			}
			break;
		}
		samples[index] = clamp_s16((int32_t)sample - subtract_level);
	}
	if (!dc_remover_is_valid(&next))
		return DC_REMOVER_INVALID_STATE;
	*remover = next;
	return DC_REMOVER_OK;
}
