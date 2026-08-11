// SPDX-License-Identifier: BSD-3-Clause

#include <v44_tx_monitor.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum { DATA_LENGTH = 512 };

static const struct v44_parameters parameters = {
	.codeword_limit = V44_MIN_CODEWORDS,
	.max_string_length = V44_MIN_STRING_LENGTH,
	.history_size = V44_MIN_HISTORY_SIZE,
};

static struct v44_tx_monitor_config test_config(void)
{
	const struct v44_tx_monitor_config config = {
		.window_octets = V44_MIN_STRING_LENGTH,
		.leave_compressed_excess_bits = UINT32_MAX,
		.enter_compressed_savings_bits = UINT32_MAX,
	};

	return config;
}

static void generate_noise(uint8_t *data, size_t length)
{
	uint32_t state = UINT32_C(0x9216ac43);

	for (size_t index = 0; index < length; ++index) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		data[index] = (uint8_t)state;
	}
}

static bool collect_sample(
	struct v44_tx_monitor *monitor,
	const uint8_t *data,
	size_t length,
	struct v44_tx_monitor_sample *sample,
	size_t *consumed)
{
	for (size_t index = 0; index < length; ++index) {
		enum v44_tx_monitor_decision decision =
			V44_TX_MONITOR_ENTER_TRANSPARENT;

		if (v44_tx_monitor_observe(
			    monitor, data[index], &decision) !=
		    V44_TX_MONITOR_OK)
			return false;
		if (v44_tx_monitor_get_last_sample(
			    monitor, sample)) {
			*consumed = index + 1;
			return decision == sample->decision;
		}
	}
	return false;
}

static bool sample_stream(
	const struct v44_tx_monitor_config *config,
	enum v44_encoder_mode mode,
	const uint8_t *data,
	size_t length,
	struct v44_tx_monitor_sample *sample,
	size_t *consumed)
{
	struct v44_tx_monitor *monitor =
		v44_tx_monitor_create(&parameters, config);
	bool sampled = false;

	if (monitor != NULL &&
	    (mode == V44_ENCODER_COMPRESSED ||
	     v44_tx_monitor_confirm_mode(monitor, mode) ==
		     V44_TX_MONITOR_OK))
		sampled = collect_sample(
			monitor, data, length, sample, consumed);
	v44_tx_monitor_destroy(monitor);
	return sampled;
}

static bool test_defaults_and_api(void)
{
	const struct v44_tx_monitor_config defaults =
		v44_tx_monitor_default_config();
	struct v44_tx_monitor_config invalid = test_config();
	struct v44_tx_monitor *monitor;
	struct v44_tx_monitor_sample unchanged = {
		.mode = V44_ENCODER_TRANSPARENT,
		.represented_octets = 41,
		.compressed_bits = 73,
		.decision = V44_TX_MONITOR_ENTER_COMPRESSED,
	};
	enum v44_tx_monitor_decision decision =
		V44_TX_MONITOR_ENTER_TRANSPARENT;

	CHECK(defaults.window_octets == 256 &&
	      defaults.leave_compressed_excess_bits == 32 &&
	      defaults.enter_compressed_savings_bits == 64,
	      "V.44 monitor defaults changed");
	invalid.window_octets = parameters.max_string_length - 1;
	CHECK(v44_tx_monitor_create(NULL, &defaults) == NULL &&
	      v44_tx_monitor_create(&parameters, NULL) == NULL &&
	      v44_tx_monitor_create(&parameters, &invalid) == NULL,
	      "V.44 monitor accepted invalid construction");
	monitor = v44_tx_monitor_create(&parameters, &defaults);
	CHECK(monitor != NULL &&
	      !v44_tx_monitor_get_last_sample(
		      monitor, &unchanged) &&
	      unchanged.mode == V44_ENCODER_TRANSPARENT &&
	      unchanged.represented_octets == 41 &&
	      unchanged.compressed_bits == 73 &&
	      unchanged.decision ==
		      V44_TX_MONITOR_ENTER_COMPRESSED &&
	      !v44_tx_monitor_get_last_sample(monitor, NULL),
	      "V.44 monitor exposed a nonexistent sample");
	CHECK(v44_tx_monitor_observe(NULL, 0, &decision) ==
		      V44_TX_MONITOR_INVALID_ARGUMENT &&
	      decision == V44_TX_MONITOR_ENTER_TRANSPARENT &&
	      v44_tx_monitor_observe(monitor, 0, NULL) ==
		      V44_TX_MONITOR_INVALID_ARGUMENT &&
	      v44_tx_monitor_confirm_mode(
		      monitor, (enum v44_encoder_mode)2) ==
		      V44_TX_MONITOR_INVALID_ARGUMENT &&
	      v44_tx_monitor_confirm_mode(
		      monitor, V44_ENCODER_COMPRESSED) ==
		      V44_TX_MONITOR_OK &&
	      v44_tx_monitor_flush(monitor, &decision) ==
		      V44_TX_MONITOR_OK,
	      "V.44 monitor API validation was not transactional");
	CHECK(strcmp(v44_tx_monitor_result_name(
			     V44_TX_MONITOR_DISABLED),
		     "disabled") == 0 &&
	      strcmp(v44_tx_monitor_result_name(
			     (enum v44_tx_monitor_result)99),
		     "unknown") == 0,
	      "V.44 monitor result names changed");
	v44_tx_monitor_destroy(monitor);
	v44_tx_monitor_destroy(NULL);
	return true;
}

