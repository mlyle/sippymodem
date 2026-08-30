// SPDX-License-Identifier: BSD-3-Clause

#include "channel_bench.h"
#include "test.h"

#include <fractional_delay.h>
#include <g711.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
	GENERATED_SAMPLES = 64,
	EXTERNAL_COMPARE_SAMPLES = 16384,
	EXTERNAL_MAXIMUM_LAG = 160,
	EXTERNAL_MAXIMUM_SAMPLES = 8 * 60 * 8000,
};

static bool test_deterministic_generators(void)
{
	static const uint32_t expected[] = {
		UINT32_C(0x00042021),
		UINT32_C(0x04080601),
		UINT32_C(0x9dcca8c5),
		UINT32_C(0x1255994f),
		UINT32_C(0x8ef917d1),
		UINT32_C(0x2c6f5bd0),
	};
	struct channel_bench_generator known;
	struct channel_bench_generator whole;
	struct channel_bench_generator chunked;
	int16_t whole_samples[GENERATED_SAMPLES];
	int16_t chunked_samples[GENERATED_SAMPLES];
	int16_t bipolar[GENERATED_SAMPLES];

	channel_bench_generator_init(&known, 1);
	for (size_t index = 0; index < ARRAY_LENGTH(expected); ++index) {
		CHECK(channel_bench_generator_next(&known) == expected[index],
		      "xorshift sequence changed at sample %zu", index);
	}
	channel_bench_generator_init(&whole, 0);
	channel_bench_generator_init(&chunked, 0);
	CHECK(channel_bench_generate_uniform(
		      &whole, whole_samples, ARRAY_LENGTH(whole_samples), 3000) ==
		      CHANNEL_BENCH_OK,
	      "whole uniform excitation failed");
	CHECK(channel_bench_generate_uniform(
		      &chunked, chunked_samples, 17, 3000) == CHANNEL_BENCH_OK &&
		      channel_bench_generate_uniform(
			      &chunked, chunked_samples + 17,
			      ARRAY_LENGTH(chunked_samples) - 17, 3000) ==
			      CHANNEL_BENCH_OK,
	      "chunked uniform excitation failed");
	CHECK(memcmp(whole_samples, chunked_samples, sizeof(whole_samples)) == 0 &&
		      whole.state == chunked.state,
	      "uniform excitation depends on host chunking");
	channel_bench_generator_init(&whole, UINT32_C(0x12345678));
	CHECK(channel_bench_generate_bipolar(
		      &whole, bipolar, ARRAY_LENGTH(bipolar), 12000) ==
		      CHANNEL_BENCH_OK,
	      "bipolar excitation failed");
	for (size_t index = 0; index < ARRAY_LENGTH(bipolar); ++index) {
		CHECK(bipolar[index] == 12000 || bipolar[index] == -12000,
		      "bipolar excitation left its amplitude at sample %zu", index);
	}
	CHECK(channel_bench_generate_uniform(
		      &whole, NULL, 1, 1) == CHANNEL_BENCH_INVALID_ARGUMENT &&
		      channel_bench_generate_bipolar(
			      &whole, bipolar, ARRAY_LENGTH(bipolar), UINT16_MAX) ==
			      CHANNEL_BENCH_INVALID_ARGUMENT &&
		      channel_bench_generator_next(NULL) == 0,
	      "generator validation is incomplete");
	return true;
}

