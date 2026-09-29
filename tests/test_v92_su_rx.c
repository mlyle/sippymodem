// SPDX-License-Identifier: BSD-3-Clause

#include <v92_su.h>
#include <v92_su_rx.h>

#include "channel_bench.h"
#include "test.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_LU = 4096,
	TEST_THRESHOLD = 256,
	TEST_PHASE_THRESHOLD = TEST_LU / 256,
	SAMPLE_LIMIT = 1024,
};

static struct v92_jp jp(uint16_t extension)
{
	return (struct v92_jp){
		.su_phase_extension_q16 = extension,
		.training_constellation = V92_CONSTELLATION_8_POINT,
		.rate_renegotiation_constellation = V92_CONSTELLATION_4_POINT,
	};
}

static bool extension_is_near(uint16_t value, uint16_t expected, uint16_t span)
{
	uint16_t forward = (uint16_t)(value - expected);
	uint16_t reverse = (uint16_t)(expected - value);

	return forward <= span || reverse <= span;
}

static bool check_phase_estimator(void)
{
	static const struct {
		int32_t first, second;
		uint16_t expected;
	} boundaries[] = {
		{ INT32_MIN, INT32_MIN, UINT16_C(0xc000) },
		{ INT32_MIN, INT32_MAX, UINT16_C(0x4000) },
		{ INT32_MAX, INT32_MIN, UINT16_C(0x4000) },
		{ INT32_MAX, INT32_MAX, UINT16_C(0xc000) },
		{ 0, INT32_MIN, 0 },
		{ 0, INT32_MAX, 0 },
		{ 1, INT32_MIN, 0 },
		{ 1, INT32_MAX, 0 },
		{ -1, INT32_MIN, 0 },
		{ -1, INT32_MAX, 0 },
	};
	uint16_t extension = UINT16_C(0x1357);
	uint16_t inverted = 0u;

	CHECK(v92_su_phase_extension(20000, 0, &extension) &&
	      extension == UINT16_C(0x8000),
	      "zero-phase Su did not request one half symbol");
	CHECK(v92_su_phase_extension(0, -20000, &extension) &&
	      extension == UINT16_C(0x0000),
	      "quadrature Su did not cancel the half-symbol adjustment");
	CHECK(v92_su_phase_extension(12000, -12000, &extension) &&
	      extension_is_near(extension, UINT16_C(0x4000), 1u) &&
	      v92_su_phase_extension(-12000, 12000, &inverted) &&
	      inverted == extension,
	      "Su phase estimate changed under line inversion: %04x/%04x",
	      extension, inverted);
	CHECK(v92_su_phase_extension(INT32_MIN, 0, &extension) &&
	      extension == UINT16_C(0x8000),
	      "Su phase estimator did not safely canonicalize INT32_MIN");
	for (size_t i = 0; i < ARRAY_LENGTH(boundaries); ++i)
		CHECK(v92_su_phase_extension(boundaries[i].first,
			      boundaries[i].second, &extension) &&
		      extension == boundaries[i].expected,
		      "Su phase boundary %zu changed to %04x", i, extension);
	extension = UINT16_C(0x1357);
	CHECK(!v92_su_phase_extension(0, 0, &extension) &&
	      !v92_su_phase_extension(1, 1, NULL) &&
	      extension == UINT16_C(0x1357),
	      "empty Su phase estimate changed its output");
	return true;
}

