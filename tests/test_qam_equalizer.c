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
	LONG_ECHO_DELAY = 8,
	LONG_TRAINING_SYMBOLS = 4096,
	LONG_PAYLOAD_SYMBOLS = 1024,
};

static struct qam_point multiply(
	struct qam_point left,
	struct qam_point right)
{
	return (struct qam_point){
		.in_phase = left.in_phase * right.in_phase -
			    left.quadrature * right.quadrature,
		.quadrature = left.in_phase * right.quadrature +
			      left.quadrature * right.in_phase,
	};
}

static bool points_are_close(
	const struct qam_point *left,
	const struct qam_point *right)
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
	const struct qam_point *equalized,
	const struct qam_point *target)
{
	return nearest_level(equalized->in_phase) == target->in_phase &&
	       nearest_level(equalized->quadrature) == target->quadrature;
}

static bool test_delay_and_flat_channel(void)
{
	const struct qam_point gain = { 0.8, -0.3 };
	const struct qam_point reference = { 1.0, 1.0 };
	struct qam_equalizer equalizer;
	struct qam_point output = { 17.0, -23.0 };
	struct qam_point saved_output = output;
	struct qam_point received_reference =
		multiply(gain, reference);
	bool ready = true;

	CHECK(qam_equalizer_init(
		      &equalizer, &received_reference, &reference) ==
		      QAM_EQUALIZER_OK &&
	      qam_equalizer_is_valid(&equalizer),
	      "flat equalizer fixture did not initialize");
	for (unsigned int index = 0;
	     index < QAM_EQUALIZER_DELAY; ++index) {
		struct qam_point input = {
			.in_phase = (double)index + 1.0,
			.quadrature = 1.0 - (double)index,
		};
		struct qam_point received = multiply(gain, input);

		CHECK(qam_equalizer_push(
			      &equalizer, &received, &output, &ready) ==
			      QAM_EQUALIZER_OK &&
		      !ready && points_are_close(&output, &saved_output),
		      "equalizer exposed delayed symbol %u early", index);
	}
	{
		struct qam_point input = { -2.0, 0.5 };
		struct qam_point received = multiply(gain, input);

		CHECK(qam_equalizer_push(
			      &equalizer, &received, &output, &ready) ==
			      QAM_EQUALIZER_OK &&
		      ready &&
		      points_are_close(
			      &output,
			      &(struct qam_point){ 1.0, 1.0 }) &&
		      qam_equalizer_adapt(
			      &equalizer, &output, 0.0) ==
			      QAM_EQUALIZER_OK &&
		      qam_equalizer_adapt(
			      &equalizer, &output, 0.0) ==
			      QAM_EQUALIZER_INVALID_STATE,
		      "equalizer delay or single-adaptation contract changed");
	}
	return true;
}