static bool test_effect_order_and_chunk_invariance(void)
{
	static const struct channel_bench_effect_spec effects[] = {
		{
			.kind = CHANNEL_BENCH_GAIN,
			.parameters.gain = { .scale_q15 = 16384 },
		},
		{
			.kind = CHANNEL_BENCH_ECHO,
			.parameters.echo = {
				.delay_samples = 2,
				.gain_q15 = 16384,
			},
		},
		{
			.kind = CHANNEL_BENCH_DELAY,
			.parameters.delay = { .samples = 3 },
		},
	};
	static const int16_t input[] = {
		1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000,
	};
	static const int16_t expected[] = {
		0, 0, 0, 500, 1000, 1750, 2500, 3250,
	};
	struct channel_bench whole = { 0 };
	struct channel_bench chunked = { 0 };
	int16_t whole_samples[ARRAY_LENGTH(input)];
	int16_t chunked_samples[ARRAY_LENGTH(input)];

	memcpy(whole_samples, input, sizeof(input));
	memcpy(chunked_samples, input, sizeof(input));
	CHECK(channel_bench_init(&whole, effects, ARRAY_LENGTH(effects)) ==
		      CHANNEL_BENCH_OK &&
		      channel_bench_init(&chunked, effects, ARRAY_LENGTH(effects)) ==
		      CHANNEL_BENCH_OK,
	      "channel chain initialization failed");
	CHECK(channel_bench_process(
		      &whole, whole_samples, ARRAY_LENGTH(whole_samples)) ==
		      CHANNEL_BENCH_OK,
	      "whole channel processing failed");
	CHECK(channel_bench_process(&chunked, chunked_samples, 3) ==
		      CHANNEL_BENCH_OK &&
		      channel_bench_process(
			      &chunked, chunked_samples + 3,
			      ARRAY_LENGTH(chunked_samples) - 3) ==
			      CHANNEL_BENCH_OK,
	      "chunked channel processing failed");
	CHECK(memcmp(whole_samples, expected, sizeof(expected)) == 0,
	      "channel effects did not run in specification order");
	CHECK(memcmp(whole_samples, chunked_samples, sizeof(whole_samples)) == 0 &&
		      whole.samples_processed == ARRAY_LENGTH(input) &&
		      chunked.samples_processed == ARRAY_LENGTH(input),
	      "channel state depends on host chunking");
	channel_bench_destroy(&whole);
	channel_bench_destroy(&chunked);
	CHECK(whole.effects == NULL && whole.effect_count == 0 &&
		      whole.samples_processed == 0,
	      "channel destruction did not clear ownership");
	return true;
}

static bool test_noise_and_codec_effects(void)
{
	static const struct channel_bench_effect_spec noise = {
		.kind = CHANNEL_BENCH_NOISE,
		.start_sample = 7,
		.end_sample = 51,
		.parameters.noise = {
			.peak = 2048,
			.seed = UINT32_C(0x13579bdf),
		},
	};
	static const int16_t codec_input[] = {
		0, 8, -8, 1000, -1000, INT16_MIN, INT16_MAX,
	};
	struct channel_bench first = { 0 };
	struct channel_bench second = { 0 };
	struct channel_bench pcmu = { 0 };
	struct channel_bench pcma = { 0 };
	const struct channel_bench_effect_spec pcmu_effect = {
		.kind = CHANNEL_BENCH_PCMU,
	};
	const struct channel_bench_effect_spec pcma_effect = {
		.kind = CHANNEL_BENCH_PCMA,
	};
	int16_t first_samples[GENERATED_SAMPLES] = { 0 };
	int16_t second_samples[GENERATED_SAMPLES] = { 0 };
	int16_t pcmu_samples[ARRAY_LENGTH(codec_input)];
	int16_t pcma_samples[ARRAY_LENGTH(codec_input)];
	bool exercised_noise = false;

	CHECK(channel_bench_init(&first, &noise, 1) == CHANNEL_BENCH_OK &&
		      channel_bench_init(&second, &noise, 1) == CHANNEL_BENCH_OK,
	      "independent noise channels failed to initialize");
	CHECK(channel_bench_process(&first, first_samples, 11) ==
		      CHANNEL_BENCH_OK &&
		      channel_bench_process(
			      &first, first_samples + 11,
			      ARRAY_LENGTH(first_samples) - 11) ==
			      CHANNEL_BENCH_OK &&
		      channel_bench_process(
			      &second, second_samples,
			      ARRAY_LENGTH(second_samples)) == CHANNEL_BENCH_OK,
	      "deterministic noise processing failed");
	for (size_t index = 0; index < ARRAY_LENGTH(first_samples); ++index) {
		if (index >= noise.start_sample && index < noise.end_sample)
			exercised_noise |= first_samples[index] != 0;
		else
			CHECK(first_samples[index] == 0,
			      "scheduled noise escaped at sample %zu", index);
	}
	CHECK(exercised_noise &&
		      memcmp(first_samples, second_samples, sizeof(first_samples)) == 0,
	      "noise state is shared or nondeterministic");
	memcpy(pcmu_samples, codec_input, sizeof(codec_input));
	memcpy(pcma_samples, codec_input, sizeof(codec_input));
	CHECK(channel_bench_init(&pcmu, &pcmu_effect, 1) == CHANNEL_BENCH_OK &&
		      channel_bench_init(&pcma, &pcma_effect, 1) == CHANNEL_BENCH_OK &&
		      channel_bench_process(
			      &pcmu, pcmu_samples, ARRAY_LENGTH(pcmu_samples)) ==
			      CHANNEL_BENCH_OK &&
		      channel_bench_process(
			      &pcma, pcma_samples, ARRAY_LENGTH(pcma_samples)) ==
			      CHANNEL_BENCH_OK,
	      "G.711 channel effects failed");
	for (size_t index = 0; index < ARRAY_LENGTH(codec_input); ++index) {
		CHECK(pcmu_samples[index] ==
			      g711_decode_pcmu(g711_encode_pcmu(codec_input[index])) &&
			      pcma_samples[index] ==
			      g711_decode_pcma(g711_encode_pcma(codec_input[index])),
		      "G.711 channel effect changed at sample %zu", index);
	}
	channel_bench_destroy(&pcma);
	channel_bench_destroy(&pcmu);
	channel_bench_destroy(&second);
	channel_bench_destroy(&first);
	return true;
}

