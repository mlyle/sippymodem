// SPDX-License-Identifier: BSD-3-Clause

#include <v8_line_conditioner.h>

#include "test.h"

#include <inttypes.h>
#include <limits.h>
#include <string.h>

enum {
	PROFILE_SAMPLES = 4096,
};

struct profile_oracle {
	uint64_t output_hash;
	uint64_t state_hash;
	uint16_t gain;
	uint16_t gain_delta;
	uint16_t filtered_energy;
	enum v8_line_gain_mode gain_mode;
};

static uint64_t hash_byte(uint64_t hash, uint8_t byte)
{
	return (hash ^ byte) * UINT64_C(1099511628211);
}

static uint64_t hash_u16(uint64_t hash, uint16_t value)
{
	hash = hash_byte(hash, (uint8_t)value);
	return hash_byte(hash, (uint8_t)(value >> 8));
}

static uint64_t hash_state(
	uint64_t hash, const struct v8_line_conditioner *line)
{
	for (size_t index = 0;
	     index < V8_LINE_ENTRANCE_FILTER_TAPS; ++index)
		hash = hash_u16(
			hash, (uint16_t)line->entrance_history[index]);
	for (size_t index = 0;
	     index < V8_LINE_INPUT_QUEUE_SAMPLES; ++index)
		hash = hash_u16(hash, (uint16_t)line->input_queue[index]);
	for (size_t index = 0;
	     index < V8_LINE_POWER_HISTORY_SAMPLES; ++index)
		hash = hash_u16(hash, line->power_history[index]);
	hash = hash_u16(hash, line->filtered_energy);
	hash = hash_u16(hash, line->gain);
	hash = hash_u16(hash, line->gain_delta);
	hash = hash_u16(hash, line->post_filter_word);
	hash = hash_u16(hash, line->power_index);
	hash = hash_u16(hash, line->input_count);
	hash = hash_u16(hash, line->overflow_count);
	hash = hash_u16(hash, (uint16_t)line->role);
	return hash_u16(hash, (uint16_t)line->gain_mode);
}

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static bool test_exact_profiles(void)
{
	static const struct profile_oracle expected[] = {
		{
			.output_hash = UINT64_C(0x8767808c80721bf6),
			.state_hash = UINT64_C(0x686f7fb5cb17e105),
			.gain = 512,
			.gain_delta = 0,
			.filtered_energy = 19166,
			.gain_mode = V8_LINE_GAIN_FROZEN,
		},
		{
			.output_hash = UINT64_C(0x834e346b295e4ce9),
			.state_hash = UINT64_C(0x80ece610a974862f),
			.gain = 415,
			.gain_delta = 547,
			.filtered_energy = 5707,
			.gain_mode = V8_LINE_GAIN_COARSE,
		},
	};
	struct v8_line_conditioner lines[ARRAY_LENGTH(expected)];
	uint32_t random[ARRAY_LENGTH(expected)];
	uint64_t output_hash[ARRAY_LENGTH(expected)];
	size_t blocks[ARRAY_LENGTH(expected)] = { 0 };

	for (size_t role = 0; role < ARRAY_LENGTH(expected); ++role) {
		random[role] = UINT32_C(0x7bd31e25);
		output_hash[role] = UINT64_C(1469598103934665603);
		CHECK(v8_line_conditioner_init(
			      &lines[role], (enum v8_station_role)role) ==
			      V8_LINE_CONDITIONER_OK,
		      "V.8 line profile %zu did not initialize", role);
	}
	for (size_t sample_index = 0;
	     sample_index < PROFILE_SAMPLES; ++sample_index) {
		for (size_t role = 0; role < ARRAY_LENGTH(expected); ++role) {
			struct v8_line_conditioned_block block = {
				.samples = { 123, 456, 789, 1011 },
				.gain_reset = true,
			};
			struct v8_line_conditioned_block untouched = block;
			int16_t sample = (int16_t)(
				fixture_random(&random[role]) >> 16);
			bool ready = true;

			CHECK(v8_line_conditioner_process_sample(
				      &lines[role], sample, &block, &ready) ==
				      V8_LINE_CONDITIONER_OK,
			      "V.8 line profile %zu rejected sample %zu",
			      role, sample_index);
			if (!ready) {
				CHECK(memcmp(&block, &untouched, sizeof(block)) == 0,
				      "V.8 line changed an unavailable block");
				continue;
			}
			CHECK(sample_index >= 5 &&
			      (sample_index - 5) % V8_LINE_BLOCK_SAMPLES == 0,
			      "V.8 line profile %zu changed cadence at %zu",
			      role, sample_index);
			blocks[role]++;
			for (size_t index = 0;
			     index < V8_LINE_BLOCK_SAMPLES; ++index)
				output_hash[role] = hash_u16(
					output_hash[role],
					(uint16_t)block.samples[index]);
			output_hash[role] = hash_byte(
				output_hash[role], block.gain_reset);
		}
	}
	for (size_t role = 0; role < ARRAY_LENGTH(expected); ++role) {
		uint64_t state_hash = hash_state(
			UINT64_C(1469598103934665603), &lines[role]);

		CHECK(blocks[role] == 1023 &&
		      output_hash[role] == expected[role].output_hash &&
		      state_hash == expected[role].state_hash &&
		      lines[role].gain == expected[role].gain &&
		      lines[role].gain_delta == expected[role].gain_delta &&
		      lines[role].filtered_energy ==
			      expected[role].filtered_energy &&
		      lines[role].gain_mode == expected[role].gain_mode &&
		      lines[role].input_count == 4 &&
		      lines[role].power_index == 12,
		      "V.8 line profile %zu changed: %zu/%016" PRIx64
		      "/%016" PRIx64,
		      role, blocks[role], output_hash[role], state_hash);
	}
	return true;
}

