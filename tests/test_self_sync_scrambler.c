// SPDX-License-Identifier: BSD-3-Clause

#include <self_sync_scrambler.h>

#include "test.h"

#include <string.h>

static const struct self_sync_scrambler_config test_configs[] = {
	{ .history_bits = 23, .tap_a_delay = 18, .tap_b_delay = 23 },
	{ .history_bits = 23, .tap_a_delay = 5, .tap_b_delay = 23 },
	{ .history_bits = 32, .tap_a_delay = 17, .tap_b_delay = 32 },
	{ .history_bits = 2, .tap_a_delay = 1, .tap_b_delay = 2 },
};

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static bool test_impulse_responses(void)
{
	for (size_t fixture = 0; fixture < ARRAY_LENGTH(test_configs);
	     ++fixture) {
		const struct self_sync_scrambler_config *config =
			&test_configs[fixture];
		struct self_sync_scrambler descrambler;

		CHECK(self_sync_scrambler_init(
			      &descrambler, config) ==
			      SELF_SYNC_SCRAMBLER_OK,
		      "impulse fixture did not initialize");
		for (unsigned int position = 0;
		     position <= config->history_bits; ++position) {
			uint8_t input = position == 0 ? 1u : 0u;
			uint8_t output;
			uint8_t expected =
				position == 0 || position == config->tap_a_delay ||
				position == config->tap_b_delay;

			CHECK(self_sync_descramble_group(
				      &descrambler, config, input, 1, &output) ==
				      SELF_SYNC_SCRAMBLER_OK &&
			      output == expected,
			      "impulse response changed at %zu/%u", fixture,
			      position);
		}
	}
	return true;
}

static bool test_normative_group_starts(void)
{
	static const uint8_t expected[][15] = {
		{ 3, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 1, 3, 3, 3 },
		{ 3, 3, 2, 0, 0, 3, 3, 2, 0, 0, 3, 2, 1, 3, 3 },
	};

	for (size_t fixture = 0; fixture < ARRAY_LENGTH(expected); ++fixture) {
		struct self_sync_scrambler scrambler;

		CHECK(self_sync_scrambler_init(
			      &scrambler, &test_configs[fixture]) ==
			      SELF_SYNC_SCRAMBLER_OK,
		      "normative fixture did not initialize");
		for (size_t index = 0; index < ARRAY_LENGTH(expected[fixture]);
		     ++index) {
			uint8_t output;

			CHECK(self_sync_scramble_group(
				      &scrambler, &test_configs[fixture], 3, 2,
				      &output) ==
				      SELF_SYNC_SCRAMBLER_OK &&
			      output == expected[fixture][index],
			      "normative group changed at %zu/%zu", fixture,
			      index);
		}
	}
	return true;
}

static bool test_group_round_trip(void)
{
	uint32_t random = UINT32_C(0x51fba273);

	for (size_t fixture = 0; fixture < ARRAY_LENGTH(test_configs);
	     ++fixture) {
		struct self_sync_scrambler scrambler;
		struct self_sync_scrambler descrambler;

		CHECK(self_sync_scrambler_init(
			      &scrambler, &test_configs[fixture]) ==
			      SELF_SYNC_SCRAMBLER_OK &&
		      self_sync_scrambler_init(
			      &descrambler, &test_configs[fixture]) ==
			      SELF_SYNC_SCRAMBLER_OK,
		      "round-trip fixtures did not initialize");
		for (size_t index = 0; index < 1024; ++index) {
			unsigned int bit_count = (unsigned int)(index % 8u) + 1u;
			uint8_t mask = (uint8_t)((1u << bit_count) - 1u);
			uint8_t input = (uint8_t)fixture_random(&random) & mask;
			uint8_t encoded;
			uint8_t output;

			CHECK(self_sync_scramble_group(
				      &scrambler, &test_configs[fixture], input,
				      bit_count, &encoded) ==
				      SELF_SYNC_SCRAMBLER_OK &&
			      self_sync_descramble_group(
				      &descrambler, &test_configs[fixture], encoded,
				      bit_count, &output) ==
				      SELF_SYNC_SCRAMBLER_OK &&
			      output == input,
			      "group round trip changed at %zu/%zu", fixture,
			      index);
		}
		CHECK(scrambler.history == descrambler.history,
		      "round-trip histories diverged at %zu", fixture);
	}
	return true;
}

