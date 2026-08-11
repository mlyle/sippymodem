// SPDX-License-Identifier: BSD-3-Clause

#include <qam_equalizer.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	TRAINING_SYMBOLS = 1024,
	PAYLOAD_SYMBOLS = 576,
	CHANNEL_TAPS = 4,
};

static struct sippymodem_qam_point multiply(
	struct sippymodem_qam_point left,
	struct sippymodem_qam_point right)
{
	return (struct sippymodem_qam_point){
		.in_phase = left.in_phase * right.in_phase -
			    left.quadrature * right.quadrature,
		.quadrature = left.in_phase * right.quadrature +
			      left.quadrature * right.in_phase,
	};
}

static bool points_are_close(
	const struct sippymodem_qam_point *left,
	const struct sippymodem_qam_point *right)
{
	return fabs(left->in_phase - right->in_phase) < 1e-11 &&
	       fabs(left->quadrature - right->quadrature) < 1e-11;
}

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static double random_level(uint32_t *random)
{
	static const double levels[] = { -3.0, -1.0, 1.0, 3.0 };

	return levels[(fixture_random(random) >> 28) & 3u];
}

static double nearest_level(double value)
{
	if (value < -2.0)
		return -3.0;
	if (value < 0.0)
		return -1.0;
	if (value < 2.0)
		return 1.0;
	return 3.0;
}

static bool same_decision(
	const struct sippymodem_qam_point *equalized,
	const struct sippymodem_qam_point *target)
{
	return nearest_level(equalized->in_phase) == target->in_phase &&
	       nearest_level(equalized->quadrature) == target->quadrature;
}

static bool test_delay_and_flat_channel(void)
{
	const struct sippymodem_qam_point gain = { 0.8, -0.3 };
	const struct sippymodem_qam_point reference = { 1.0, 1.0 };
	struct sippymodem_qam_equalizer equalizer;
	struct sippymodem_qam_point output = { 17.0, -23.0 };
	struct sippymodem_qam_point saved_output = output;
	struct sippymodem_qam_point received_reference =
		multiply(gain, reference);
	bool ready = true;

	CHECK(sippymodem_qam_equalizer_init(
		      &equalizer, &received_reference, &reference) ==
		      SIPPYMODEM_QAM_EQUALIZER_OK &&
	      sippymodem_qam_equalizer_is_valid(&equalizer),
	      "flat equalizer fixture did not initialize");
	for (unsigned int index = 0;
	     index < SIPPYMODEM_QAM_EQUALIZER_DELAY; ++index) {
		struct sippymodem_qam_point input = {
			.in_phase = (double)index + 1.0,
			.quadrature = 1.0 - (double)index,
		};
		struct sippymodem_qam_point received = multiply(gain, input);

		CHECK(sippymodem_qam_equalizer_push(
			      &equalizer, &received, &output, &ready) ==
			      SIPPYMODEM_QAM_EQUALIZER_OK &&
		      !ready && points_are_close(&output, &saved_output),
		      "equalizer exposed delayed symbol %u early", index);
	}
	{
		struct sippymodem_qam_point input = { -2.0, 0.5 };
		struct sippymodem_qam_point received = multiply(gain, input);

		CHECK(sippymodem_qam_equalizer_push(
			      &equalizer, &received, &output, &ready) ==
			      SIPPYMODEM_QAM_EQUALIZER_OK &&
		      ready &&
		      points_are_close(
			      &output,
			      &(struct sippymodem_qam_point){ 1.0, 1.0 }) &&
		      sippymodem_qam_equalizer_adapt(
			      &equalizer, &output, 0.0) ==
			      SIPPYMODEM_QAM_EQUALIZER_OK &&
		      sippymodem_qam_equalizer_adapt(
			      &equalizer, &output, 0.0) ==
			      SIPPYMODEM_QAM_EQUALIZER_INVALID_STATE,
		      "equalizer delay or single-adaptation contract changed");
	}
	return true;
}