static bool test_phase_accumulator_bounds(void)
{
	const uint16_t counts[] = { 0, 1, V92_SU_RX_PHASE_SYMBOLS - 1,
		V92_SU_RX_PHASE_SYMBOLS };

	for (unsigned segment = 0; segment < 2u; ++segment) {
		for (size_t c = 0; c < ARRAY_LENGTH(counts); ++c) {
			struct v92_su_rx base;
			int32_t limit = (int32_t)counts[c] * 32768;
			const int32_t invalid[] = {
				limit + 1, -limit - 1, INT32_MIN, INT32_MAX,
			};

			CHECK(v92_su_rx_init(&base, V92_SU_RX_MEASURE_PHASE,
				      1, 1) == V92_SU_RX_OK, "bound fixture init");
			base.total_symbols = 1000;
			base.phase_samples[segment] = counts[c];
			if (segment == 0u) {
				base.stage = counts[c] == 0u ? V92_SU_RX_ACQUIRING :
					counts[c] == V92_SU_RX_PHASE_SYMBOLS ?
					V92_SU_RX_WAITING_TRANSITION : V92_SU_RX_FIRST_PHASE;
			} else {
				base.phase_samples[0] = V92_SU_RX_PHASE_SYMBOLS;
				base.transition_symbol = 1;
				base.stage = counts[c] == V92_SU_RX_PHASE_SYMBOLS ?
					V92_SU_RX_COMPLETE : V92_SU_RX_SECOND_PHASE;
			}
			for (unsigned sign = 0; sign < 2u; ++sign) {
				struct v92_su_rx receiver = base;
				int16_t sample = sign ? INT16_MAX : INT16_MIN;
				enum v92_su_rx_result result;

				receiver.phase_component[segment] = sign ? -limit : limit;
				receiver.history_valid = receiver.comparison_valid = 0x3f;
				receiver.comparison_same = 0x3f;
				receiver.history_signs = sign ? 0x3f : 0;
				receiver.stable_symbols = V92_SU_RX_MIN_STABLE_SYMBOLS;
				CHECK(v92_su_rx_is_valid(&receiver), "valid bound rejected");
				result = v92_su_rx_sample(&receiver, sample);
				CHECK(result <= V92_SU_RX_FAILURE &&
				      v92_su_rx_is_valid(&receiver),
				      "boundary accumulation failed: %u/%u/%u result=%d",
				      segment, counts[c], sign, result);
			}
			for (size_t i = 0; i < ARRAY_LENGTH(invalid); ++i) {
				struct v92_su_rx receiver = base;
				struct v92_su_rx before;

				receiver.phase_component[segment] = invalid[i];
				before = receiver;
				CHECK(!v92_su_rx_is_valid(&receiver) &&
				      v92_su_rx_sample(&receiver, INT16_MIN) ==
					      V92_SU_RX_INVALID_ARGUMENT &&
				      memcmp(&receiver, &before, sizeof(receiver)) == 0,
				      "invalid accumulator accepted: %u/%u value=%d",
				      segment, counts[c], invalid[i]);
			}
		}
	}
	return true;
}

static bool run_phase_receiver(
	const struct channel_bench_effect_spec *effects, size_t effect_count,
	bool phase_expected, uint16_t *extension)
{
	struct channel_bench channel = { 0 };
	struct fractional_delay delay;
	struct v92_su source;
	struct v92_su_rx receiver;
	unsigned acquired = 0u;
	unsigned transitions = 0u;
	unsigned ready = 0u;

	CHECK(channel_bench_init(&channel, effects, effect_count) ==
		      CHANNEL_BENCH_OK,
	      "Su channel did not initialize");
	fractional_delay_init(&delay);
	CHECK(v92_su_init(&source, TEST_LU) == V92_SU_OK &&
	      v92_su_rx_init(
		      &receiver, V92_SU_RX_MEASURE_PHASE,
		      TEST_THRESHOLD, TEST_PHASE_THRESHOLD) ==
		      V92_SU_RX_OK,
	      "Su phase fixture did not initialize");
	for (size_t index = 0; index < SAMPLE_LIMIT && ready == 0u; ++index) {
		int16_t sample;
		enum v92_su_result generated =
			v92_su_next(&source, &delay, &sample);
		enum v92_su_rx_result result;

		CHECK(generated == V92_SU_OK ||
		      generated == V92_SU_REACHED_JP_WAIT,
		      "Su source stopped at %zu with %s", index,
		      v92_su_result_name(generated));
		CHECK(channel_bench_process(&channel, &sample, 1u) ==
			      CHANNEL_BENCH_OK,
		      "Su channel failed at %zu", index);
		result = v92_su_rx_sample(&receiver, sample);
		if (result == V92_SU_RX_FAILURE && !phase_expected) {
			struct v92_su_rx before = receiver;
			uint16_t untouched = UINT16_C(0x1357);

			CHECK(acquired == 1u && transitions == 1u &&
			      receiver.phase_samples[0] == V92_SU_RX_PHASE_SYMBOLS &&
			      receiver.phase_samples[1] == V92_SU_RX_PHASE_SYMBOLS &&
			      !v92_su_rx_phase_extension(&receiver, &untouched) &&
			      untouched == UINT16_C(0x1357) &&
			      v92_su_rx_sample(&receiver, 1234) == V92_SU_RX_FAILURE &&
			      memcmp(&receiver, &before, sizeof(receiver)) == 0,
			      "absent Su phase was not rejected transactionally");
			channel_bench_destroy(&channel);
			return true;
		}
		CHECK(result == V92_SU_RX_OK || result == V92_SU_RX_ACQUIRED ||
		      result == V92_SU_RX_TRANSITION ||
		      result == V92_SU_RX_PHASE_READY,
		      "Su phase receiver returned %s at %zu",
		      v92_su_rx_result_name(result), index);
		acquired += result == V92_SU_RX_ACQUIRED;
		transitions += result == V92_SU_RX_TRANSITION;
		ready += result == V92_SU_RX_PHASE_READY;
	}
	CHECK(phase_expected && acquired == 1u && transitions == 1u && ready == 1u &&
	      v92_su_rx_is_complete(&receiver) &&
	      !v92_su_rx_has_failed(&receiver) &&
	      v92_su_rx_phase_extension(&receiver, extension) &&
	      source.stage == V92_SU_JP_WAIT,
	      "Su phase receiver did not complete once: %u/%u/%u stage=%s",
	      acquired, transitions, ready,
	      v92_su_rx_stage_name(receiver.stage));
	channel_bench_destroy(&channel);
	return true;
}

