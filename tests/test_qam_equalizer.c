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
	CARRIER_TRACKING_SYMBOLS = 8192,
	CARRIER_TRACKING_SETTLE_SYMBOLS = 768,
};

static const double CARRIER_DRIFT_RADIANS_PER_SYMBOL = 0.01;

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

static struct qam_point rotate(
	struct qam_point point, double phase)
{
	double cosine = cos(phase);
	double sine = sin(phase);

	return (struct qam_point){
		.in_phase = point.in_phase * cosine -
			    point.quadrature * sine,
		.quadrature = point.in_phase * sine +
			      point.quadrature * cosine,
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

static bool same_quadrant(
	const struct qam_point *equalized,
	const struct qam_point *target)
{
	return (equalized->in_phase < 0.0) == (target->in_phase < 0.0) &&
	       (equalized->quadrature < 0.0) ==
		       (target->quadrature < 0.0);
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
	      qam_equalizer_is_valid(&equalizer) &&
	      !qam_equalizer_is_handoff_ready(&equalizer),
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
		      !qam_equalizer_is_handoff_ready(&equalizer),
		      "equalizer delay changed");
		CHECK(qam_equalizer_adapt(
			      &equalizer, &output, 0.0) ==
			      QAM_EQUALIZER_OK &&
		      qam_equalizer_is_handoff_ready(&equalizer) &&
		      qam_equalizer_adapt(
			      &equalizer, &output, 0.0) ==
			      QAM_EQUALIZER_INVALID_STATE,
		      "equalizer handoff or single-adaptation contract changed");
	}
	return true;
}