static bool test_fractional_delay_effect(void)
{
	static const struct channel_bench_effect_spec effect = {
		.kind = CHANNEL_BENCH_FRACTIONAL_DELAY,
		.parameters.fractional_delay.phase_q16 =
			FRACTIONAL_DELAY_HALF_SAMPLE_Q16,
	};
	struct channel_bench_generator generator;
	struct channel_bench whole = { 0 };
	struct channel_bench chunked = { 0 };
	int16_t whole_samples[GENERATED_SAMPLES];
	int16_t chunked_samples[GENERATED_SAMPLES];
	int16_t original[GENERATED_SAMPLES];

	channel_bench_generator_init(&generator, UINT32_C(0x9e3779b9));
	CHECK(channel_bench_generate_uniform(
		      &generator, whole_samples, ARRAY_LENGTH(whole_samples),
		      12000) == CHANNEL_BENCH_OK,
	      "fractional-delay excitation failed");
	memcpy(chunked_samples, whole_samples, sizeof(whole_samples));
	memcpy(original, whole_samples, sizeof(original));
	CHECK(channel_bench_init(&whole, &effect, 1) == CHANNEL_BENCH_OK &&
		      channel_bench_init(&chunked, &effect, 1) == CHANNEL_BENCH_OK,
	      "fractional-delay channels did not initialize");
	CHECK(channel_bench_process(
		      &whole, whole_samples, ARRAY_LENGTH(whole_samples)) ==
		      CHANNEL_BENCH_OK &&
		      channel_bench_process(&chunked, chunked_samples, 17) ==
		      CHANNEL_BENCH_OK &&
		      channel_bench_process(
			      &chunked, chunked_samples + 17,
			      ARRAY_LENGTH(chunked_samples) - 17) ==
		      CHANNEL_BENCH_OK,
	      "fractional-delay channel processing failed");
	CHECK(memcmp(whole_samples, original, sizeof(original)) != 0 &&
		      memcmp(whole_samples, chunked_samples, sizeof(whole_samples)) ==
		      0,
	      "fractional-delay effect changed with ownership or chunking");
	channel_bench_destroy(&chunked);
	channel_bench_destroy(&whole);
	return true;
}