static bool test_descrambler_resynchronization(void)
{
	uint32_t random = UINT32_C(0x9e6c301d);

	for (size_t fixture = 0; fixture < ARRAY_LENGTH(test_configs);
	     ++fixture) {
		const struct self_sync_scrambler_config *config =
			&test_configs[fixture];
		struct self_sync_scrambler scrambler = { 0 };
		struct self_sync_scrambler descrambler = {
			.history = UINT32_MAX,
		};

		if (config->history_bits < 32)
			descrambler.history >>= 32u - config->history_bits;
		for (unsigned int position = 0;
		     position < config->history_bits + 32u; ++position) {
			uint8_t input = (uint8_t)(fixture_random(&random) & 1u);
			uint8_t encoded;
			uint8_t output;

			CHECK(self_sync_scramble_group(
				      &scrambler, config, input, 1, &encoded) ==
				      SELF_SYNC_SCRAMBLER_OK &&
			      self_sync_descramble_group(
				      &descrambler, config, encoded, 1, &output) ==
				      SELF_SYNC_SCRAMBLER_OK &&
			      (position < config->history_bits || output == input),
			      "descrambler did not synchronize at %zu/%u", fixture,
			      position);
		}
		CHECK(scrambler.history == descrambler.history,
		      "descrambler history did not converge at %zu", fixture);
	}
	return true;
}

static bool test_unpacked_streams(void)
{
	uint32_t random = UINT32_C(0x294f813b);

	for (size_t fixture = 0; fixture < ARRAY_LENGTH(test_configs); ++fixture)
	for (unsigned int direction = 0; direction < 2; ++direction)
	for (unsigned int inplace = 0; inplace < 2; ++inplace) {
		const struct self_sync_scrambler_config *config = &test_configs[fixture];
		uint8_t input[257], expected[257], output[257], past[32];
		struct self_sync_scrambler state = { 0 };
		uint32_t expected_history = 0;

		for (unsigned int bit = 0; bit < config->history_bits; ++bit) {
			past[bit] = fixture_random(&random) >> 31;
			state.history |= (uint32_t)past[bit] << bit;
		}
		/* An explicit delay line is independent of the packed recurrence. */
		for (size_t bit = 0; bit < sizeof(input); ++bit) {
			input[bit] = fixture_random(&random) >> 31;
			expected[bit] = input[bit] ^ past[config->tap_a_delay - 1] ^
				past[config->tap_b_delay - 1];
			memmove(past + 1, past, config->history_bits - 1);
			past[0] = direction ? input[bit] : expected[bit];
		}
		for (unsigned int bit = 0; bit < config->history_bits; ++bit)
			expected_history |= (uint32_t)past[bit] << bit;
		memcpy(output, input, sizeof(input));
		for (size_t offset = 0; offset < sizeof(input);) {
			size_t count = offset % 17 + 1;
			if (count > sizeof(input) - offset)
				count = sizeof(input) - offset;
			if (count <= 8 && (offset & 1u)) {
				uint8_t group = 0;
				for (size_t bit = 0; bit < count; ++bit)
					group = (uint8_t)((group << 1) | input[offset + bit]);
				CHECK((direction ? self_sync_descramble_group :
					self_sync_scramble_group)(&state, config, group,
					count, &group) == SELF_SYNC_SCRAMBLER_OK,
				      "mixed group rejected at %zu", offset);
				for (size_t bit = 0; bit < count; ++bit)
					output[offset + bit] = (group >> (count - bit - 1)) & 1u;
			} else {
				CHECK((direction ? self_sync_descramble_unpacked_bits :
					self_sync_scramble_unpacked_bits)(&state, config,
					inplace ? output + offset : input + offset,
					count, output + offset) == SELF_SYNC_SCRAMBLER_OK,
				      "unpacked chunk rejected at %zu", offset);
			}
			offset += count;
		}
		CHECK(memcmp(output, expected, sizeof(output)) == 0 &&
		      state.history == expected_history,
		      "unpacked delay-line result changed at %zu/%u/%u",
		      fixture, direction, inplace);
	}
	return true;
}

