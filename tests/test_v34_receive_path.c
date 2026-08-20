// SPDX-License-Identifier: BSD-3-Clause

#include <v34_receive_path.h>

#include "test.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct rate_profile {
	enum v34_symbol_rate rate;
	uint32_t symbol_numerator;
	uint32_t symbol_denominator;
	uint32_t carrier_numerator;
	uint32_t carrier_denominator;
};

static const struct rate_profile RATE_PROFILES[] = {
	{ V34_SYMBOL_RATE_2400, 2400, 1, 1800, 1 },
	{ V34_SYMBOL_RATE_2743, 19200, 7, 12800, 7 },
	{ V34_SYMBOL_RATE_2800, 2800, 1, 5600, 3 },
	{ V34_SYMBOL_RATE_3000, 3000, 1, 2000, 1 },
	{ V34_SYMBOL_RATE_3200, 3200, 1, 1920, 1 },
	{ V34_SYMBOL_RATE_3429, 24000, 7, 96000, 49 },
};

static struct v34_phase3_profile profile(
	const struct rate_profile *rate, enum v34_station_role transmitting_role)
{
	return (struct v34_phase3_profile){
		.symbol_rate_numerator = rate->symbol_numerator,
		.symbol_rate_denominator = rate->symbol_denominator,
		.carrier_frequency_numerator = rate->carrier_numerator,
		.carrier_frequency_denominator = rate->carrier_denominator,
		.symbol_rate = rate->rate,
		.local_role = transmitting_role,
		.preemphasis_filter_index = 0,
		.projected_data_rate_2400_bps_units = 1,
		.high_carrier = true,
	};
}

static uint64_t hash_u8(uint64_t hash, uint8_t value)
{
	return (hash ^ value) * UINT64_C(1099511628211);
}

static uint64_t hash_u16(uint64_t hash, uint16_t value)
{
	hash = hash_u8(hash, (uint8_t)value);
	return hash_u8(hash, (uint8_t)(value >> 8));
}

static uint64_t hash_u32(uint64_t hash, uint32_t value)
{
	hash = hash_u16(hash, (uint16_t)value);
	return hash_u16(hash, (uint16_t)(value >> 16));
}

