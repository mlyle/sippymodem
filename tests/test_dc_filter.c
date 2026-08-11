// SPDX-License-Identifier: BSD-3-Clause

#include <dc_filter.h>

#include "test.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum {
	WARMUP_SAMPLES = 5760,
	CALIBRATION_SAMPLES = 9600,
	TRACKING_SAMPLES = 19200,
};

static enum sippymodem_dc_filter_result feed_constant(
	struct sippymodem_dc_filter *filter,
	int16_t value,
	size_t count,
	size_t chunk_size)
{
	int16_t samples[160];
	enum sippymodem_dc_filter_result result = SIPPYMODEM_DC_FILTER_OK;

	if (chunk_size == 0 || chunk_size > ARRAY_LENGTH(samples))
		return SIPPYMODEM_DC_FILTER_INVALID_ARGUMENT;
	for (size_t offset = 0; offset < count; offset += chunk_size) {
		size_t chunk = count - offset;

		if (chunk > chunk_size)
			chunk = chunk_size;
		for (size_t index = 0; index < chunk; ++index)
			samples[index] = value;
		result = sippymodem_dc_filter_process(filter, samples, chunk);
	}
	return result;
}

static bool test_calibrates_and_tracks_offset(void)
{
	struct sippymodem_dc_filter filter;
	int16_t sample = 1300;

	sippymodem_dc_filter_init(&filter);
	CHECK(feed_constant(&filter, 1200, WARMUP_SAMPLES, 73) ==
			      SIPPYMODEM_DC_FILTER_OK &&
		      filter.phase == SIPPYMODEM_DC_FILTER_CALIBRATION &&
		      sippymodem_dc_filter_offset(&filter) == 0,
	      "DC filter did not finish its warmup exactly");
	CHECK(feed_constant(&filter, 1200, CALIBRATION_SAMPLES, 127) ==
			      SIPPYMODEM_DC_FILTER_OK &&
		      filter.phase == SIPPYMODEM_DC_FILTER_TRACKING &&
		      sippymodem_dc_filter_offset(&filter) == 1200,
	      "DC filter did not establish the initial offset");
	CHECK(sippymodem_dc_filter_process(&filter, &sample, 1) ==
			      SIPPYMODEM_DC_FILTER_OK &&
		      sample == 100,
	      "DC filter did not subtract the calibrated offset");
	CHECK(feed_constant(&filter, 2200, TRACKING_SAMPLES - 1, 160) ==
			      SIPPYMODEM_DC_FILTER_OK &&
		      sippymodem_dc_filter_offset(&filter) == 1300,
	      "DC filter tracking update was not deterministic");
	return true;
}

static bool test_is_chunk_invariant(void)
{
	enum { SAMPLE_COUNT = 40000 };
	struct sippymodem_dc_filter whole;
	struct sippymodem_dc_filter chunked;
	int16_t *input = malloc(SAMPLE_COUNT * sizeof(*input));
	int16_t *output = malloc(SAMPLE_COUNT * sizeof(*output));

	CHECK(input != NULL && output != NULL,
	      "DC chunk-invariance fixture allocation failed");
	for (size_t index = 0; index < SAMPLE_COUNT; ++index)
		input[index] = (int16_t)(
			(index < 16000 ? 18 : 42) + (int)(index % 23) - 11);
	memcpy(output, input, SAMPLE_COUNT * sizeof(*output));
	sippymodem_dc_filter_init(&whole);
	sippymodem_dc_filter_init(&chunked);
	CHECK(sippymodem_dc_filter_process(&whole, input, SAMPLE_COUNT) ==
			      SIPPYMODEM_DC_FILTER_OK,
	      "whole-buffer DC filtering failed");
	for (size_t offset = 0; offset < SAMPLE_COUNT; offset += 137) {
		size_t count = SAMPLE_COUNT - offset;

		if (count > 137)
			count = 137;
		CHECK(sippymodem_dc_filter_process(
			      &chunked, output + offset, count) ==
			      SIPPYMODEM_DC_FILTER_OK,
		      "chunked DC filtering failed");
	}
	CHECK(memcmp(input, output, SAMPLE_COUNT * sizeof(*input)) == 0 &&
		      whole.phase == chunked.phase &&
		      whole.sample_sum == chunked.sample_sum &&
		      whole.sample_count == chunked.sample_count &&
		      whole.offset == chunked.offset,
	      "DC filtering depends on host frame boundaries");
	free(output);
	free(input);
	return true;
}

static bool test_bounds_output_and_validates_arguments(void)
{
	struct sippymodem_dc_filter filter;
	int16_t sample = INT16_MIN;

	sippymodem_dc_filter_init(&filter);
	CHECK(feed_constant(
		      &filter, 4000, WARMUP_SAMPLES + CALIBRATION_SAMPLES, 160) ==
			      SIPPYMODEM_DC_FILTER_OFFSET_LIMIT &&
		      sippymodem_dc_filter_offset(&filter) == 4000,
	      "DC filter did not report an excessive offset");
	CHECK(sippymodem_dc_filter_process(&filter, &sample, 1) ==
			      SIPPYMODEM_DC_FILTER_OFFSET_LIMIT &&
		      sample == INT16_MIN,
	      "DC subtraction did not saturate at the sample boundary");
	CHECK(sippymodem_dc_filter_process(NULL, &sample, 1) ==
			      SIPPYMODEM_DC_FILTER_INVALID_ARGUMENT &&
		      sippymodem_dc_filter_process(&filter, NULL, 1) ==
			      SIPPYMODEM_DC_FILTER_INVALID_ARGUMENT &&
		      sippymodem_dc_filter_process(&filter, NULL, 0) ==
			      SIPPYMODEM_DC_FILTER_OFFSET_LIMIT &&
		      sippymodem_dc_filter_offset(NULL) == 0 &&
		      strcmp(sippymodem_dc_filter_result_name(
			     SIPPYMODEM_DC_FILTER_OFFSET_LIMIT),
			     "DC offset exceeds the supported limit") == 0,
	      "DC filter argument or result diagnostics were incomplete");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "calibration and tracking", test_calibrates_and_tracks_offset },
		{ "chunk invariance", test_is_chunk_invariant },
		{ "bounds and validation",
		  test_bounds_output_and_validates_arguments },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