static bool test_leave_compressed_threshold(void)
{
	uint8_t data[DATA_LENGTH];
	struct v44_tx_monitor_config config = test_config();
	struct v44_tx_monitor_sample measured;
	struct v44_tx_monitor_sample equal;
	struct v44_tx_monitor_sample exceeded;
	size_t measured_length = 0;
	size_t equal_length = 0;
	size_t exceeded_length = 0;
	uint64_t raw_bits;
	uint64_t excess;

	generate_noise(data, sizeof(data));
	CHECK(sample_stream(
		      &config, V44_ENCODER_COMPRESSED, data,
		      sizeof(data), &measured, &measured_length),
	      "could not measure a generated incompressible V.44 sample");
	raw_bits = measured.represented_octets * 8U;
	CHECK(measured.mode == V44_ENCODER_COMPRESSED &&
	      measured.decision == V44_TX_MONITOR_STAY &&
	      measured.compressed_bits > raw_bits &&
	      measured.compressed_bits - raw_bits <= UINT32_MAX,
	      "generated V.44 sample did not expose compression overhead");
	excess = measured.compressed_bits - raw_bits;
	config.leave_compressed_excess_bits = (uint32_t)excess;
	CHECK(sample_stream(
		      &config, V44_ENCODER_COMPRESSED, data,
		      measured_length, &equal, &equal_length) &&
	      equal_length == measured_length &&
	      equal.represented_octets == measured.represented_octets &&
	      equal.compressed_bits == measured.compressed_bits &&
	      equal.decision == V44_TX_MONITOR_STAY,
	      "V.44 leave threshold did not keep equality compressed");
	config.leave_compressed_excess_bits = (uint32_t)excess - 1U;
	CHECK(sample_stream(
		      &config, V44_ENCODER_COMPRESSED, data,
		      measured_length, &exceeded, &exceeded_length) &&
	      exceeded_length == measured_length &&
	      exceeded.decision ==
		      V44_TX_MONITOR_ENTER_TRANSPARENT,
	      "V.44 leave threshold ignored one excess bit");
	return true;
}

static bool test_enter_compressed_threshold_and_latch(void)
{
	uint8_t data[DATA_LENGTH];
	struct v44_tx_monitor_config config = test_config();
	struct v44_tx_monitor_sample measured;
	struct v44_tx_monitor_sample equal;
	struct v44_tx_monitor_sample exceeded;
	struct v44_tx_monitor *monitor;
	size_t measured_length = 0;
	size_t equal_length = 0;
	size_t exceeded_length = 0;
	uint64_t raw_bits;
	uint64_t savings;
	enum v44_tx_monitor_decision decision =
		V44_TX_MONITOR_ENTER_TRANSPARENT;

	memset(data, 'A', sizeof(data));
	CHECK(sample_stream(
		      &config, V44_ENCODER_TRANSPARENT, data,
		      sizeof(data), &measured, &measured_length),
	      "could not measure a generated compressible V.44 sample");
	raw_bits = measured.represented_octets * 8U;
	CHECK(measured.mode == V44_ENCODER_TRANSPARENT &&
	      measured.decision == V44_TX_MONITOR_STAY &&
	      raw_bits > measured.compressed_bits &&
	      raw_bits - measured.compressed_bits <= UINT32_MAX,
	      "generated V.44 sample did not expose compression savings");
	savings = raw_bits - measured.compressed_bits;
	config.enter_compressed_savings_bits = (uint32_t)savings;
	CHECK(sample_stream(
		      &config, V44_ENCODER_TRANSPARENT, data,
		      measured_length, &equal, &equal_length) &&
	      equal_length == measured_length &&
	      equal.represented_octets == measured.represented_octets &&
	      equal.compressed_bits == measured.compressed_bits &&
	      equal.decision == V44_TX_MONITOR_STAY,
	      "V.44 enter threshold did not keep equality transparent");
	config.enter_compressed_savings_bits = (uint32_t)savings - 1U;
	monitor = v44_tx_monitor_create(&parameters, &config);
	CHECK(monitor != NULL &&
	      v44_tx_monitor_confirm_mode(
		      monitor, V44_ENCODER_TRANSPARENT) ==
		      V44_TX_MONITOR_OK &&
	      collect_sample(
		      monitor, data, measured_length, &exceeded,
		      &exceeded_length) &&
	      exceeded_length == measured_length &&
	      exceeded.decision ==
		      V44_TX_MONITOR_ENTER_COMPRESSED,
	      "V.44 enter threshold ignored one saved bit");
	CHECK(v44_tx_monitor_observe(monitor, 'B', &decision) ==
		      V44_TX_MONITOR_INVALID_STATE &&
	      decision == V44_TX_MONITOR_STAY &&
	      v44_tx_monitor_flush(monitor, &decision) ==
		      V44_TX_MONITOR_INVALID_STATE &&
	      v44_tx_monitor_confirm_mode(
		      monitor, V44_ENCODER_TRANSPARENT) ==
		      V44_TX_MONITOR_INVALID_STATE &&
	      v44_tx_monitor_confirm_mode(
		      monitor, V44_ENCODER_COMPRESSED) ==
		      V44_TX_MONITOR_OK,
	      "V.44 monitor did not latch its transition decision");
	v44_tx_monitor_destroy(monitor);
	return true;
}

