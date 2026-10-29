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
			quality, rms * rms * V92_PCM_MAPPING_FRAME_SYMBOLS, false, false);

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
		      quality.recovery.bad_windows == 0, "ceiling requested recovery");
	for (unsigned int n = 0; n < V92_DATA_QUALITY_BAD_WINDOWS - 1u; ++n)
		CHECK(window(&quality, 2 * V92_DATA_MAX_RMS_ERROR, V92_DATA_QUALITY_OK),
		      "short disturbance triggered recovery");
	CHECK(window(&quality, 0, V92_DATA_QUALITY_OK) && quality.recovery.bad_windows == 0,
	      "clean window did not break consecutive loss");
	for (unsigned int attempt = 0; attempt <= V92_DATA_QUALITY_MAX_RETRAINS; ++attempt) {
		for (unsigned int n = 0; n < V92_DATA_QUALITY_BAD_WINDOWS; ++n)
			CHECK(window(&quality, 2 * V92_DATA_MAX_RMS_ERROR,
				n + 1u != V92_DATA_QUALITY_BAD_WINDOWS ? V92_DATA_QUALITY_OK :
				attempt == V92_DATA_QUALITY_MAX_RETRAINS ? V92_DATA_QUALITY_EXHAUSTED :
				V92_DATA_QUALITY_RETRAIN), "attempt %u failed", attempt);
		if (attempt == V92_DATA_QUALITY_MAX_RETRAINS)
			break;
		CHECK(quality.recovery.attempts == attempt + 1u &&
		      quality.frames == 0 && quality.squared_error == 0 &&
		      quality.recovery.bad_windows == 0 && v92_data_quality_restart(&quality) &&
		      quality.recovery.attempts == attempt + 1u,
		      "reconnect lost attempt budget");
	}
	return true;
}

static bool test_ambiguity(void)
{
	struct v92_data_quality quality = { 0 };

	for (unsigned int attempt = 0; attempt < V92_DATA_QUALITY_MAX_RETRAINS; ++attempt) {
		CHECK(v92_data_quality_observe(&quality, 0, false, false) == V92_DATA_QUALITY_OK &&
		      v92_data_quality_observe(&quality, 0, true, false) == V92_DATA_QUALITY_RETRAIN &&
		      quality.recovery.attempts == attempt + 1u &&
		      quality.frames == 0 && v92_data_quality_restart(&quality),
		      "ambiguity did not request immediate bounded recovery");
	}
	struct v92_data_quality before = quality;
	CHECK(v92_data_quality_observe(&quality, 0, true, false) == V92_DATA_QUALITY_EXHAUSTED &&
	      memcmp(&before, &quality, sizeof(quality)) == 0,
	      "exhausted ambiguity replenished budget");
	CHECK(v92_data_quality_observe(&quality, 100, false, false) == V92_DATA_QUALITY_OK &&
	      v92_data_quality_restart(&quality) && quality.frames == 0 &&
	      quality.squared_error == 0 && quality.recovery.attempts == 2,
	      "restart retained measurement or replenished budget");
	return true;
}