static bool test_generated_isi_channel(void)
{
	static const struct qam_point channel[CHANNEL_TAPS] = {
		{ 1.0, 0.25 },
		{ 0.28, -0.18 },
		{ -0.12, 0.09 },
		{ 0.05, 0.03 },
	};
	const struct qam_point reference = { 1.0, 1.0 };
	struct qam_point transmitted[
		QAM_EQUALIZER_MAX_TAP_COUNT] = { 0 };
	struct qam_point received_reference =
		multiply(channel[0], reference);
	struct qam_equalizer baseline;
	struct qam_equalizer trained;
	uint32_t random = UINT32_C(0x51414d45);
	unsigned int baseline_errors = 0;
	unsigned int trained_errors = 0;

	CHECK(qam_equalizer_init(
		      &baseline, &received_reference, &reference) ==
		      QAM_EQUALIZER_OK &&
	      qam_equalizer_init(
		      &trained, &received_reference, &reference) ==
		      QAM_EQUALIZER_OK,
	      "ISI equalizer fixture did not initialize");
	for (unsigned int symbol = 0;
	     symbol < TRAINING_SYMBOLS + PAYLOAD_SYMBOLS; ++symbol) {
		struct qam_point baseline_output = { 0 };
		struct qam_point trained_output = { 0 };
		struct qam_point received = { 0 };
		bool baseline_ready;
		bool trained_ready;

		for (size_t index = ARRAY_LENGTH(transmitted) - 1;
		     index != 0; --index)
			transmitted[index] = transmitted[index - 1];
		transmitted[0] = (struct qam_point){
			.in_phase = random_level(&random),
			.quadrature = random_level(&random),
		};
		for (size_t index = 0; index < ARRAY_LENGTH(channel); ++index) {
			struct qam_point contribution =
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
		CHECK(qam_equalizer_push(
			      &baseline, &received, &baseline_output,
			      &baseline_ready) == QAM_EQUALIZER_OK &&
		      qam_equalizer_push(
			      &trained, &received, &trained_output,
			      &trained_ready) == QAM_EQUALIZER_OK &&
		      baseline_ready == trained_ready,
		      "ISI equalizer rejected generated symbol %u", symbol);
		if (!trained_ready)
			continue;
		if (symbol < TRAINING_SYMBOLS) {
			CHECK(qam_equalizer_adapt(
				      &trained,
				      &transmitted[
					      QAM_EQUALIZER_DELAY],
				      0.2) == QAM_EQUALIZER_OK,
			      "ISI training failed at symbol %u", symbol);
			continue;
		}
		baseline_errors += !same_decision(
			&baseline_output,
			&transmitted[QAM_EQUALIZER_DELAY]);
		trained_errors += !same_decision(
			&trained_output,
			&transmitted[QAM_EQUALIZER_DELAY]);
		{
			struct qam_point decision = {
				.in_phase = nearest_level(trained_output.in_phase),
				.quadrature = nearest_level(
					trained_output.quadrature),
			};

			CHECK(qam_equalizer_adapt(
				      &trained, &decision, 0.02) ==
				      QAM_EQUALIZER_OK,
			      "decision tracking failed at symbol %u", symbol);
		}
	}
	CHECK(baseline_errors > PAYLOAD_SYMBOLS / 5 && trained_errors == 0,
	      "adaptive equalizer errors were %u versus %u without adaptation",
	      trained_errors, baseline_errors);
	return true;
}

static bool test_instance_selected_span(void)
{
	const struct qam_point reference = { 1.0, 1.0 };
	struct qam_point transmitted[QAM_EQUALIZER_MAX_TAP_COUNT] = { 0 };
	struct qam_equalizer short_span;
	struct qam_equalizer long_span;
	uint32_t random = UINT32_C(0x5350414e);
	unsigned int short_errors = 0;
	unsigned int long_errors = 0;

	CHECK(qam_equalizer_init(
		      &short_span, &reference, &reference) ==
		      QAM_EQUALIZER_OK &&
	      qam_equalizer_init_span(
		      &long_span, &reference, &reference,
		      QAM_EQUALIZER_MAX_TAP_COUNT) == QAM_EQUALIZER_OK &&
	      short_span.tap_count == QAM_EQUALIZER_DEFAULT_TAP_COUNT &&
	      long_span.tap_count == QAM_EQUALIZER_MAX_TAP_COUNT,
	      "equalizer spans did not initialize independently");
	for (unsigned int symbol = 0;
	     symbol < LONG_TRAINING_SYMBOLS + LONG_PAYLOAD_SYMBOLS;
	     ++symbol) {
		struct qam_point short_output = { 0 };
		struct qam_point long_output = { 0 };
		struct qam_point received;
		bool short_ready;
		bool long_ready;

		for (size_t index = ARRAY_LENGTH(transmitted) - 1u;
		     index != 0; --index)
			transmitted[index] = transmitted[index - 1u];
		transmitted[0] = (struct qam_point){
			.in_phase = random_level(&random),
			.quadrature = random_level(&random),
		};
		received = transmitted[0];
		received.in_phase +=
			0.35 * transmitted[LONG_ECHO_DELAY].in_phase;
		received.quadrature +=
			0.35 * transmitted[LONG_ECHO_DELAY].quadrature;
		CHECK(qam_equalizer_push(
			      &short_span, &received, &short_output,
			      &short_ready) == QAM_EQUALIZER_OK &&
		      qam_equalizer_push(
			      &long_span, &received, &long_output,
			      &long_ready) == QAM_EQUALIZER_OK &&
		      short_ready == long_ready,
		      "span readiness diverged at symbol %u", symbol);
		if (!long_ready)
			continue;
		if (symbol < LONG_TRAINING_SYMBOLS) {
			CHECK(qam_equalizer_adapt(
				      &short_span,
				      &transmitted[QAM_EQUALIZER_DELAY],
				      0.3) == QAM_EQUALIZER_OK &&
			      qam_equalizer_adapt(
				      &long_span,
				      &transmitted[QAM_EQUALIZER_DELAY],
				      0.3) == QAM_EQUALIZER_OK,
			      "span training failed at symbol %u", symbol);
			continue;
		}
		short_errors += !same_decision(
			&short_output, &transmitted[QAM_EQUALIZER_DELAY]);
		long_errors += !same_decision(
			&long_output, &transmitted[QAM_EQUALIZER_DELAY]);
	}
	CHECK(short_errors > LONG_PAYLOAD_SYMBOLS / 8u && long_errors == 0,
	      "long echo produced %u errors with the selected span versus %u "
	      "with the default",
	      long_errors, short_errors);
	{
		struct qam_equalizer_observation observation = { 0 };
		struct qam_equalizer saved_long = long_span;
		struct qam_equalizer_observation saved_observation;
		bool ready;

		CHECK(qam_equalizer_observe(
			      &short_span, &transmitted[0], &observation,
			      &ready) == QAM_EQUALIZER_OK && ready,
		      "short-span observation was not retained");
		saved_observation = observation;
		CHECK(qam_equalizer_adapt_observation(
			      &long_span, &observation,
			      &transmitted[QAM_EQUALIZER_DELAY], 0.1) ==
			      QAM_EQUALIZER_INVALID_STATE &&
		      memcmp(&long_span, &saved_long, sizeof(long_span)) == 0 &&
		      memcmp(&observation, &saved_observation,
			     sizeof(observation)) == 0,
		      "cross-span observation rejection was not transactional");
	}
	return true;
}

static bool test_delayed_observations(void)
{
	static const struct qam_point transmitted[] = {
		{ 1.0, 1.0 },
		{ -3.0, 1.0 },
		{ 3.0, -1.0 },
		{ -1.0, -3.0 },
		{ 1.0, 3.0 },
	};
	const struct qam_point gain = { 0.8, -0.3 };
	const struct qam_point invalid = { NAN, 0.0 };
	struct qam_equalizer_observation observation[2] = { 0 };
	struct qam_point received_reference =
		multiply(gain, transmitted[0]);
	struct qam_equalizer equalizer;
	bool ready = true;

	CHECK(ARRAY_LENGTH(transmitted) ==
		      QAM_EQUALIZER_DELAY + 2u &&
	      qam_equalizer_init(
		      &equalizer, &received_reference, &transmitted[0]) ==
		      QAM_EQUALIZER_OK,
	      "delayed observation fixture did not initialize");
	for (size_t index = 0; index < ARRAY_LENGTH(transmitted); ++index) {
		struct qam_point received =
			multiply(gain, transmitted[index]);
		struct qam_equalizer_observation *destination =
			index <= QAM_EQUALIZER_DELAY ?
				&observation[0] :
				&observation[1];

		CHECK(qam_equalizer_observe(
			      &equalizer, &received, destination, &ready) ==
			      QAM_EQUALIZER_OK &&
		      ready == (index >= QAM_EQUALIZER_DELAY),
		      "delayed observation push %zu changed readiness", index);
	}
	CHECK(qam_equalizer_observation_is_pending(
		      &observation[0]) &&
	      qam_equalizer_observation_is_pending(
		      &observation[1]) &&
	      points_are_close(&observation[0].output, &transmitted[0]) &&
	      points_are_close(&observation[1].output, &transmitted[1]) &&
	      qam_equalizer_adapt(
		      &equalizer, &transmitted[1], 0.2) ==
		      QAM_EQUALIZER_INVALID_STATE,
	      "delayed observations did not retain both outputs");
	{
		struct qam_equalizer saved_equalizer = equalizer;
		struct qam_equalizer_observation saved =
			observation[1];
		struct qam_point received =
			multiply(gain, transmitted[2]);

		CHECK(qam_equalizer_observe(
			      &equalizer, &received, &observation[1], &ready) ==
			      QAM_EQUALIZER_INVALID_STATE &&
		      qam_equalizer_adapt_observation(
			      &equalizer, &observation[1], &invalid, 0.2) ==
			      QAM_EQUALIZER_INVALID_ARGUMENT &&
		      memcmp(&equalizer, &saved_equalizer,
			     sizeof(equalizer)) == 0 &&
		      memcmp(&observation[1], &saved, sizeof(saved)) == 0,
		      "pending observation rejection was not transactional");
	}
	{
		struct qam_equalizer before = equalizer;
		struct qam_point adjusted = {
			.in_phase = transmitted[0].in_phase + 0.25,
			.quadrature = transmitted[0].quadrature - 0.125,
		};

		CHECK(qam_equalizer_adapt_observation(
			      &equalizer, &observation[0], &adjusted, 0.2) ==
			      QAM_EQUALIZER_OK &&
		      !qam_equalizer_observation_is_pending(
			      &observation[0]) &&
		      memcmp(&equalizer.coefficients, &before.coefficients,
			     sizeof(equalizer.coefficients)) != 0 &&
		      qam_equalizer_adapt_observation(
			      &equalizer, &observation[0], &adjusted, 0.2) ==
			      QAM_EQUALIZER_INVALID_STATE &&
		      qam_equalizer_adapt_observation(
			      &equalizer, &observation[1], &transmitted[1], 0.2) ==
			      QAM_EQUALIZER_OK &&
		      !qam_equalizer_observation_is_pending(
			      &observation[1]),
		      "delayed observations were not consumed exactly once");
	}
	CHECK(!qam_equalizer_observation_is_pending(NULL) &&
	      qam_equalizer_observe(
		      NULL, &transmitted[0], &observation[0], &ready) ==
		      QAM_EQUALIZER_INVALID_ARGUMENT,
	      "invalid delayed observation arguments were accepted");
	return true;
}

static bool test_validation(void)
{
	const struct qam_point reference = { 1.0, -1.0 };
	const struct qam_point zero = { 0 };
	const struct qam_point invalid = { NAN, 0.0 };
	struct qam_equalizer equalizer = { 0 };
	struct qam_equalizer saved;
	struct qam_point output = { 4.0, 5.0 };
	struct qam_point saved_output = output;
	bool ready = true;
	bool saved_ready = ready;

	CHECK(qam_equalizer_init(
		      &equalizer, &reference, &reference) ==
		      QAM_EQUALIZER_OK,
	      "validation equalizer did not initialize");
	saved = equalizer;
	CHECK(qam_equalizer_init(NULL, &reference, &reference) ==
		      QAM_EQUALIZER_INVALID_ARGUMENT &&
	      qam_equalizer_init_span(
		      &equalizer, &reference, &reference,
		      QAM_EQUALIZER_DELAY) == QAM_EQUALIZER_INVALID_ARGUMENT &&
	      qam_equalizer_init_span(
		      &equalizer, &reference, &reference,
		      QAM_EQUALIZER_MAX_TAP_COUNT + 1u) ==
		      QAM_EQUALIZER_INVALID_ARGUMENT &&
	      qam_equalizer_init(&equalizer, &zero, &reference) ==
		      QAM_EQUALIZER_INVALID_ARGUMENT &&
	      qam_equalizer_init(&equalizer, &invalid, &reference) ==
		      QAM_EQUALIZER_INVALID_ARGUMENT &&
	      qam_equalizer_push(
		      &equalizer, &invalid, &output, &ready) ==
		      QAM_EQUALIZER_INVALID_ARGUMENT &&
	      qam_equalizer_push(
		      &equalizer, &reference, NULL, &ready) ==
		      QAM_EQUALIZER_INVALID_ARGUMENT &&
	      qam_equalizer_adapt(&equalizer, &reference, 0.2) ==
		      QAM_EQUALIZER_INVALID_STATE &&
	      memcmp(&equalizer, &saved, sizeof(equalizer)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0 &&
	      ready == saved_ready,
	      "invalid equalizer operation was not transactional");
	equalizer.coefficients[0].in_phase = INFINITY;
	CHECK(!qam_equalizer_is_valid(&equalizer) &&
	      !qam_equalizer_is_valid(NULL) &&
	      strcmp(qam_equalizer_result_name(
		     QAM_EQUALIZER_OK),
		     "ok") == 0 &&
	      strcmp(qam_equalizer_result_name(
		     QAM_EQUALIZER_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(qam_equalizer_result_name(
		     QAM_EQUALIZER_INVALID_STATE),
		     "invalid state") == 0 &&
	      strcmp(qam_equalizer_result_name(
		     (enum qam_equalizer_result)99),
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
		{ "instance-selected channel span", test_instance_selected_span },
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