static bool test_validation_and_saturation(void)
{
	struct channel_bench bench = { 0 };
	const struct channel_bench_effect_spec invalid_delay = {
		.kind = CHANNEL_BENCH_DELAY,
		.parameters.delay = { .samples = 0 },
	};
	const struct channel_bench_effect_spec invalid_window = {
		.kind = CHANNEL_BENCH_MUTE,
		.start_sample = 10,
		.end_sample = 10,
	};
	const struct channel_bench_effect_spec invalid_fractional_delay = {
		.kind = CHANNEL_BENCH_FRACTIONAL_DELAY,
		.parameters.fractional_delay.phase_q16 =
			FRACTIONAL_DELAY_ONE_SAMPLE_Q16 + 1,
	};
	const struct channel_bench_effect_spec gain = {
		.kind = CHANNEL_BENCH_GAIN,
		.parameters.gain = { .scale_q15 = 65536 },
	};
	int16_t samples[] = { 20000, -20000 };

	CHECK(channel_bench_init(&bench, &invalid_delay, 1) ==
		      CHANNEL_BENCH_INVALID_ARGUMENT &&
		      bench.effects == NULL && bench.effect_count == 0,
	      "invalid effect retained partial channel state");
	CHECK(channel_bench_init(&bench, &invalid_window, 1) ==
		      CHANNEL_BENCH_INVALID_ARGUMENT,
	      "channel accepted an empty effect window");
	CHECK(channel_bench_init(&bench, &invalid_fractional_delay, 1) ==
		      CHANNEL_BENCH_INVALID_ARGUMENT,
	      "channel accepted an invalid fractional delay");
	CHECK(channel_bench_init(&bench, &gain, 1) == CHANNEL_BENCH_OK,
	      "gain channel failed to initialize");
	CHECK(channel_bench_init(&bench, &gain, 1) ==
		      CHANNEL_BENCH_INVALID_ARGUMENT,
	      "channel allowed live state to be overwritten");
	CHECK(channel_bench_process(&bench, NULL, 1) ==
		      CHANNEL_BENCH_INVALID_ARGUMENT &&
		      bench.samples_processed == 0,
	      "invalid processing changed channel state");
	CHECK(channel_bench_process(&bench, samples, ARRAY_LENGTH(samples)) ==
		      CHANNEL_BENCH_OK &&
		      samples[0] == INT16_MAX && samples[1] == INT16_MIN,
	      "fixed-point gain did not saturate at the PCM boundary");
	CHECK(strcmp(channel_bench_result_name(
			     CHANNEL_BENCH_RECORDING_MISSING),
		     "optional recording is missing") == 0 &&
		      strcmp(channel_bench_result_name(
			     (enum channel_bench_result)99),
		     "unknown channel bench result") == 0,
	      "channel result diagnostics are incomplete");
	channel_bench_destroy(&bench);
	return true;
}

static bool test_comparison_metrics_and_alignment(void)
{
	struct channel_bench_generator generator;
	struct channel_bench_comparison comparison;
	int16_t reference[GENERATED_SAMPLES];
	int16_t observed[GENERATED_SAMPLES + 5] = { 123, -456, 789 };
	const int16_t constant_reference[] = { 10, 10, 10, 10 };
	const int16_t constant_observed[] = { 12, 12, 12, 12 };

	channel_bench_generator_init(&generator, 7);
	CHECK(channel_bench_generate_bipolar(
		      &generator, reference, ARRAY_LENGTH(reference), 1000) ==
		      CHANNEL_BENCH_OK,
	      "comparison reference generation failed");
	for (size_t index = 0; index < ARRAY_LENGTH(reference); ++index)
		observed[index + 3] = (int16_t)(reference[index] / 2 + 7);
	observed[GENERATED_SAMPLES + 3] = -111;
	observed[GENERATED_SAMPLES + 4] = 222;
	CHECK(channel_bench_find_alignment(
		      reference, ARRAY_LENGTH(reference), observed,
		      ARRAY_LENGTH(observed), 5, GENERATED_SAMPLES - 5,
		      &comparison) == CHANNEL_BENCH_OK,
	      "recording alignment failed");
	CHECK(comparison.lag_samples == 3 && comparison.reference_start == 0 &&
		      comparison.observed_start == 3 &&
		      comparison.compared_samples == GENERATED_SAMPLES,
	      "recording alignment chose lag %td (%zu/%zu samples=%zu)",
	      comparison.lag_samples, comparison.reference_start,
	      comparison.observed_start, comparison.compared_samples);
	CHECK(comparison.correlation_defined &&
		      fabs(comparison.correlation - 1.0) < 1e-12 &&
		      fabs(comparison.fitted_gain - 0.5) < 1e-12 &&
		      fabs(comparison.fitted_bias - 7.0) < 1e-12 &&
		      comparison.fitted_rmse < 1e-12 &&
		      comparison.maximum_absolute_error == 507 &&
		      comparison.first_difference == 0,
	      "aligned comparison metrics are incomplete");
	CHECK(channel_bench_compare_window(
		      reference, ARRAY_LENGTH(reference), 0,
		      reference, ARRAY_LENGTH(reference), 0,
		      ARRAY_LENGTH(reference), &comparison) == CHANNEL_BENCH_OK &&
		      comparison.raw_rmse == 0.0 &&
		      comparison.maximum_absolute_error == 0 &&
		      comparison.first_difference == SIZE_MAX,
	      "exact recording comparison was not exact");
	CHECK(channel_bench_compare_window(
		      constant_reference, ARRAY_LENGTH(constant_reference), 0,
		      constant_observed, ARRAY_LENGTH(constant_observed), 0,
		      ARRAY_LENGTH(constant_reference), &comparison) ==
		      CHANNEL_BENCH_OK &&
		      !comparison.correlation_defined &&
		      comparison.raw_rmse == 2.0 && comparison.fitted_rmse == 0.0,
	      "constant recording metrics claimed a defined correlation");
	CHECK(channel_bench_find_alignment(
		      reference, ARRAY_LENGTH(reference), observed,
		      ARRAY_LENGTH(observed), 5, 100, &comparison) ==
		      CHANNEL_BENCH_NO_COMPARABLE_WINDOW,
	      "alignment accepted a window below the comparison floor");
	return true;
}