static bool test_invalid(void)
{
	struct v92_data_quality quality = { 0 }, before;
	const double invalid[] = { -1.0, NAN, INFINITY, -INFINITY };

	CHECK(!v92_data_quality_is_valid(NULL) && !v92_data_quality_restart(NULL) &&
	      v92_data_quality_retrain(NULL) == V92_DATA_QUALITY_INVALID_ARGUMENT &&
	      v92_data_quality_observe(NULL, 0, false, false) == V92_DATA_QUALITY_INVALID_ARGUMENT,
	      "null state accepted");
	for (unsigned int n = 0; n < sizeof(invalid) / sizeof(invalid[0]); ++n) {
		before = quality;
		CHECK(v92_data_quality_observe(&quality, invalid[n], true, false) ==
		      V92_DATA_QUALITY_INVALID_ARGUMENT &&
		      memcmp(&before, &quality, sizeof(quality)) == 0,
		      "invalid metric changed state");
	}
	quality.squared_error = DBL_MAX;
	quality.frames = 1;
	before = quality;
	CHECK(v92_data_quality_observe(&quality, DBL_MAX, false, false) ==
	      V92_DATA_QUALITY_INVALID_ARGUMENT && memcmp(&before, &quality, sizeof(quality)) == 0,
	      "metric overflow changed state");
	quality.frames = V92_DATA_QUALITY_WINDOW_FRAMES;
	before = quality;
	CHECK(!v92_data_quality_is_valid(&quality) && !v92_data_quality_restart(&quality) &&
	      v92_data_quality_retrain(&quality) == V92_DATA_QUALITY_INVALID_ARGUMENT &&
	      v92_data_quality_observe(&quality, 0, false, false) == V92_DATA_QUALITY_INVALID_ARGUMENT &&
	      memcmp(&before, &quality, sizeof(quality)) == 0, "invalid window accepted");
	quality = (struct v92_data_quality){ .recovery.attempts = 3 };
	CHECK(!v92_data_quality_is_valid(&quality), "invalid budget accepted");
	quality = (struct v92_data_quality){ .recovery.bad_windows = V92_DATA_QUALITY_BAD_WINDOWS };
	CHECK(!v92_data_quality_is_valid(&quality), "invalid loss count accepted");
	quality = (struct v92_data_quality){ .squared_error = 1 };
	CHECK(!v92_data_quality_is_valid(&quality), "unowned measurement accepted");
	quality = (struct v92_data_quality){ .uncertain_window = true };
	CHECK(!v92_data_quality_is_valid(&quality), "unowned confidence accepted");
	quality = (struct v92_data_quality){
		.recovery.attempts = 1, .recovery.good_windows = V92_DATA_QUALITY_REARM_WINDOWS,
	};
	CHECK(!v92_data_quality_is_valid(&quality), "unbounded healthy credit accepted");
	quality = (struct v92_data_quality){ .recovery.good_windows = 1 };
	CHECK(!v92_data_quality_is_valid(&quality), "healthy credit without used attempts accepted");
	quality = (struct v92_data_quality){ .recovery.attempts = 1, .recovery.good_windows = 1, .recovery.bad_windows = 1 };
	CHECK(!v92_data_quality_is_valid(&quality), "concurrent good and bad windows accepted");
	return true;
}

static bool test_training_recovery(void)
{
	struct v92_data_quality quality = { .frames = 1, .squared_error = 100.0 };

	CHECK(v92_data_quality_retrain(&quality) == V92_DATA_QUALITY_RETRAIN &&
	      quality.recovery.attempts == 1u && quality.frames == 0u && quality.squared_error == 0.0 &&
	      v92_data_quality_restart(&quality) &&
	      v92_data_quality_observe(&quality, 0, true, false) == V92_DATA_QUALITY_RETRAIN &&
	      quality.recovery.attempts == V92_DATA_QUALITY_MAX_RETRAINS,
	      "training and data failures did not share their recovery budget");
	struct v92_data_quality before = quality;
	CHECK(v92_data_quality_retrain(&quality) == V92_DATA_QUALITY_EXHAUSTED &&
	      memcmp(&quality, &before, sizeof(quality)) == 0,
	      "exhausted training recovery changed state");
	return true;
}

static bool test_uncertainty(void)
{
	struct v92_data_quality quality = { 0 };
	bool uncertain[3 * V92_DATA_QUALITY_BAD_WINDOWS];
	unsigned bad = 0;

	/* Two interrupted runs must not combine into one persistent fault. */
	for (unsigned n = 0; n < ARRAY_LENGTH(uncertain); ++n)
		uncertain[n] = true;
	uncertain[V92_DATA_QUALITY_BAD_WINDOWS - 1u] = false;
	uncertain[2 * V92_DATA_QUALITY_BAD_WINDOWS - 1u] = false;
	/* One disputed observation makes a weak window, not a payload erasure
	 * or an immediate retrain. Good windows break the consecutive run. */
	for (unsigned n = 0; n < ARRAY_LENGTH(uncertain); ++n) {
		for (unsigned f = 0; f < V92_DATA_QUALITY_WINDOW_FRAMES; ++f) {
			bool weak = uncertain[n] && f == V92_DATA_QUALITY_WINDOW_FRAMES / 2u;
			enum v92_data_quality_result expected = n + 1u == ARRAY_LENGTH(uncertain) &&
				f + 1u == V92_DATA_QUALITY_WINDOW_FRAMES ?
				V92_DATA_QUALITY_RETRAIN : V92_DATA_QUALITY_OK;

			CHECK(v92_data_quality_observe(&quality, 0, false, weak) == expected,
				"uncertainty recovered at the wrong boundary %u/%u", n, f);
		}
		bad = uncertain[n] ? bad + 1u : 0u;
		if (bad == V92_DATA_QUALITY_BAD_WINDOWS) bad = 0;
		CHECK(quality.recovery.bad_windows == bad && !quality.uncertain_window &&
			v92_data_quality_is_valid(&quality), "uncertainty window did not clear");
	}
	CHECK(quality.recovery.attempts == 1u &&
		v92_data_quality_observe(&quality, 0, false, true) == V92_DATA_QUALITY_OK &&
		v92_data_quality_restart(&quality) && !quality.uncertain_window &&
		quality.recovery.attempts == 1u, "restart lost confidence ownership or retry budget");
	return true;
}