static bool test_generated_phase_probe(void)
{
	static const struct channel_bench_effect_spec exact[] = {
		{ .kind = CHANNEL_BENCH_PCMU },
		{ .kind = CHANNEL_BENCH_PCMA },
	};
	static const struct channel_bench_effect_spec impaired[] = {
		{
			.kind = CHANNEL_BENCH_DELAY,
			.parameters.delay.samples = 7u,
		},
		{
			.kind = CHANNEL_BENCH_GAIN,
			.parameters.gain.scale_q15 = -24576,
		},
		{
			.kind = CHANNEL_BENCH_NOISE,
			.parameters.noise = {
				.peak = 32u,
				.seed = UINT32_C(0x72a4c931),
			},
		},
		{
			.kind = CHANNEL_BENCH_PCMA,
		},
	};
	/* H(z) = (1 + 0.96 z^-1) / 2 retains the low-frequency Su
	 * waveform but attenuates its Nyquist phase component by 34 dB. */
	static const struct channel_bench_effect_spec band_limited[] = {
		{
			.kind = CHANNEL_BENCH_ECHO,
			.parameters.echo = {
				.delay_samples = 1u,
				.gain_q15 = 31457,
			},
		},
		{
			.kind = CHANNEL_BENCH_GAIN,
			.parameters.gain.scale_q15 = 16384,
		},
	};
	uint16_t exact_extension = 0u;
	uint16_t impaired_extension = 0u;
	uint16_t filtered_extension = 0u;

	for (size_t law = 0; law < ARRAY_LENGTH(exact); ++law) {
		CHECK(run_phase_receiver(exact + law, 1u, true, &exact_extension),
		      "exact generated Su phase probe failed");
		CHECK(extension_is_near(
			      exact_extension, UINT16_C(0x8000), UINT16_C(0x0010)),
		      "exact Su phase was %04x instead of one half symbol for law %zu",
		      exact_extension, law);
	}
	CHECK(run_phase_receiver(
		      impaired, ARRAY_LENGTH(impaired), true, &impaired_extension),
	      "impaired generated Su phase probe failed");
	CHECK(extension_is_near(
		      impaired_extension, UINT16_C(0x8000), UINT16_C(0x1000)),
	      "impaired Su phase drifted to %04x", impaired_extension);
	CHECK(run_phase_receiver(
		      band_limited, ARRAY_LENGTH(band_limited),
		      true, &filtered_extension),
	      "band-limited Su phase probe failed");
	CHECK(extension_is_near(
		      filtered_extension, UINT16_C(0x8000), UINT16_C(0x1000)),
	      "band-limited Su phase drifted to %04x", filtered_extension);
	return true;
}