static bool test_unpacked_rejection(void)
{
	for (unsigned int direction = 0; direction < 2; ++direction) {
		struct self_sync_scrambler_config config = { 8, 3, 8 };
		struct self_sync_scrambler state = { .history = 0x5a };
		uint8_t input[17] = { 0 }, output[17], saved[17];
		enum self_sync_scrambler_result (*transform)(
			struct self_sync_scrambler *,
			const struct self_sync_scrambler_config *,
			const uint8_t *, size_t, uint8_t *) = direction ?
			self_sync_descramble_unpacked_bits : self_sync_scramble_unpacked_bits;

		memset(output, 0xa5, sizeof(output));
		memcpy(saved, output, sizeof(saved));
		input[16] = 2;
		CHECK(transform(&state, &config, input, sizeof(input), output) ==
		      SELF_SYNC_SCRAMBLER_INVALID_ARGUMENT && state.history == 0x5a &&
		      memcmp(output, saved, sizeof(output)) == 0,
		      "late nonbinary bit partially committed");
		uint8_t invalid[sizeof(input)];
		memcpy(invalid, input, sizeof(input));
		CHECK(transform(&state, &config, input, sizeof(input), input) ==
		      SELF_SYNC_SCRAMBLER_INVALID_ARGUMENT && state.history == 0x5a &&
		      memcmp(input, invalid, sizeof(input)) == 0,
		      "late nonbinary bit partially committed in place");
		input[16] = 0;
		CHECK(transform(&state, &config, input, 0, output) ==
		      SELF_SYNC_SCRAMBLER_INVALID_ARGUMENT &&
		      transform(NULL, &config, input, 1, output) ==
		      SELF_SYNC_SCRAMBLER_INVALID_ARGUMENT &&
		      transform(&state, NULL, input, 1, output) ==
		      SELF_SYNC_SCRAMBLER_INVALID_ARGUMENT &&
		      transform(&state, &config, NULL, 1, output) ==
		      SELF_SYNC_SCRAMBLER_INVALID_ARGUMENT &&
		      transform(&state, &config, input, 1, NULL) ==
		      SELF_SYNC_SCRAMBLER_INVALID_ARGUMENT && state.history == 0x5a &&
		      memcmp(output, saved, sizeof(output)) == 0,
		      "invalid unpacked arguments partially committed");
		CHECK(transform(&state, &config, output, 8, output + 1) ==
		      SELF_SYNC_SCRAMBLER_INVALID_ARGUMENT &&
		      transform(&state, &config, input, 1, (uint8_t *)&state) ==
		      SELF_SYNC_SCRAMBLER_INVALID_ARGUMENT &&
		      transform(&state, &config, (uint8_t *)&state, 1, output) ==
		      SELF_SYNC_SCRAMBLER_INVALID_ARGUMENT &&
		      transform(&state, &config, input, 1, (uint8_t *)&config) ==
		      SELF_SYNC_SCRAMBLER_INVALID_ARGUMENT && state.history == 0x5a &&
		      config.history_bits == 8 && memcmp(output, saved, sizeof(output)) == 0,
		      "overlapping unpacked regions partially committed");
		union {
			struct self_sync_scrambler state;
			struct self_sync_scrambler_config config;
		} overlapping;
		memset(&overlapping, 0, sizeof(overlapping));
		overlapping.config = test_configs[2];
		struct self_sync_scrambler_config before = overlapping.config;
		CHECK(transform(&overlapping.state, &overlapping.config, input, 1, output) ==
		      SELF_SYNC_SCRAMBLER_INVALID_ARGUMENT &&
		      memcmp(&overlapping.config, &before, sizeof(before)) == 0,
		      "unpacked operation overwrote overlapping state/config");
		state.history = 0x100;
		CHECK(transform(&state, &config, input, 1, output) ==
		      SELF_SYNC_SCRAMBLER_INVALID_ARGUMENT && state.history == 0x100 &&
		      memcmp(output, saved, sizeof(output)) == 0,
		      "unpacked input accepted an invalid history");
	}
	return true;
}

