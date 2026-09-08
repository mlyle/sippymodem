// SPDX-License-Identifier: BSD-3-Clause

#include <v92_data_quality.h>
#include <v92_pcm_mapping.h>

#include "test.h"

#include <float.h>
#include <math.h>
#include <string.h>

static bool window(struct v92_data_quality *quality, double rms,
		   enum v92_data_quality_result expected)
{
	for (unsigned int frame = 0; frame < V92_DATA_QUALITY_WINDOW_FRAMES; ++frame) {
		enum v92_data_quality_result result = v92_data_quality_observe(
			quality, rms * rms * V92_PCM_MAPPING_FRAME_SYMBOLS, false);

		CHECK(result == (frame + 1u == V92_DATA_QUALITY_WINDOW_FRAMES ?
			expected : V92_DATA_QUALITY_OK), "unexpected window boundary %u", frame);
	}
	return true;
}

static bool test_windows(void)
{
	struct v92_data_quality quality = { 0 };

	CHECK(v92_data_quality_is_valid(&quality), "zero state invalid");
	/* At the B1 ceiling is acceptable, not a loss window. */
	for (unsigned int n = 0; n < 4; ++n)
		CHECK(window(&quality, V92_DATA_MAX_RMS_ERROR, V92_DATA_QUALITY_OK) &&
		      quality.bad_windows == 0, "ceiling requested recovery");
	for (unsigned int n = 0; n < V92_DATA_QUALITY_BAD_WINDOWS - 1u; ++n)
		CHECK(window(&quality, 2 * V92_DATA_MAX_RMS_ERROR, V92_DATA_QUALITY_OK),
		      "short disturbance triggered recovery");
	CHECK(window(&quality, 0, V92_DATA_QUALITY_OK) && quality.bad_windows == 0,
	      "clean window did not break consecutive loss");
	for (unsigned int attempt = 0; attempt <= V92_DATA_QUALITY_MAX_RETRAINS; ++attempt) {
		for (unsigned int n = 0; n < V92_DATA_QUALITY_BAD_WINDOWS; ++n)
			CHECK(window(&quality, 2 * V92_DATA_MAX_RMS_ERROR,
				n + 1u != V92_DATA_QUALITY_BAD_WINDOWS ? V92_DATA_QUALITY_OK :
				attempt == V92_DATA_QUALITY_MAX_RETRAINS ? V92_DATA_QUALITY_EXHAUSTED :
				V92_DATA_QUALITY_RETRAIN), "attempt %u failed", attempt);
		if (attempt == V92_DATA_QUALITY_MAX_RETRAINS)
			break;
		CHECK(quality.automatic_retrains == attempt + 1u &&
		      quality.frames == 0 && quality.squared_error == 0 &&
		      quality.bad_windows == 0 && v92_data_quality_restart(&quality) &&
		      quality.automatic_retrains == attempt + 1u,
		      "reconnect lost attempt budget");
	}
	return true;
}

static bool test_ambiguity(void)
{
	struct v92_data_quality quality = { 0 };

	for (unsigned int attempt = 0; attempt < V92_DATA_QUALITY_MAX_RETRAINS; ++attempt) {
		CHECK(v92_data_quality_observe(&quality, 0, false) == V92_DATA_QUALITY_OK &&
		      v92_data_quality_observe(&quality, 0, true) == V92_DATA_QUALITY_RETRAIN &&
		      quality.automatic_retrains == attempt + 1u &&
		      quality.frames == 0 && v92_data_quality_restart(&quality),
		      "ambiguity did not request immediate bounded recovery");
	}
	struct v92_data_quality before = quality;
	CHECK(v92_data_quality_observe(&quality, 0, true) == V92_DATA_QUALITY_EXHAUSTED &&
	      memcmp(&before, &quality, sizeof(quality)) == 0,
	      "exhausted ambiguity replenished budget");
	CHECK(v92_data_quality_observe(&quality, 100, false) == V92_DATA_QUALITY_OK &&
	      v92_data_quality_restart(&quality) && quality.frames == 0 &&
	      quality.squared_error == 0 && quality.automatic_retrains == 2,
	      "restart retained measurement or replenished budget");
	return true;
}

static bool test_invalid(void)
{
	struct v92_data_quality quality = { 0 }, before;
	const double invalid[] = { -1.0, NAN, INFINITY, -INFINITY };

	CHECK(!v92_data_quality_is_valid(NULL) && !v92_data_quality_restart(NULL) &&
	      v92_data_quality_observe(NULL, 0, false) == V92_DATA_QUALITY_INVALID_ARGUMENT,
	      "null state accepted");
	for (unsigned int n = 0; n < sizeof(invalid) / sizeof(invalid[0]); ++n) {
		before = quality;
		CHECK(v92_data_quality_observe(&quality, invalid[n], true) ==
		      V92_DATA_QUALITY_INVALID_ARGUMENT &&
		      memcmp(&before, &quality, sizeof(quality)) == 0,
		      "invalid metric changed state");
	}
	quality.squared_error = DBL_MAX;
	quality.frames = 1;
	before = quality;
	CHECK(v92_data_quality_observe(&quality, DBL_MAX, false) ==
	      V92_DATA_QUALITY_INVALID_ARGUMENT && memcmp(&before, &quality, sizeof(quality)) == 0,
	      "metric overflow changed state");
	quality.frames = V92_DATA_QUALITY_WINDOW_FRAMES;
	before = quality;
	CHECK(!v92_data_quality_is_valid(&quality) && !v92_data_quality_restart(&quality) &&
	      v92_data_quality_observe(&quality, 0, false) == V92_DATA_QUALITY_INVALID_ARGUMENT &&
	      memcmp(&before, &quality, sizeof(quality)) == 0, "invalid window accepted");
	quality = (struct v92_data_quality){ .automatic_retrains = 3 };
	CHECK(!v92_data_quality_is_valid(&quality), "invalid budget accepted");
	quality = (struct v92_data_quality){ .bad_windows = 3 };
	CHECK(!v92_data_quality_is_valid(&quality), "invalid loss count accepted");
	quality = (struct v92_data_quality){ .squared_error = 1 };
	CHECK(!v92_data_quality_is_valid(&quality), "unowned measurement accepted");
	return true;
}

int main(void)
{
	return test_windows() && test_ambiguity() && test_invalid() ? 0 : 1;
}