static bool test_absent_phase_is_rejected(void)
{
	/* H(z) = (1 + z^-1) / 2 removes only the phase information;
	 * there is still a strong periodic waveform and a detectable inversion. */
	const struct channel_bench_effect_spec notch[] = {
		{
			.kind = CHANNEL_BENCH_ECHO,
			.parameters.echo = {
				.delay_samples = 1u,
				.gain_q15 = 32768,
			},
		},
		{
			.kind = CHANNEL_BENCH_GAIN,
			.parameters.gain.scale_q15 = 16384,
		},
	};
	struct v92_su_rx receiver;
	struct channel_bench_generator noise;
	uint16_t extension = 0;

	CHECK(run_phase_receiver(notch, ARRAY_LENGTH(notch), false, &extension),
	      "Su without phase information was accepted");
	CHECK(v92_su_rx_init(&receiver, V92_SU_RX_MEASURE_PHASE,
			    TEST_THRESHOLD, TEST_PHASE_THRESHOLD) == V92_SU_RX_OK,
	      "noise receiver did not initialize");
	channel_bench_generator_init(&noise, UINT32_C(0x23688ba1));
	for (size_t index = 0; index < SAMPLE_LIMIT; ++index) {
		int16_t sample;

		CHECK(channel_bench_generate_uniform(&noise, &sample, 1u, 32u) ==
			      CHANNEL_BENCH_OK &&
		      v92_su_rx_sample(&receiver, sample) == V92_SU_RX_OK &&
		      receiver.stage == V92_SU_RX_ACQUIRING,
		      "sub-threshold noise acquired Su at %zu", index);
	}
	return true;
}

static bool advance_to_jp_wait(
	struct v92_su *source, struct fractional_delay *delay)
{
	for (size_t index = 0; index < SAMPLE_LIMIT; ++index) {
		int16_t sample;
		enum v92_su_result result = v92_su_next(source, delay, &sample);

		if (result == V92_SU_REACHED_JP_WAIT)
			return true;
		if (result != V92_SU_OK)
			return false;
	}
	return false;
}

static bool test_transition_receiver_is_reusable(void)
{
	struct fractional_delay delay;
	struct v92_su source;
	struct v92_su_rx receiver;
	struct v92_jp parameters = jp(UINT16_C(0x8000));
	unsigned acquired = 0u;
	unsigned transitions = 0u;

	fractional_delay_init(&delay);
	CHECK(v92_su_init(&source, TEST_LU) == V92_SU_OK &&
	      advance_to_jp_wait(&source, &delay) &&
	      v92_su_rx_init(
		      &receiver, V92_SU_RX_TRANSITION_ONLY,
		      TEST_THRESHOLD, TEST_PHASE_THRESHOLD) == V92_SU_RX_OK,
	      "Su transition fixture did not initialize");
	for (size_t index = 0; index < 48u; ++index) {
		int16_t sample;
		enum v92_su_rx_result result;

		CHECK(v92_su_next(&source, &delay, &sample) == V92_SU_OK,
		      "Su wait source failed at %zu", index);
		result = v92_su_rx_sample(&receiver, sample);
		CHECK(result == V92_SU_RX_OK || result == V92_SU_RX_ACQUIRED,
		      "Su transition receiver reported early %s",
		      v92_su_rx_result_name(result));
		acquired += result == V92_SU_RX_ACQUIRED;
	}
	CHECK(acquired == 1u &&
	      v92_su_accept_jp(&source, &parameters) == V92_SU_OK,
	      "Su transition receiver did not arm once");
	for (size_t index = 0; index < 48u && transitions == 0u; ++index) {
		int16_t sample;
		enum v92_su_result generated = v92_su_next(&source, &delay, &sample);
		if (generated == V92_SU_COMPLETE) {
			CHECK(fractional_delay_process(&delay, 0, &sample) == FRACTIONAL_DELAY_OK,
			      "Su filter tail failed");
			generated = V92_SU_OK;
		}
		enum v92_su_rx_result result;

		CHECK(generated == V92_SU_OK ||
		      generated == V92_SU_REACHED_TRN1U_READY,
		      "Su Jp response source failed at %zu", index);
		result = v92_su_rx_sample(&receiver, sample);
		CHECK(result == V92_SU_RX_OK ||
		      result == V92_SU_RX_TRANSITION,
		      "Su transition receiver returned %s",
		      v92_su_rx_result_name(result));
		transitions += result == V92_SU_RX_TRANSITION;
	}
	CHECK(transitions == 1u && v92_su_rx_is_complete(&receiver),
	      "Su Jp response transition was not detected once");
	{
		struct v92_su_rx before = receiver;

		CHECK(v92_su_rx_sample(&receiver, 1234) == V92_SU_RX_DONE &&
		      memcmp(&receiver, &before, sizeof(receiver)) == 0,
		      "complete Su receiver consumed another symbol");
	}
	return true;
}