static bool test_flush_cost(void)
{
	uint8_t data[DATA_LENGTH];
	const struct v44_tx_monitor_config config = test_config();
	struct v44_tx_monitor_sample plain;
	struct v44_tx_monitor_sample flushed;
	struct v44_tx_monitor *monitor;
	size_t plain_length = 0;
	size_t flushed_length = 0;
	enum v44_tx_monitor_decision decision =
		V44_TX_MONITOR_ENTER_TRANSPARENT;

	generate_noise(data, sizeof(data));
	CHECK(sample_stream(
		      &config, V44_ENCODER_COMPRESSED, data,
		      sizeof(data), &plain, &plain_length),
	      "could not collect an unflushed V.44 monitor sample");
	monitor = v44_tx_monitor_create(&parameters, &config);
	CHECK(monitor != NULL &&
	      v44_tx_monitor_flush(monitor, &decision) ==
		      V44_TX_MONITOR_OK &&
	      decision == V44_TX_MONITOR_STAY &&
	      collect_sample(
		      monitor, data, plain_length, &flushed,
		      &flushed_length) &&
	      flushed_length == plain_length &&
	      flushed.represented_octets == plain.represented_octets &&
	      flushed.compressed_bits == plain.compressed_bits + 8U,
	      "V.44 monitor did not include an exact mirrored C-FLUSH cost");
	v44_tx_monitor_destroy(monitor);
	return true;
}

static bool test_flush_finalizes_sample(void)
{
	const struct v44_tx_monitor_config config = test_config();
	struct v44_tx_monitor *monitor =
		v44_tx_monitor_create(&parameters, &config);
	struct v44_tx_monitor_sample sample;
	enum v44_tx_monitor_decision decision =
		V44_TX_MONITOR_ENTER_TRANSPARENT;

	CHECK(monitor != NULL, "could not create V.44 flush-boundary monitor");
	for (uint32_t index = 0; index < config.window_octets; ++index) {
		CHECK(v44_tx_monitor_observe(
			      monitor, 'A', &decision) ==
			      V44_TX_MONITOR_OK &&
		      decision == V44_TX_MONITOR_STAY,
		      "could not feed V.44 lookahead to a flush boundary");
	}
	CHECK(!v44_tx_monitor_get_last_sample(monitor, &sample) &&
	      v44_tx_monitor_flush(monitor, &decision) ==
		      V44_TX_MONITOR_OK &&
	      decision == V44_TX_MONITOR_STAY &&
	      v44_tx_monitor_get_last_sample(monitor, &sample) &&
	      sample.mode == V44_ENCODER_COMPRESSED &&
	      sample.represented_octets == config.window_octets,
	      "C-FLUSH did not expose its newly completed V.44 sample");
	v44_tx_monitor_destroy(monitor);
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "defaults and API", test_defaults_and_api },
		{ "leave compressed threshold",
		  test_leave_compressed_threshold },
		{ "enter compressed threshold and latch",
		  test_enter_compressed_threshold_and_latch },
		{ "flush cost", test_flush_cost },
		{ "flush finalizes sample", test_flush_finalizes_sample },
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