static bool test_healthy_rearm(void)
{
	for (unsigned budget = 1; budget <= V92_DATA_QUALITY_MAX_RETRAINS; ++budget) {
		struct v92_data_quality quality = { .recovery.attempts = budget };

		for (unsigned n = 0; n < V92_DATA_QUALITY_REARM_WINDOWS; ++n) {
			CHECK(window(&quality, V92_DATA_MAX_RMS_ERROR, V92_DATA_QUALITY_OK),
			      "clean reception failed");
			CHECK(quality.recovery.attempts ==
			      (n + 1u == V92_DATA_QUALITY_REARM_WINDOWS ? 0u : budget),
			      "healthy reception rearmed at wrong window %u", n);
		}
		CHECK(quality.recovery.good_windows == 0 && v92_data_quality_is_valid(&quality),
		      "rearmed state retained healthy credit");
		for (unsigned n = 0; n < V92_DATA_QUALITY_MAX_RETRAINS; ++n)
			CHECK(v92_data_quality_observe(&quality, 0, true, false) == V92_DATA_QUALITY_RETRAIN &&
			      quality.recovery.attempts == n + 1u && v92_data_quality_restart(&quality),
			      "rearmed recovery did not retain its bound");
		CHECK(v92_data_quality_observe(&quality, 0, true, false) == V92_DATA_QUALITY_EXHAUSTED,
		      "continuous failure escaped rearmed limit");
	}
	return true;
}

static bool test_interrupted_healthy_credit(void)
{
	for (unsigned interruption = 0; interruption < 3; ++interruption) {
		struct v92_data_quality quality = { .recovery.attempts = V92_DATA_QUALITY_MAX_RETRAINS };

		for (unsigned n = 1; n < V92_DATA_QUALITY_REARM_WINDOWS; ++n)
			CHECK(window(&quality, 0, V92_DATA_QUALITY_OK), "healthy setup failed");
		CHECK(quality.recovery.good_windows == V92_DATA_QUALITY_REARM_WINDOWS - 1u,
		      "healthy credit was not retained");
		if (interruption == 0) {
			CHECK(window(&quality, 2 * V92_DATA_MAX_RMS_ERROR, V92_DATA_QUALITY_OK),
			      "isolated noisy window forced recovery");
		} else if (interruption == 1) {
			for (unsigned f = 0; f < V92_DATA_QUALITY_WINDOW_FRAMES; ++f)
				CHECK(v92_data_quality_observe(&quality, 0, false, f == 0) == V92_DATA_QUALITY_OK,
				      "isolated uncertain window forced recovery");
		} else {
			CHECK(v92_data_quality_restart(&quality), "restart failed");
		}
		CHECK(quality.recovery.good_windows == 0 &&
		      quality.recovery.attempts == V92_DATA_QUALITY_MAX_RETRAINS &&
		      window(&quality, 0, V92_DATA_QUALITY_OK) && quality.recovery.good_windows == 1,
		      "interruption retained healthy credit or replenished attempts");
	}
	return true;
}

static bool test_ambiguous_rearm_boundary(void)
{
	struct v92_data_quality quality = { .recovery.attempts = V92_DATA_QUALITY_MAX_RETRAINS };

	for (unsigned n = 1; n < V92_DATA_QUALITY_REARM_WINDOWS; ++n)
		CHECK(window(&quality, 0, V92_DATA_QUALITY_OK), "healthy setup failed");
	for (unsigned f = 1; f < V92_DATA_QUALITY_WINDOW_FRAMES; ++f)
		CHECK(v92_data_quality_observe(&quality, 0, false, false) == V92_DATA_QUALITY_OK &&
		      quality.recovery.attempts == V92_DATA_QUALITY_MAX_RETRAINS,
		      "partial clean window replenished attempts");
	struct v92_data_quality before = quality;
	CHECK(v92_data_quality_observe(&quality, 0, true, false) == V92_DATA_QUALITY_EXHAUSTED &&
	      memcmp(&quality, &before, sizeof(quality)) == 0,
	      "ambiguous final frame rearmed recovery or changed exhausted state");
	CHECK(v92_data_quality_observe(&quality, 0, false, false) == V92_DATA_QUALITY_OK &&
	      quality.recovery.attempts == 0 && quality.recovery.good_windows == 0,
	      "complete clean window did not rearm recovery");
	return true;
}

int main(void)
{
	return test_windows() && test_ambiguity() && test_uncertainty() && test_training_recovery() &&
		test_healthy_rearm() && test_interrupted_healthy_credit() &&
		test_ambiguous_rearm_boundary() && test_invalid() ? 0 : 1;
}