static bool test_instance_selected_delay(void)
{
	const struct qam_point reference = { 1.0, -1.0 };
	const unsigned int centered_delay =
		QAM_EQUALIZER_MAX_TAP_COUNT / 2u;
	struct qam_equalizer centered;
	struct qam_point output = { 0 };
	bool ready = true;

	CHECK(qam_equalizer_init_geometry(
		      &centered, &reference, &reference,
		      QAM_EQUALIZER_MAX_TAP_COUNT, centered_delay) ==
		      QAM_EQUALIZER_OK &&
	      centered.tap_count == QAM_EQUALIZER_MAX_TAP_COUNT &&
	      centered.decision_delay == centered_delay,
	      "centered equalizer geometry did not initialize");
	for (unsigned int index = 0; index <= centered_delay; ++index) {
		const struct qam_point input =
			index == 0 ? reference : (struct qam_point){ 0 };

		CHECK(qam_equalizer_push(
			      &centered, &input, &output, &ready) ==
			      QAM_EQUALIZER_OK &&
		      ready == (index == centered_delay),
		      "centered equalizer changed readiness at input %u", index);
	}
	CHECK(points_are_close(&output, &reference),
	      "centered equalizer did not emit its delayed reference");
	{
		struct qam_equalizer default_delay;
		struct qam_equalizer_observation observation = { 0 };
		struct qam_equalizer saved_default;
		struct qam_equalizer_observation saved_observation;

		CHECK(qam_equalizer_observe(
			      &centered, &reference, &observation, &ready) ==
			      QAM_EQUALIZER_OK && ready &&
		      qam_equalizer_init_geometry(
			      &default_delay, &reference, &reference,
			      QAM_EQUALIZER_MAX_TAP_COUNT,
			      QAM_EQUALIZER_DELAY) == QAM_EQUALIZER_OK,
		      "decision-delay observation fixture did not initialize");
		saved_default = default_delay;
		saved_observation = observation;
		CHECK(qam_equalizer_adapt_observation(
			      &default_delay, &observation, &reference, 0.1) ==
			      QAM_EQUALIZER_INVALID_STATE &&
		      memcmp(&default_delay, &saved_default,
			     sizeof(default_delay)) == 0 &&
		      memcmp(&observation, &saved_observation,
			     sizeof(observation)) == 0,
		      "cross-delay observation rejection was not transactional");
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

static bool test_carrier_frequency_tracking(void)
{
	static const double unit_coordinate =
		0.70710678118654752440084436210485;
	const struct qam_point reference = {
		unit_coordinate,
		unit_coordinate,
	};
	struct qam_point transmitted[QAM_EQUALIZER_MAX_TAP_COUNT] = { 0 };
	struct qam_equalizer baseline;
	struct qam_equalizer tracked;
	uint32_t random = UINT32_C(0x504c4c21);
	unsigned int baseline_errors = 0;
	unsigned int tracked_errors = 0;

	CHECK(qam_equalizer_init(&baseline, &reference, &reference) ==
		      QAM_EQUALIZER_OK &&
	      qam_equalizer_init(&tracked, &reference, &reference) ==
		      QAM_EQUALIZER_OK &&
	      qam_equalizer_enable_carrier_tracking(
		      &tracked, 1.0 / 4.0, 1.0 / 8192.0) ==
		      QAM_EQUALIZER_OK,
	      "carrier-tracking equalizer fixture did not initialize");
	for (unsigned int symbol = 0;
	     symbol < CARRIER_TRACKING_SYMBOLS; ++symbol) {
		struct qam_equalizer_observation baseline_observation = { 0 };
		struct qam_equalizer_observation tracked_observation = { 0 };
		struct qam_point received;
		struct qam_point target;
		bool baseline_ready;
		bool tracked_ready;

		for (size_t index = ARRAY_LENGTH(transmitted) - 1u;
		     index != 0; --index)
			transmitted[index] = transmitted[index - 1u];
		transmitted[0] = (struct qam_point){
			.in_phase = (fixture_random(&random) & 1u) != 0u ?
				-unit_coordinate : unit_coordinate,
			.quadrature = (fixture_random(&random) & 1u) != 0u ?
				-unit_coordinate : unit_coordinate,
		};
		received = rotate(
			transmitted[0],
			CARRIER_DRIFT_RADIANS_PER_SYMBOL * symbol);
		CHECK(qam_equalizer_observe(
			      &baseline, &received, &baseline_observation,
			      &baseline_ready) == QAM_EQUALIZER_OK &&
		      qam_equalizer_observe(
			      &tracked, &received, &tracked_observation,
			      &tracked_ready) == QAM_EQUALIZER_OK &&
		      baseline_ready == tracked_ready,
		      "carrier tracker rejected symbol %u", symbol);
		if (!tracked_ready)
			continue;
		target = transmitted[QAM_EQUALIZER_DELAY];
		if (symbol >= CARRIER_TRACKING_SETTLE_SYMBOLS) {
			baseline_errors += !same_quadrant(
				&baseline_observation.output, &target);
			tracked_errors += !same_quadrant(
				&tracked_observation.output, &target);
		}
		CHECK(qam_equalizer_adapt_observation(
			      &baseline, &baseline_observation, &target, 0.0) ==
			      QAM_EQUALIZER_OK &&
		      qam_equalizer_adapt_observation(
			      &tracked, &tracked_observation, &target, 0.0) ==
			      QAM_EQUALIZER_OK,
		      "carrier tracker did not consume symbol %u", symbol);
	}
	CHECK(baseline_errors > CARRIER_TRACKING_SYMBOLS / 2u &&
	      tracked_errors == 0 && tracked.carrier_tracking &&
	      fabs(tracked.carrier_frequency +
		   CARRIER_DRIFT_RADIANS_PER_SYMBOL) < 0.001,
	      "carrier drift produced %u tracked errors versus %u without "
	      "tracking at frequency %.6g",
	      tracked_errors, baseline_errors, tracked.carrier_frequency);
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

static bool test_span_extension(void)
{
	const struct qam_point reference = { 1.0, -1.0 };
	const struct qam_point next_input = { -3.0, 1.0 };
	struct qam_equalizer baseline;
	struct qam_equalizer extended;
	struct qam_equalizer saved;
	struct qam_point baseline_output = { 0 };
	struct qam_point extended_output = { 0 };
	bool baseline_ready = false;
	bool extended_ready = false;

	CHECK(qam_equalizer_init(
		      &baseline, &reference, &reference) == QAM_EQUALIZER_OK,
	      "extension fixture did not initialize");
	for (unsigned int index = 0; index <= QAM_EQUALIZER_DELAY; ++index) {
		struct qam_point output = { 0 };
		bool ready;

		CHECK(qam_equalizer_push(
			      &baseline, &reference, &output, &ready) ==
			      QAM_EQUALIZER_OK,
		      "extension fixture did not fill at %u", index);
		if (ready)
			CHECK(qam_equalizer_adapt(
				      &baseline, &reference, 0.0) ==
				      QAM_EQUALIZER_OK,
			      "extension fixture did not become idle");
	}
	extended = baseline;
	CHECK(qam_equalizer_extend_span(
		      &extended, QAM_EQUALIZER_MAX_TAP_COUNT) ==
		      QAM_EQUALIZER_OK &&
	      extended.tap_count == QAM_EQUALIZER_MAX_TAP_COUNT &&
	      extended.decision_delay == baseline.decision_delay &&
	      extended.fill == baseline.fill && !extended.can_adapt,
	      "equalizer span did not extend in place");
	for (size_t index = 0; index < baseline.tap_count; ++index) {
		CHECK(points_are_close(
			      &extended.coefficients[index],
			      &baseline.coefficients[index]) &&
		      points_are_close(
			      &extended.history[index],
			      &baseline.history[index]),
		      "extension changed active tap %zu", index);
	}
	for (size_t index = baseline.tap_count;
	     index < extended.tap_count; ++index) {
		CHECK(points_are_close(
			      &extended.coefficients[index],
			      &(struct qam_point){ 0 }) &&
		      points_are_close(
			      &extended.history[index],
			      &(struct qam_point){ 0 }),
		      "extension exposed nonzero tap %zu", index);
	}
	CHECK(qam_equalizer_push(
		      &baseline, &next_input, &baseline_output,
		      &baseline_ready) == QAM_EQUALIZER_OK &&
	      qam_equalizer_push(
		      &extended, &next_input, &extended_output,
		      &extended_ready) == QAM_EQUALIZER_OK &&
	      baseline_ready && extended_ready &&
	      points_are_close(&baseline_output, &extended_output),
	      "zero-extended span changed the next output");
	saved = extended;
	CHECK(qam_equalizer_extend_span(
		      &extended, extended.tap_count - 1u) ==
		      QAM_EQUALIZER_INVALID_ARGUMENT &&
	      qam_equalizer_extend_span(
		      &extended, QAM_EQUALIZER_MAX_TAP_COUNT + 1u) ==
		      QAM_EQUALIZER_INVALID_ARGUMENT &&
	      qam_equalizer_extend_span(&extended, extended.tap_count) ==
		      QAM_EQUALIZER_INVALID_STATE &&
	      qam_equalizer_extend_span(NULL, QAM_EQUALIZER_MAX_TAP_COUNT) ==
		      QAM_EQUALIZER_INVALID_ARGUMENT &&
	      memcmp(&extended, &saved, sizeof(extended)) == 0,
	      "invalid span extension was not transactional");
	return true;
}

static bool test_validation(void)
{
	const struct qam_point reference = { 1.0, -1.0 };
	const struct qam_point zero = { 0 };
	const struct qam_point invalid = { NAN, 0.0 };
	struct qam_equalizer equalizer = { 0 };
	struct qam_equalizer carrier;
	struct qam_equalizer pending;
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
	      qam_equalizer_init_geometry(
		      &equalizer, &reference, &reference, 0, 0) ==
		      QAM_EQUALIZER_INVALID_ARGUMENT &&
	      qam_equalizer_init_geometry(
		      &equalizer, &reference, &reference,
		      QAM_EQUALIZER_MAX_TAP_COUNT,
		      QAM_EQUALIZER_MAX_TAP_COUNT) ==
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
	CHECK(qam_equalizer_enable_carrier_tracking(NULL, 0.25, 0.001) ==
		      QAM_EQUALIZER_INVALID_ARGUMENT &&
	      qam_equalizer_enable_carrier_tracking(
		      &equalizer, NAN, 0.001) ==
		      QAM_EQUALIZER_INVALID_ARGUMENT &&
	      qam_equalizer_enable_carrier_tracking(
		      &equalizer, 0.25, 0.0) ==
		      QAM_EQUALIZER_INVALID_ARGUMENT &&
	      qam_equalizer_enable_carrier_tracking(
		      &equalizer, 0.25, 0.5) ==
		      QAM_EQUALIZER_INVALID_ARGUMENT &&
	      memcmp(&equalizer, &saved, sizeof(equalizer)) == 0,
	      "invalid carrier-loop gains changed the equalizer");
	carrier = equalizer;
	CHECK(qam_equalizer_enable_carrier_tracking(
		      &carrier, 0.25, 0.001) == QAM_EQUALIZER_OK &&
	      qam_equalizer_is_valid(&carrier),
	      "valid carrier-loop gains were rejected");
	saved = carrier;
	CHECK(qam_equalizer_enable_carrier_tracking(
		      &carrier, 0.25, 0.001) ==
		      QAM_EQUALIZER_INVALID_STATE &&
	      memcmp(&carrier, &saved, sizeof(carrier)) == 0,
	      "re-enabling carrier tracking was not transactional");
	pending = equalizer;
	for (unsigned int index = 0; index <= QAM_EQUALIZER_DELAY; ++index) {
		CHECK(qam_equalizer_push(
			      &pending, &reference, &output, &ready) ==
			      QAM_EQUALIZER_OK,
		      "pending carrier-loop fixture rejected input %u", index);
	}
	saved = pending;
	CHECK(ready && qam_equalizer_enable_carrier_tracking(
			       &pending, 0.25, 0.001) ==
			       QAM_EQUALIZER_INVALID_STATE &&
	      memcmp(&pending, &saved, sizeof(pending)) == 0,
	      "carrier tracking accepted an unconsumed direct decision");
	equalizer.coefficients[0].in_phase = INFINITY;
	CHECK(!qam_equalizer_is_valid(&equalizer) &&
	      !qam_equalizer_is_handoff_ready(&equalizer) &&
	      !qam_equalizer_is_valid(NULL) &&
	      !qam_equalizer_is_handoff_ready(NULL) &&
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
		{ "instance-selected decision delay", test_instance_selected_delay },
		{ "generated ISI channel", test_generated_isi_channel },
		{ "carrier-frequency tracking", test_carrier_frequency_tracking },
		{ "instance-selected channel span", test_instance_selected_span },
		{ "delayed observations", test_delayed_observations },
		{ "span extension", test_span_extension },
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