static bool write_all(int descriptor, const uint8_t *data, size_t size)
{
	size_t offset = 0;

	while (offset < size) {
		ssize_t written = write(descriptor, data + offset, size - offset);

		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return false;
		offset += (size_t)written;
	}
	return true;
}

static bool test_bounded_recording_loader(void)
{
	static const uint8_t bytes[] = {
		0x34, 0x12, 0xfe, 0xff, 0x00, 0x80, 0xff, 0x7f,
	};
	struct channel_bench_recording recording = { 0 };
	char path[] = "/tmp/channel-bench-XXXXXX";
	int descriptor = mkstemp(path);

	CHECK(descriptor >= 0, "could not create recording fixture");
	if (!write_all(descriptor, bytes, sizeof(bytes)) ||
	    close(descriptor) != 0) {
		unlink(path);
		CHECK(false, "could not write recording fixture");
	}
	CHECK(channel_bench_recording_load(path, 4, &recording) ==
		      CHANNEL_BENCH_OK,
	      "canonical recording did not load");
	CHECK(recording.count == 4 && recording.samples[0] == 0x1234 &&
		      recording.samples[1] == -2 &&
		      recording.samples[2] == INT16_MIN &&
		      recording.samples[3] == INT16_MAX,
	      "canonical recording byte order changed");
	channel_bench_recording_release(&recording);
	CHECK(channel_bench_recording_load(path, 3, &recording) ==
		      CHANNEL_BENCH_RECORDING_TOO_LARGE,
	      "recording loader ignored its sample bound");
	CHECK(truncate(path, (off_t)sizeof(bytes) - 1) == 0,
	      "could not shorten recording fixture");
	CHECK(channel_bench_recording_load(path, 4, &recording) ==
		      CHANNEL_BENCH_RECORDING_MALFORMED,
	      "recording loader accepted a partial sample");
	CHECK(unlink(path) == 0, "could not remove recording fixture");
	CHECK(channel_bench_recording_load(path, 4, &recording) ==
		      CHANNEL_BENCH_RECORDING_MISSING,
	      "missing optional recording was not distinguishable");
	return true;
}