static bool test_clipping_recovery(void)
{
	struct v8_line_conditioner line;
	bool reset = false;

	CHECK(v8_line_conditioner_init(&line, V8_CALLING_STATION) ==
		      V8_LINE_CONDITIONER_OK,
	      "V.8 clipping fixture did not initialize");
	line.gain = 0x7fff;
	for (size_t sample_index = 0; sample_index <= 25; ++sample_index) {
		struct v8_line_conditioned_block block;
		int16_t sample = sample_index & 1 ? INT16_MAX : INT16_MIN;
		bool ready;

		CHECK(v8_line_conditioner_process_sample(
			      &line, sample, &block, &ready) ==
			      V8_LINE_CONDITIONER_OK,
		      "V.8 clipping fixture rejected sample %zu", sample_index);
		if (!ready || !block.gain_reset)
			continue;
		CHECK(sample_index == 25 && line.gain == 0x0400 &&
		      line.overflow_count == 0 &&
		      block.samples[0] == 32512 &&
		      block.samples[1] == -32512 &&
		      block.samples[2] == -760 && block.samples[3] == -181,
		      "V.8 clipping recovery changed at sample %zu", sample_index);
		reset = true;
	}
	CHECK(reset, "V.8 clipping recovery did not reset gain");
	return true;
}

static bool test_contracts(void)
{
	struct v8_line_conditioner line;
	struct v8_line_conditioner saved;
	struct v8_line_conditioner expected;
	struct v8_line_conditioned_block block = {
		.samples = { 1, 2, 3, 4 },
		.gain_reset = true,
	};
	struct v8_line_conditioned_block saved_block = block;
	bool ready = true;

	CHECK(v8_line_conditioner_init(&line, V8_CALLING_STATION) ==
		      V8_LINE_CONDITIONER_OK &&
	      v8_line_conditioner_is_valid(&line),
	      "V.8 line contract fixture did not initialize");
	saved = line;
	CHECK(v8_line_conditioner_init(NULL, V8_CALLING_STATION) ==
		      V8_LINE_CONDITIONER_INVALID_ARGUMENT &&
	      v8_line_conditioner_init(
		      &line, (enum v8_station_role)99) ==
		      V8_LINE_CONDITIONER_INVALID_ARGUMENT &&
	      memcmp(&line, &saved, sizeof(line)) == 0,
	      "V.8 line initialization changed state on rejection");
	expected = line;
	expected.gain_mode = V8_LINE_GAIN_FINE;
	CHECK(v8_line_conditioner_set_gain_mode(
		      &line, V8_LINE_GAIN_FINE) == V8_LINE_CONDITIONER_OK &&
	      memcmp(&line, &expected, sizeof(line)) == 0,
	      "V.8 line gain-mode transition changed unrelated state");
	saved = line;
	CHECK(v8_line_conditioner_set_gain_mode(NULL, V8_LINE_GAIN_FINE) ==
		      V8_LINE_CONDITIONER_INVALID_ARGUMENT &&
	      v8_line_conditioner_set_gain_mode(
		      &line, (enum v8_line_gain_mode)99) ==
		      V8_LINE_CONDITIONER_INVALID_ARGUMENT &&
	      memcmp(&line, &saved, sizeof(line)) == 0,
	      "V.8 line gain mode accepted an invalid transition");
	CHECK(v8_line_conditioner_process_sample(NULL, 0, &block, &ready) ==
		      V8_LINE_CONDITIONER_INVALID_ARGUMENT &&
	      v8_line_conditioner_process_sample(&line, 0, NULL, &ready) ==
		      V8_LINE_CONDITIONER_INVALID_ARGUMENT &&
	      v8_line_conditioner_process_sample(&line, 0, &block, NULL) ==
		      V8_LINE_CONDITIONER_INVALID_ARGUMENT &&
	      v8_line_conditioner_process_sample(
		      &line, 0, (struct v8_line_conditioned_block *)&line,
		      &ready) == V8_LINE_CONDITIONER_INVALID_ARGUMENT &&
	      v8_line_conditioner_process_sample(
		      &line, 0, &block, (bool *)&line) ==
		      V8_LINE_CONDITIONER_INVALID_ARGUMENT &&
	      v8_line_conditioner_process_sample(
		      &line, 0, &block, (bool *)&block) ==
		      V8_LINE_CONDITIONER_INVALID_ARGUMENT &&
	      memcmp(&line, &saved, sizeof(line)) == 0 &&
	      memcmp(&block, &saved_block, sizeof(block)) == 0 && ready,
	      "V.8 line overlap rejection changed state or output");
	line.gain = 0;
	saved = line;
	CHECK(!v8_line_conditioner_is_valid(&line) &&
	      v8_line_conditioner_process_sample(
		      &line, 0, &block, &ready) ==
		      V8_LINE_CONDITIONER_INVALID_ARGUMENT &&
	      v8_line_conditioner_set_gain_mode(
		      &line, V8_LINE_GAIN_COARSE) ==
		      V8_LINE_CONDITIONER_INVALID_ARGUMENT &&
	      memcmp(&line, &saved, sizeof(line)) == 0 &&
	      memcmp(&block, &saved_block, sizeof(block)) == 0 && ready,
	      "V.8 line accepted corrupt owned state");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "exact profiles", test_exact_profiles },
		{ "clipping recovery", test_clipping_recovery },
		{ "contracts", test_contracts },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		else {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