static bool test_short_response_at_each_pattern_phase(void)
{
	/* The peer starts TRN1u immediately after its (24 + epsilon)T
	 * response. Detect the inversion before those first training symbols
	 * leave the reconstruction delay, including a half-symbol input phase. */
	for (unsigned wait = 96; wait < 108; ++wait) {
		struct fractional_delay delay;
		struct v92_su source;
		struct v92_su_rx receiver;
		struct v92_jp parameters = jp(UINT16_C(0x8000));
		bool detected = false;

		fractional_delay_init(&delay);
		CHECK(v92_su_init(&source, 6908) == V92_SU_OK &&
		      advance_to_jp_wait(&source, &delay) &&
		      v92_su_rx_init(&receiver, V92_SU_RX_TRANSITION_ONLY,
			      TEST_THRESHOLD, TEST_PHASE_THRESHOLD) == V92_SU_RX_OK,
		      "short Su response did not initialize");
		for (unsigned i = 0; i < wait; ++i) {
			int16_t sample;
			CHECK(v92_su_next(&source, &delay, &sample) == V92_SU_OK,
			      "Su wait failed");
			enum v92_su_rx_result result = v92_su_rx_sample(&receiver, sample);
			CHECK(result == V92_SU_RX_OK || result == V92_SU_RX_ACQUIRED,
			      "Su reversed before Jp");
		}
		CHECK(v92_su_accept_jp(&source, &parameters) == V92_SU_OK,
		      "short Su response rejected Jp");
		for (unsigned i = 0; i < V92_SU_JP_BAR_INPUT_SYMBOLS +
			     FRACTIONAL_DELAY_CENTER && !detected; ++i) {
			int16_t sample;
			enum v92_su_result generated = v92_su_next(&source, &delay, &sample);
			if (generated == V92_SU_COMPLETE) {
				CHECK(fractional_delay_process(&delay, 0, &sample) == FRACTIONAL_DELAY_OK,
				      "Su filter tail failed");
				generated = V92_SU_OK;
			}
			CHECK(generated == V92_SU_OK || generated == V92_SU_REACHED_TRN1U_READY,
			      "short Su generation failed");
			enum v92_su_rx_result result = v92_su_rx_sample(&receiver, sample);
			CHECK(result == V92_SU_RX_OK || result == V92_SU_RX_TRANSITION,
			      "short Su response failed");
			detected = result == V92_SU_RX_TRANSITION;
		}
		CHECK(detected, "missed Su reversal after %u wait symbols", wait);
	}
	return true;
}