static bool test_generated_isi_channel(void)
{
	static const struct sippymodem_qam_point channel[CHANNEL_TAPS] = {
		{ 1.0, 0.25 },
		{ 0.28, -0.18 },
		{ -0.12, 0.09 },
		{ 0.05, 0.03 },
	};
	const struct sippymodem_qam_point reference = { 1.0, 1.0 };
	struct sippymodem_qam_point transmitted[
		SIPPYMODEM_QAM_EQUALIZER_TAP_COUNT] = { 0 };
	struct sippymodem_qam_point received_reference =
		multiply(channel[0], reference);
	struct sippymodem_qam_equalizer baseline;
	struct sippymodem_qam_equalizer trained;
	uint32_t random = UINT32_C(0x51414d45);
	unsigned int baseline_errors = 0;
	unsigned int trained_errors = 0;

	CHECK(sippymodem_qam_equalizer_init(
		      &baseline, &received_reference, &reference) ==
		      SIPPYMODEM_QAM_EQUALIZER_OK &&
	      sippymodem_qam_equalizer_init(
		      &trained, &received_reference, &reference) ==
		      SIPPYMODEM_QAM_EQUALIZER_OK,
	      "ISI equalizer fixture did not initialize");
	for (unsigned int symbol = 0;
	     symbol < TRAINING_SYMBOLS + PAYLOAD_SYMBOLS; ++symbol) {
		struct sippymodem_qam_point baseline_output = { 0 };
		struct sippymodem_qam_point trained_output = { 0 };
		struct sippymodem_qam_point received = { 0 };
		bool baseline_ready;
		bool trained_ready;

		for (size_t index = ARRAY_LENGTH(transmitted) - 1;
		     index != 0; --index)
			transmitted[index] = transmitted[index - 1];
		transmitted[0] = (struct sippymodem_qam_point){
			.in_phase = random_level(&random),
			.quadrature = random_level(&random),
		};
		for (size_t index = 0; index < ARRAY_LENGTH(channel); ++index) {
			struct sippymodem_qam_point contribution =
				multiply(channel[index], transmitted[index]);

			received.in_phase += contribution.in_phase;
			received.quadrature += contribution.quadrature;
		}
		received.in_phase +=
			((double)(fixture_random(&random) & 0xffffu) / 65535.0 -
			 0.5) *
			0.01;
		received.quadrature +=
			((double)(fixture_random(&random) & 0xffffu) / 65535.0 -
			 0.5) *
			0.01;
		CHECK(sippymodem_qam_equalizer_push(
			      &baseline, &received, &baseline_output,
			      &baseline_ready) == SIPPYMODEM_QAM_EQUALIZER_OK &&
		      sippymodem_qam_equalizer_push(
			      &trained, &received, &trained_output,
			      &trained_ready) == SIPPYMODEM_QAM_EQUALIZER_OK &&
		      baseline_ready == trained_ready,
		      "ISI equalizer rejected generated symbol %u", symbol);
		if (!trained_ready)
			continue;
		if (symbol < TRAINING_SYMBOLS) {
			CHECK(sippymodem_qam_equalizer_adapt(
				      &trained,
				      &transmitted[
					      SIPPYMODEM_QAM_EQUALIZER_DELAY],
				      0.2) == SIPPYMODEM_QAM_EQUALIZER_OK,
			      "ISI training failed at symbol %u", symbol);
			continue;
		}
		baseline_errors += !same_decision(
			&baseline_output,
			&transmitted[SIPPYMODEM_QAM_EQUALIZER_DELAY]);
		trained_errors += !same_decision(
			&trained_output,
			&transmitted[SIPPYMODEM_QAM_EQUALIZER_DELAY]);
		{
			struct sippymodem_qam_point decision = {
				.in_phase = nearest_level(trained_output.in_phase),
				.quadrature = nearest_level(
					trained_output.quadrature),
			};

			CHECK(sippymodem_qam_equalizer_adapt(
				      &trained, &decision, 0.02) ==
				      SIPPYMODEM_QAM_EQUALIZER_OK,
			      "decision tracking failed at symbol %u", symbol);
		}
	}
	CHECK(baseline_errors > PAYLOAD_SYMBOLS / 5 && trained_errors == 0,
	      "adaptive equalizer errors were %u versus %u without adaptation",
	      trained_errors, baseline_errors);
	return true;
}