static bool run_optional_external_comparison(void)
{
	const char *reference_path = getenv("MODEM_TEST_CHANNEL_REFERENCE");
	const char *observed_path = getenv("MODEM_TEST_CHANNEL_OBSERVED");
	struct channel_bench_recording reference = { 0 };
	struct channel_bench_recording observed = { 0 };
	struct channel_bench_comparison comparison;
	enum channel_bench_result reference_result;
	enum channel_bench_result observed_result;
	size_t reference_count;
	size_t observed_count;
	size_t shorter;
	size_t maximum_lag;
	size_t minimum_samples;
	bool passed = false;

	printf("[ RUN      ] optional external recording comparison\n");
	if (reference_path == NULL || reference_path[0] == '\0' ||
	    observed_path == NULL || observed_path[0] == '\0') {
		printf("[  SKIPPED ] optional external recording comparison "
		       "(set MODEM_TEST_CHANNEL_REFERENCE and "
		       "MODEM_TEST_CHANNEL_OBSERVED)\n");
		return true;
	}
	reference_result = channel_bench_recording_load(
		reference_path, EXTERNAL_MAXIMUM_SAMPLES, &reference);
	observed_result = channel_bench_recording_load(
		observed_path, EXTERNAL_MAXIMUM_SAMPLES, &observed);
	if (reference_result == CHANNEL_BENCH_RECORDING_MISSING ||
	    observed_result == CHANNEL_BENCH_RECORDING_MISSING) {
		printf("[  SKIPPED ] optional external recording comparison "
		       "(external evidence is absent)\n");
		passed = true;
		goto done;
	}
	if (reference_result != CHANNEL_BENCH_OK ||
	    observed_result != CHANNEL_BENCH_OK) {
		fprintf(stderr,
			"external recording load failed: reference=%s observed=%s\n",
			channel_bench_result_name(reference_result),
			channel_bench_result_name(observed_result));
		goto done;
	}
	reference_count = reference.count;
	if (reference_count > EXTERNAL_COMPARE_SAMPLES + EXTERNAL_MAXIMUM_LAG)
		reference_count = EXTERNAL_COMPARE_SAMPLES + EXTERNAL_MAXIMUM_LAG;
	observed_count = observed.count;
	if (observed_count > EXTERNAL_COMPARE_SAMPLES + EXTERNAL_MAXIMUM_LAG)
		observed_count = EXTERNAL_COMPARE_SAMPLES + EXTERNAL_MAXIMUM_LAG;
	shorter = reference_count < observed_count ?
			  reference_count : observed_count;
	if (shorter == 0) {
		fprintf(stderr, "external recording is empty\n");
		goto done;
	}
	maximum_lag = shorter > 1 ? EXTERNAL_MAXIMUM_LAG : 0;
	if (maximum_lag >= shorter)
		maximum_lag = shorter - 1;
	minimum_samples = shorter - maximum_lag;
	if (channel_bench_find_alignment(
		    reference.samples, reference_count, observed.samples,
		    observed_count, maximum_lag, minimum_samples,
		    &comparison) != CHANNEL_BENCH_OK) {
		fprintf(stderr, "external recordings have no comparable window\n");
		goto done;
	}
	printf("[  EVIDENCE ] samples=%zu lag=%td correlation=%s%.9f "
	       "gain=%.9f bias=%.3f raw_rmse=%.3f fitted_rmse=%.3f "
	       "max_error=%u\n",
	       comparison.compared_samples, comparison.lag_samples,
	       comparison.correlation_defined ? "" : "undefined/",
	       comparison.correlation, comparison.fitted_gain,
	       comparison.fitted_bias, comparison.raw_rmse,
	       comparison.fitted_rmse, comparison.maximum_absolute_error);
	printf("[       OK ] optional external recording comparison "
	       "(evidence only; no ground-truth threshold)\n");
	passed = true;

done:
	channel_bench_recording_release(&observed);
	channel_bench_recording_release(&reference);
	if (!passed)
		printf("[  FAILED  ] optional external recording comparison\n");
	return passed;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "deterministic generators", test_deterministic_generators },
		{ "effect order and chunk invariance",
		  test_effect_order_and_chunk_invariance },
		{ "noise and codec effects", test_noise_and_codec_effects },
		{ "fractional delay effect", test_fractional_delay_effect },
		{ "validation and saturation", test_validation_and_saturation },
		{ "comparison metrics and alignment",
		  test_comparison_metrics_and_alignment },
		{ "bounded recording loader", test_bounded_recording_loader },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		printf("[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			printf("[       OK ] %s\n", tests[index].name);
		else {
			printf("[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	if (!run_optional_external_comparison())
		failures++;
	return failures == 0 ? 0 : 1;
}