static bool test_validation_is_transactional(void)
{
	struct v92_su_rx receiver;
	struct v92_su_rx before;
	uint16_t extension = UINT16_C(0x2468);

	memset(&receiver, 0xa5, sizeof(receiver));
	before = receiver;
	CHECK(v92_su_rx_init(NULL, V92_SU_RX_TRANSITION_ONLY, 1u, 1u) ==
		      V92_SU_RX_INVALID_ARGUMENT &&
	      v92_su_rx_init(
		      &receiver, (enum v92_su_rx_mode)99, 1u, 1u) ==
		      V92_SU_RX_INVALID_ARGUMENT &&
	      v92_su_rx_init(&receiver, V92_SU_RX_MEASURE_PHASE, 0u, 1u) ==
		      V92_SU_RX_INVALID_ARGUMENT &&
	      v92_su_rx_init(&receiver, V92_SU_RX_MEASURE_PHASE, 1u, 0u) ==
		      V92_SU_RX_INVALID_ARGUMENT &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0,
	      "invalid Su initialization changed the receiver");
	CHECK(v92_su_rx_init(
		      &receiver, V92_SU_RX_TRANSITION_ONLY,
		      TEST_THRESHOLD, TEST_PHASE_THRESHOLD) == V92_SU_RX_OK &&
	      v92_su_rx_is_valid(&receiver) &&
	      !v92_su_rx_is_complete(&receiver) &&
	      !v92_su_rx_has_failed(&receiver) &&
	      !v92_su_rx_phase_extension(&receiver, &extension) &&
	      extension == UINT16_C(0x2468),
	      "valid Su initialization or getter failed");
	before = receiver;
	receiver.total_symbols = UINT64_MAX;
	CHECK(v92_su_rx_sample(&receiver, 1) == V92_SU_RX_INVALID_STATE,
	      "Su receiver accepted an overflowing clock");
	receiver = before;
	receiver.comparison_same = 1u;
	receiver.comparison_opposite = 1u;
	before = receiver;
	CHECK(!v92_su_rx_is_valid(&receiver) &&
	      v92_su_rx_sample(&receiver, 1) == V92_SU_RX_INVALID_ARGUMENT &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0,
	      "invalid Su receiver consumed a symbol");
	CHECK(strcmp(v92_su_rx_stage_name(V92_SU_RX_SECOND_PHASE),
		     "measuring second phase") == 0 &&
	      strcmp(v92_su_rx_stage_name((enum v92_su_rx_stage)99),
		     "unknown") == 0 &&
	      strcmp(v92_su_rx_result_name(V92_SU_RX_TRANSITION),
		     "inversion detected") == 0 &&
	      strcmp(v92_su_rx_result_name((enum v92_su_rx_result)99),
		     "unknown") == 0,
	      "Su receiver diagnostics were incomplete");
	return true;
}

static bool test_acquisition_magnitude(void)
{
	const int16_t samples[] = {
		INT16_MIN, -32767, -257, -256, -1, 0, 1, 256, 257, INT16_MAX,
	};
	const uint16_t thresholds[] = { 256, 32767, 32768 };

	for (size_t s = 0; s < ARRAY_LENGTH(samples); ++s) {
		uint16_t magnitude = (uint16_t)(samples[s] < 0 ?
			-(int32_t)samples[s] : samples[s]);
		for (size_t t = 0; t < ARRAY_LENGTH(thresholds); ++t) {
			struct v92_su_rx receiver;
			unsigned acquired = 0;
			CHECK(v92_su_rx_init(&receiver, V92_SU_RX_TRANSITION_ONLY,
			      thresholds[t], 1) == V92_SU_RX_OK, "magnitude fixture init failed");
			for (unsigned i = 0; i < 32; ++i) {
				enum v92_su_rx_result result = v92_su_rx_sample(&receiver, samples[s]);
				CHECK(result == V92_SU_RX_OK || result == V92_SU_RX_ACQUIRED,
				      "constant periodic input returned %d", result);
				acquired += result == V92_SU_RX_ACQUIRED;
			}
			bool qualifies = magnitude > thresholds[t];
			CHECK(acquired == (unsigned)qualifies &&
			      receiver.peak == (qualifies ? magnitude : 0) &&
			      receiver.stage == (qualifies ? V92_SU_RX_WAITING_TRANSITION :
				V92_SU_RX_ACQUIRING),
			      "sample %d at threshold %u changed acquisition", samples[s], thresholds[t]);
		}
	}
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "phase estimator", check_phase_estimator },
		{ "phase accumulator bounds", test_phase_accumulator_bounds },
		{ "acquisition magnitude", test_acquisition_magnitude },
		{ "generated phase probe", test_generated_phase_probe },
		{ "absent phase rejection", test_absent_phase_is_rejected },
		{ "reusable transition receiver",
		  test_transition_receiver_is_reusable },
		{ "short response at each pattern phase", test_short_response_at_each_pattern_phase },
		{ "transactional validation", test_validation_is_transactional },
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