static bool test_delayed_observations(void)
{
	static const struct sippymodem_qam_point transmitted[] = {
		{ 1.0, 1.0 },
		{ -3.0, 1.0 },
		{ 3.0, -1.0 },
		{ -1.0, -3.0 },
		{ 1.0, 3.0 },
	};
	const struct sippymodem_qam_point gain = { 0.8, -0.3 };
	const struct sippymodem_qam_point invalid = { NAN, 0.0 };
	struct sippymodem_qam_equalizer_observation observation[2] = { 0 };
	struct sippymodem_qam_point received_reference =
		multiply(gain, transmitted[0]);
	struct sippymodem_qam_equalizer equalizer;
	bool ready = true;

	CHECK(ARRAY_LENGTH(transmitted) ==
		      SIPPYMODEM_QAM_EQUALIZER_DELAY + 2u &&
	      sippymodem_qam_equalizer_init(
		      &equalizer, &received_reference, &transmitted[0]) ==
		      SIPPYMODEM_QAM_EQUALIZER_OK,
	      "delayed observation fixture did not initialize");
	for (size_t index = 0; index < ARRAY_LENGTH(transmitted); ++index) {
		struct sippymodem_qam_point received =
			multiply(gain, transmitted[index]);
		struct sippymodem_qam_equalizer_observation *destination =
			index <= SIPPYMODEM_QAM_EQUALIZER_DELAY ?
				&observation[0] :
				&observation[1];

		CHECK(sippymodem_qam_equalizer_observe(
			      &equalizer, &received, destination, &ready) ==
			      SIPPYMODEM_QAM_EQUALIZER_OK &&
		      ready == (index >= SIPPYMODEM_QAM_EQUALIZER_DELAY),
		      "delayed observation push %zu changed readiness", index);
	}
	CHECK(sippymodem_qam_equalizer_observation_is_pending(
		      &observation[0]) &&
	      sippymodem_qam_equalizer_observation_is_pending(
		      &observation[1]) &&
	      points_are_close(&observation[0].output, &transmitted[0]) &&
	      points_are_close(&observation[1].output, &transmitted[1]) &&
	      sippymodem_qam_equalizer_adapt(
		      &equalizer, &transmitted[1], 0.2) ==
		      SIPPYMODEM_QAM_EQUALIZER_INVALID_STATE,
	      "delayed observations did not retain both outputs");
	{
		struct sippymodem_qam_equalizer saved_equalizer = equalizer;
		struct sippymodem_qam_equalizer_observation saved =
			observation[1];
		struct sippymodem_qam_point received =
			multiply(gain, transmitted[2]);

		CHECK(sippymodem_qam_equalizer_observe(
			      &equalizer, &received, &observation[1], &ready) ==
			      SIPPYMODEM_QAM_EQUALIZER_INVALID_STATE &&
		      sippymodem_qam_equalizer_adapt_observation(
			      &equalizer, &observation[1], &invalid, 0.2) ==
			      SIPPYMODEM_QAM_EQUALIZER_INVALID_ARGUMENT &&
		      memcmp(&equalizer, &saved_equalizer,
			     sizeof(equalizer)) == 0 &&
		      memcmp(&observation[1], &saved, sizeof(saved)) == 0,
		      "pending observation rejection was not transactional");
	}
	{
		struct sippymodem_qam_equalizer before = equalizer;
		struct sippymodem_qam_point adjusted = {
			.in_phase = transmitted[0].in_phase + 0.25,
			.quadrature = transmitted[0].quadrature - 0.125,
		};

		CHECK(sippymodem_qam_equalizer_adapt_observation(
			      &equalizer, &observation[0], &adjusted, 0.2) ==
			      SIPPYMODEM_QAM_EQUALIZER_OK &&
		      !sippymodem_qam_equalizer_observation_is_pending(
			      &observation[0]) &&
		      memcmp(&equalizer.coefficients, &before.coefficients,
			     sizeof(equalizer.coefficients)) != 0 &&
		      sippymodem_qam_equalizer_adapt_observation(
			      &equalizer, &observation[0], &adjusted, 0.2) ==
			      SIPPYMODEM_QAM_EQUALIZER_INVALID_STATE &&
		      sippymodem_qam_equalizer_adapt_observation(
			      &equalizer, &observation[1], &transmitted[1], 0.2) ==
			      SIPPYMODEM_QAM_EQUALIZER_OK &&
		      !sippymodem_qam_equalizer_observation_is_pending(
			      &observation[1]),
		      "delayed observations were not consumed exactly once");
	}
	CHECK(!sippymodem_qam_equalizer_observation_is_pending(NULL) &&
	      sippymodem_qam_equalizer_observe(
		      NULL, &transmitted[0], &observation[0], &ready) ==
		      SIPPYMODEM_QAM_EQUALIZER_INVALID_ARGUMENT,
	      "invalid delayed observation arguments were accepted");
	return true;
}