static uint32_t next_word(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static bool test_profiles_and_cadence(void)
{
	static const uint32_t expected_symbols[2][6] = {
		{ 3600, 4114, 4200, 4500, 4800, 5143 },
		{ 3600, 4114, 4199, 4500, 4799, 5143 },
	};
	uint64_t output_hash = UINT64_C(1469598103934665603);
	uint64_t state_hash = UINT64_C(1469598103934665603);
	uint32_t random = UINT32_C(0x71a49c3d);

	for (unsigned int role = 0; role < 2; ++role) {
		for (size_t rate_index = 0;
		     rate_index < ARRAY_LENGTH(RATE_PROFILES); ++rate_index) {
			const struct rate_profile *rate = &RATE_PROFILES[rate_index];
			struct v34_phase3_profile selected = profile(
				rate, (enum v34_station_role)role);
			struct v34_receive_path path;
			struct v34_receive_path twin;
			uint32_t symbols = 0;

			CHECK(v34_receive_path_init(&path, &selected) ==
				      V34_RECEIVE_PATH_OK &&
			      v34_receive_path_is_valid(&path),
			      "receive path did not initialize at %u/%zu",
			      role, rate_index);
			CHECK(path.timing.local_role ==
				      (role == V34_CALLING_STATION ?
					       V34_ANSWERING_STATION :
					       V34_CALLING_STATION) &&
			      path.gain.gain ==
				      (int16_t)(path.timing.phase_limit / 2u),
			      "receive role or initial gain changed at %u/%zu",
			      role, rate_index);
			twin = path;
			for (unsigned int sample_index = 0;
			     sample_index < 12000; ++sample_index) {
				struct v34_analytic_sample left = { 123, -456 };
				struct v34_analytic_sample right = left;
				bool left_ready = true;
				bool right_ready = true;
				int16_t input = (int16_t)(next_word(&random) >> 16);
				enum v34_receive_path_result left_result =
					v34_receive_path_sample(
						&path, input, &left, &left_ready);
				enum v34_receive_path_result right_result =
					v34_receive_path_sample(
						&twin, input, &right, &right_ready);

				CHECK((left_result == V34_RECEIVE_PATH_OK ||
				       left_result == V34_RECEIVE_PATH_SYMBOL_READY) &&
				      left_result == right_result &&
				      left_ready == right_ready &&
				      memcmp(&left, &right, sizeof(left)) == 0 &&
				      memcmp(&path, &twin, sizeof(path)) == 0,
				      "receive path diverged at %u/%zu/%u",
				      role, rate_index, sample_index);
				if (!left_ready) {
					CHECK(left.in_phase == 123 &&
					      left.quadrature == -456,
					      "non-symbol call changed output at %u/%zu/%u",
					      role, rate_index, sample_index);
					continue;
				}
				CHECK(left_result == V34_RECEIVE_PATH_SYMBOL_READY,
				      "ready flag did not match result at %u/%zu/%u",
				      role, rate_index, sample_index);
				symbols++;
				output_hash = hash_u16(
					output_hash, (uint16_t)left.in_phase);
				output_hash = hash_u16(
					output_hash, (uint16_t)left.quadrature);
			}
			CHECK(symbols == expected_symbols[role][rate_index] &&
			      path.input_count < V34_RECEIVE_PATH_INPUT_CAPACITY &&
			      path.input_advance_pending <= 2u,
			      "receive cadence escaped bounds at %u/%zu: %u",
			      role, rate_index, symbols);
			state_hash = hash_u32(state_hash, symbols);
			state_hash = hash_u16(state_hash, path.timing.phase);
			state_hash = hash_u16(
				state_hash, path.timing.phase_increment);
			state_hash = hash_u32(state_hash, path.carrier.phase);
			state_hash = hash_u32(state_hash, path.equalizer.index);
			state_hash = hash_u8(state_hash, path.input_count);
			state_hash = hash_u8(
				state_hash, path.input_advance_pending);
		}
	}
	CHECK(random == UINT32_C(0xd959aabd),
	      "receive fixture random lineage changed: %#x", random);
	CHECK(output_hash == UINT64_C(0xd730818ab43035c1) &&
	      state_hash == UINT64_C(0x93c4cd8cc256f904),
	      "receive path oracle changed: %#llx/%#llx",
	      (unsigned long long)output_hash,
	      (unsigned long long)state_hash);
	return true;
}

static bool test_transactionality(void)
{
	struct v34_phase3_profile selected =
		profile(&RATE_PROFILES[3], V34_CALLING_STATION);
	struct v34_receive_path path;
	struct v34_receive_path saved;
	struct v34_receive_path invalid = { 0 };
	struct v34_analytic_sample output = { 91, -37 };
	struct v34_analytic_sample saved_output = output;
	bool ready = true;
	bool saved_ready = ready;

	CHECK(v34_receive_path_init(&path, &selected) ==
		      V34_RECEIVE_PATH_OK,
	      "transaction path did not initialize");
	saved = path;
	CHECK(v34_receive_path_init(NULL, &selected) ==
		      V34_RECEIVE_PATH_INVALID_ARGUMENT &&
	      v34_receive_path_init(&invalid, NULL) ==
		      V34_RECEIVE_PATH_INVALID_ARGUMENT &&
	      v34_receive_path_sample(NULL, 0, &output, &ready) ==
		      V34_RECEIVE_PATH_INVALID_STATE &&
	      v34_receive_path_sample(&path, 0, NULL, &ready) ==
		      V34_RECEIVE_PATH_INVALID_ARGUMENT &&
	      v34_receive_path_sample(&path, 0, &output, NULL) ==
		      V34_RECEIVE_PATH_INVALID_ARGUMENT &&
	      v34_receive_path_sample(
		      &path, 0, (struct v34_analytic_sample *)&path,
		      &ready) == V34_RECEIVE_PATH_INVALID_ARGUMENT &&
	      v34_receive_path_sample(
		      &path, 0, &output, (bool *)&output) ==
		      V34_RECEIVE_PATH_INVALID_ARGUMENT &&
	      memcmp(&path, &saved, sizeof(path)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0 &&
	      ready == saved_ready,
	      "receive argument rejection changed state or output");

	path.input_count = V34_RECEIVE_PATH_INPUT_CAPACITY;
	saved = path;
	CHECK(v34_receive_path_is_valid(&path) &&
	      v34_receive_path_sample(&path, 0, &output, &ready) ==
		      V34_RECEIVE_PATH_INVALID_STATE &&
	      memcmp(&path, &saved, sizeof(path)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0 &&
	      ready == saved_ready,
	      "receive queue saturation was not transactional");

	path = saved;
	path.input_count = 0;
	path.input_read = V34_RECEIVE_PATH_INPUT_CAPACITY;
	saved = path;
	CHECK(!v34_receive_path_is_valid(&path) &&
	      v34_receive_path_sample(&path, 0, &output, &ready) ==
		      V34_RECEIVE_PATH_INVALID_STATE &&
	      memcmp(&path, &saved, sizeof(path)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0 &&
	      ready == saved_ready,
	      "receive invalid-state rejection changed state or output");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "profiles and cadence", test_profiles_and_cadence },
		{ "transactionality", test_transactionality },
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