static bool test_validation(void)
{
	static const struct self_sync_scrambler_config invalid[] = {
		{ .history_bits = 0, .tap_a_delay = 1, .tap_b_delay = 2 },
		{ .history_bits = 33, .tap_a_delay = 1, .tap_b_delay = 2 },
		{ .history_bits = 8, .tap_a_delay = 0, .tap_b_delay = 8 },
		{ .history_bits = 8, .tap_a_delay = 9, .tap_b_delay = 8 },
		{ .history_bits = 8, .tap_a_delay = 3, .tap_b_delay = 0 },
		{ .history_bits = 8, .tap_a_delay = 3, .tap_b_delay = 9 },
		{ .history_bits = 8, .tap_a_delay = 3, .tap_b_delay = 3 },
	};
	const struct self_sync_scrambler_config config = {
		.history_bits = 8,
		.tap_a_delay = 3,
		.tap_b_delay = 8,
	};
	struct self_sync_scrambler state = { .history = 0x5au };
	struct self_sync_scrambler saved = state;
	uint8_t output = UINT8_MAX;

	CHECK(self_sync_scrambler_config_is_valid(&config) &&
		      !self_sync_scrambler_config_is_valid(NULL) &&
		      self_sync_scrambler_is_valid(&state, &config) &&
		      !self_sync_scrambler_is_valid(NULL, &config),
	      "valid configuration or state was rejected");
	for (size_t index = 0; index < ARRAY_LENGTH(invalid); ++index) {
		CHECK(!self_sync_scrambler_config_is_valid(
			      &invalid[index]) &&
		      self_sync_scrambler_init(
			      &state, &invalid[index]) ==
			      SELF_SYNC_SCRAMBLER_INVALID_ARGUMENT &&
		      memcmp(&state, &saved, sizeof(state)) == 0,
		      "invalid configuration was accepted at %zu", index);
	}
	CHECK(self_sync_scrambler_init(NULL, &config) ==
		      SELF_SYNC_SCRAMBLER_INVALID_ARGUMENT &&
	      self_sync_scramble_group(
		      NULL, &config, 0, 1, &output) ==
		      SELF_SYNC_SCRAMBLER_INVALID_ARGUMENT &&
	      self_sync_scramble_group(
		      &state, NULL, 0, 1, &output) ==
		      SELF_SYNC_SCRAMBLER_INVALID_ARGUMENT &&
	      self_sync_scramble_group(
		      &state, &config, 0, 0, &output) ==
		      SELF_SYNC_SCRAMBLER_INVALID_ARGUMENT &&
	      self_sync_scramble_group(
		      &state, &config, 0, 9, &output) ==
		      SELF_SYNC_SCRAMBLER_INVALID_ARGUMENT &&
	      self_sync_scramble_group(
		      &state, &config, 8, 3, &output) ==
		      SELF_SYNC_SCRAMBLER_INVALID_ARGUMENT &&
	      self_sync_descramble_group(
		      &state, &config, 0, 1, NULL) ==
		      SELF_SYNC_SCRAMBLER_INVALID_ARGUMENT &&
	      memcmp(&state, &saved, sizeof(state)) == 0 &&
	      output == UINT8_MAX,
	      "invalid group operation mutated its outputs");
	state.history = UINT32_C(0x100);
	saved = state;
	CHECK(!self_sync_scrambler_is_valid(&state, &config) &&
		      self_sync_scramble_group(
			      &state, &config, 0, 1, &output) ==
			      SELF_SYNC_SCRAMBLER_INVALID_ARGUMENT &&
		      memcmp(&state, &saved, sizeof(state)) == 0,
	      "invalid history was accepted");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "impulse responses", test_impulse_responses },
		{ "normative group starts", test_normative_group_starts },
		{ "group round trip", test_group_round_trip },
		{ "descrambler resynchronization",
		  test_descrambler_resynchronization },
		{ "validation", test_validation },
		{ "unpacked streams", test_unpacked_streams },
		{ "unpacked rejection", test_unpacked_rejection },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		bool passed;

		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		passed = tests[index].run();
		if (!passed) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