static bool test_validation(void)
{
	const struct sippymodem_qam_point reference = { 1.0, -1.0 };
	const struct sippymodem_qam_point zero = { 0 };
	const struct sippymodem_qam_point invalid = { NAN, 0.0 };
	struct sippymodem_qam_equalizer equalizer = { 0 };
	struct sippymodem_qam_equalizer saved;
	struct sippymodem_qam_point output = { 4.0, 5.0 };
	struct sippymodem_qam_point saved_output = output;
	bool ready = true;
	bool saved_ready = ready;

	CHECK(sippymodem_qam_equalizer_init(
		      &equalizer, &reference, &reference) ==
		      SIPPYMODEM_QAM_EQUALIZER_OK,
	      "validation equalizer did not initialize");
	saved = equalizer;
	CHECK(sippymodem_qam_equalizer_init(NULL, &reference, &reference) ==
		      SIPPYMODEM_QAM_EQUALIZER_INVALID_ARGUMENT &&
	      sippymodem_qam_equalizer_init(&equalizer, &zero, &reference) ==
		      SIPPYMODEM_QAM_EQUALIZER_INVALID_ARGUMENT &&
	      sippymodem_qam_equalizer_init(&equalizer, &invalid, &reference) ==
		      SIPPYMODEM_QAM_EQUALIZER_INVALID_ARGUMENT &&
	      sippymodem_qam_equalizer_push(
		      &equalizer, &invalid, &output, &ready) ==
		      SIPPYMODEM_QAM_EQUALIZER_INVALID_ARGUMENT &&
	      sippymodem_qam_equalizer_push(
		      &equalizer, &reference, NULL, &ready) ==
		      SIPPYMODEM_QAM_EQUALIZER_INVALID_ARGUMENT &&
	      sippymodem_qam_equalizer_adapt(&equalizer, &reference, 0.2) ==
		      SIPPYMODEM_QAM_EQUALIZER_INVALID_STATE &&
	      memcmp(&equalizer, &saved, sizeof(equalizer)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0 &&
	      ready == saved_ready,
	      "invalid equalizer operation was not transactional");
	equalizer.coefficients[0].in_phase = INFINITY;
	CHECK(!sippymodem_qam_equalizer_is_valid(&equalizer) &&
	      !sippymodem_qam_equalizer_is_valid(NULL) &&
	      strcmp(sippymodem_qam_equalizer_result_name(
		     SIPPYMODEM_QAM_EQUALIZER_OK),
		     "ok") == 0 &&
	      strcmp(sippymodem_qam_equalizer_result_name(
		     SIPPYMODEM_QAM_EQUALIZER_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(sippymodem_qam_equalizer_result_name(
		     SIPPYMODEM_QAM_EQUALIZER_INVALID_STATE),
		     "invalid state") == 0 &&
	      strcmp(sippymodem_qam_equalizer_result_name(
		     (enum sippymodem_qam_equalizer_result)99),
		     "unknown") == 0,
	      "equalizer validation or result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "decision delay and flat channel", test_delay_and_flat_channel },
		{ "generated ISI channel", test_generated_isi_channel },
		{ "delayed observations", test_delayed_observations },
		{ "validation", test_validation },
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
