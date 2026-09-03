// SPDX-License-Identifier: BSD-3-Clause

#include <dc_remover.h>

#include "test.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static bool test_calibration_and_tracking(void)
{
	enum {
		CALIBRATED = DC_REMOVER_WARMUP_SAMPLES +
			DC_REMOVER_CALIBRATION_SAMPLES,
	};
	struct dc_remover remover;
	int16_t samples[CALIBRATED];
	int16_t tracking[DC_REMOVER_TRACKING_SAMPLES + 1u];

	for (size_t index = 0; index < ARRAY_LENGTH(samples); ++index)
		samples[index] = 1400;
	CHECK(dc_remover_init(&remover) == DC_REMOVER_OK &&
	      dc_remover_process(&remover, samples, ARRAY_LENGTH(samples)) ==
		      DC_REMOVER_OK &&
	      remover.stage == DC_REMOVER_TRACKING && remover.level == 1400 &&
	      samples[CALIBRATED - 1u] == 1400,
	      "DC remover did not preserve its warmup/calibration boundary");

	for (size_t index = 0; index < ARRAY_LENGTH(tracking); ++index)
		tracking[index] = 2400;
	CHECK(dc_remover_process(
		      &remover, tracking, ARRAY_LENGTH(tracking)) ==
		      DC_REMOVER_OK &&
	      remover.level == 1500 &&
	      tracking[DC_REMOVER_TRACKING_SAMPLES - 1u] == 1000 &&
	      tracking[DC_REMOVER_TRACKING_SAMPLES] == 900,
	      "DC remover did not track the long-term offset");

	for (size_t index = 0; index < ARRAY_LENGTH(samples); ++index)
		samples[index] = -1400;
	for (size_t index = 0; index < ARRAY_LENGTH(tracking); ++index)
		tracking[index] = -2400;
	CHECK(dc_remover_init(&remover) == DC_REMOVER_OK &&
	      dc_remover_process(&remover, samples, ARRAY_LENGTH(samples)) ==
		      DC_REMOVER_OK &&
	      remover.level == -1400 &&
	      dc_remover_process(
		      &remover, tracking, ARRAY_LENGTH(tracking)) ==
		      DC_REMOVER_OK &&
	      remover.level == -1500 &&
	      tracking[DC_REMOVER_TRACKING_SAMPLES - 1u] == -1000 &&
	      tracking[DC_REMOVER_TRACKING_SAMPLES] == -900,
	      "DC remover did not track a negative offset");
	return true;
}

static bool test_chunk_invariance_and_saturation(void)
{
	enum { SAMPLE_COUNT = 40000 };
	struct dc_remover scalar;
	struct dc_remover blocked;
	int16_t input[SAMPLE_COUNT];
	int16_t one[SAMPLE_COUNT];
	int16_t chunks[SAMPLE_COUNT];

	for (size_t index = 0; index < ARRAY_LENGTH(input); ++index)
		input[index] = (int16_t)(1400 + (int)(index % 29u) - 14);
	memcpy(one, input, sizeof(one));
	memcpy(chunks, input, sizeof(chunks));
	CHECK(dc_remover_init(&scalar) == DC_REMOVER_OK &&
	      dc_remover_init(&blocked) == DC_REMOVER_OK,
	      "DC remover instances did not initialize");
	for (size_t index = 0; index < ARRAY_LENGTH(one); ++index)
		CHECK(dc_remover_process(&scalar, &one[index], 1) ==
			      DC_REMOVER_OK,
		      "DC remover rejected scalar sample %zu", index);
	for (size_t offset = 0; offset < ARRAY_LENGTH(chunks); offset += 127u) {
		size_t remaining = ARRAY_LENGTH(chunks) - offset;
		size_t count = remaining < 127u ? remaining : 127u;

		CHECK(dc_remover_process(&blocked, chunks + offset, count) ==
			      DC_REMOVER_OK,
		      "DC remover rejected block at %zu", offset);
	}
	CHECK(memcmp(one, chunks, sizeof(one)) == 0 &&
	      memcmp(&scalar, &blocked, sizeof(scalar)) == 0,
	      "DC removal depends on host block size");

	blocked.stage = DC_REMOVER_TRACKING;
	blocked.level = INT16_MIN;
	blocked.sum = 0;
	blocked.sample_count = 0;
	chunks[0] = INT16_MAX;
	CHECK(dc_remover_process(&blocked, chunks, 1) == DC_REMOVER_OK &&
	      chunks[0] == INT16_MAX,
	      "DC removal overflow did not saturate");
	return true;
}

static bool test_contracts(void)
{
	struct dc_remover remover;
	struct dc_remover saved;
	int16_t sample = 123;

	CHECK(dc_remover_init(NULL) == DC_REMOVER_INVALID_ARGUMENT &&
	      dc_remover_init(&remover) == DC_REMOVER_OK &&
	      dc_remover_is_valid(&remover),
	      "DC remover lifecycle rejected valid initialization");
	saved = remover;
	CHECK(dc_remover_process(&remover, NULL, 1) ==
		      DC_REMOVER_INVALID_ARGUMENT &&
	      dc_remover_process(&remover, &sample, 0) ==
		      DC_REMOVER_INVALID_ARGUMENT &&
	      memcmp(&remover, &saved, sizeof(remover)) == 0 && sample == 123,
	      "DC remover argument rejection changed its owner");
	remover.stage = (enum dc_remover_stage)99;
	CHECK(dc_remover_process(&remover, &sample, 1) ==
		      DC_REMOVER_INVALID_STATE &&
	      sample == 123,
	      "DC remover accepted invalid state");
	return true;
}

int main(void)
{
	if (!test_calibration_and_tracking() ||
	    !test_chunk_invariance_and_saturation() || !test_contracts())
		return 1;
	puts("DC remover tests passed");
	return 0;
}
