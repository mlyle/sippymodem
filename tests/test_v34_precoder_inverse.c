// SPDX-License-Identifier: BSD-3-Clause

#include <v34_precoder.h>

#include "test.h"

#include <limits.h>
#include <string.h>

static bool initialize_precoder(
	struct v34_precoder *precoder,
	enum v34_symbol_rate symbol_rate, uint8_t primary_rate,
	const struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT])
{
	struct v34_frame_geometry geometry;
	struct v34_mapping_parameters parameters;

	return v34_frame_geometry_init(
		       &geometry, symbol_rate, primary_rate, false) ==
		       V34_FRAME_SCHEDULE_OK &&
	       v34_mapping_parameters_init(
		       &parameters, &geometry, false) ==
		       V34_MAPPING_PARAMETERS_OK &&
	       v34_precoder_init(
		       precoder, &parameters, coefficients) ==
		       V34_PRECODER_OK;
}

static bool q7_points_are_equal(
	const struct v34_q7_point *left,
	const struct v34_q7_point *right)
{
	return left->in_phase == right->in_phase &&
	       left->quadrature == right->quadrature;
}

static bool grid_points_are_equal(
	const struct v34_grid_point *left,
	const struct v34_grid_point *right)
{
	return left->in_phase == right->in_phase &&
	       left->quadrature == right->quadrature;
}

static bool symbols_are_equal(
	const struct v34_precoder_symbol *left,
	const struct v34_precoder_symbol *right)
{
	return q7_points_are_equal(
		       &left->filter_output, &right->filter_output) &&
	       grid_points_are_equal(
		       &left->lattice_offset, &right->lattice_offset) &&
	       grid_points_are_equal(
		       &left->channel_output, &right->channel_output) &&
	       q7_points_are_equal(
		       &left->precoded_symbol, &right->precoded_symbol);
}

static bool precoders_are_equal(
	const struct v34_precoder *left,
	const struct v34_precoder *right)
{
	if (left->grid_step != right->grid_step)
		return false;
	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT;
	     ++tap) {
		if (left->coefficients[tap].real !=
			    right->coefficients[tap].real ||
		    left->coefficients[tap].imaginary !=
			    right->coefficients[tap].imaginary ||
		    !q7_points_are_equal(
			    &left->previous_symbols[tap],
			    &right->previous_symbols[tap]))
			return false;
	}
	return true;
}

static bool test_known_inverse(void)
{
	struct v34_precoder precoder = {
		.coefficients = { { 8192, 0 } },
		.previous_symbols = { { 512, -256 } },
		.grid_step = V34_PRECODER_GRID_STEP_2,
	};
	struct v34_grid_point channel_and_mapper = { 5, -3 };
	struct v34_precoder_symbol symbol;
	const struct v34_grid_point expected_mapper = { 3, -3 };
	const struct v34_precoder_symbol expected_symbol = {
		.filter_output = { 256, -128 },
		.lattice_offset = { 2, 0 },
		.channel_output = { 5, -3 },
		.precoded_symbol = { 384, -256 },
	};
	const struct v34_q7_point expected_history[] = {
		{ 384, -256 }, { 512, -256 }, { 0, 0 },
	};

	CHECK(v34_precoder_inverse_step(
		      &precoder, &channel_and_mapper, &channel_and_mapper,
		      &symbol) == V34_PRECODER_OK &&
	      grid_points_are_equal(&channel_and_mapper, &expected_mapper) &&
	      symbols_are_equal(&symbol, &expected_symbol),
	      "known inverse step did not recover its mapper point");
	for (size_t tap = 0; tap < ARRAY_LENGTH(expected_history); ++tap)
		CHECK(q7_points_are_equal(
			      &precoder.previous_symbols[tap],
			      &expected_history[tap]),
		      "known inverse history tap %zu changed", tap);
	return true;
}

static int16_t generated_odd_coordinate(uint32_t *state)
{
	uint32_t coordinate;

	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	coordinate = (*state >> 24) & 31u;
	return (int16_t)((int)coordinate * 2 - 31);
}

static bool test_generated_round_trip(void)
{
	static const struct v34_q14_coefficient coefficients[] = {
		{ 2048, -1024 }, { -1024, 512 }, { 512, 1024 },
	};
	static const struct {
		enum v34_symbol_rate symbol_rate;
		uint8_t primary_rate;
		enum v34_precoder_grid_step grid_step;
	} modes[] = {
		{ V34_SYMBOL_RATE_3000, 8,
		  V34_PRECODER_GRID_STEP_2 },
		{ V34_SYMBOL_RATE_2743, 8,
		  V34_PRECODER_GRID_STEP_4 },
	};
	uint32_t state = UINT32_C(0x6d2b79f5);
	unsigned int nonzero_offsets = 0;

	for (size_t mode = 0; mode < ARRAY_LENGTH(modes); ++mode) {
		struct v34_precoder transmitter;
		struct v34_precoder receiver;

		CHECK(initialize_precoder(
			      &transmitter, modes[mode].symbol_rate,
			      modes[mode].primary_rate, coefficients) &&
		      initialize_precoder(
			      &receiver, modes[mode].symbol_rate,
			      modes[mode].primary_rate, coefficients) &&
		      transmitter.grid_step == modes[mode].grid_step &&
		      receiver.grid_step == modes[mode].grid_step,
		      "generated mode %zu did not initialize", mode);
		for (size_t index = 0; index < 1024u; ++index) {
			const struct v34_grid_point mapper = {
				generated_odd_coordinate(&state),
				generated_odd_coordinate(&state),
			};
			struct v34_grid_point recovered;
			struct v34_precoder_symbol transmitted;
			struct v34_precoder_symbol received;

			CHECK(v34_precoder_step(
				      &transmitter, &mapper, &transmitted) ==
				      V34_PRECODER_OK,
			      "generated transmit %zu/%zu failed", mode, index);
			if (transmitted.lattice_offset.in_phase != 0 ||
			    transmitted.lattice_offset.quadrature != 0)
				nonzero_offsets++;
			CHECK(v34_precoder_inverse_step(
				      &receiver, &transmitted.channel_output,
				      &recovered, &received) ==
				      V34_PRECODER_OK &&
			      grid_points_are_equal(&recovered, &mapper) &&
			      symbols_are_equal(&received, &transmitted) &&
			      precoders_are_equal(&receiver, &transmitter),
			      "generated inverse %zu/%zu diverged", mode, index);
		}
	}
	CHECK(nonzero_offsets != 0,
	      "generated inverse never exercised a precoder offset");
	return true;
}

static bool rejected_inverse_is_transactional(
	struct v34_precoder *precoder,
	const struct v34_grid_point *channel,
	enum v34_precoder_result expected)
{
	struct v34_precoder saved = *precoder;
	struct v34_grid_point mapper;
	struct v34_grid_point saved_mapper;
	struct v34_precoder_symbol symbol;
	struct v34_precoder_symbol saved_symbol;

	memset(&mapper, 0x5a, sizeof(mapper));
	memset(&symbol, 0xa5, sizeof(symbol));
	saved_mapper = mapper;
	saved_symbol = symbol;
	return v34_precoder_inverse_step(
		       precoder, channel, &mapper, &symbol) == expected &&
	       precoders_are_equal(precoder, &saved) &&
	       memcmp(&mapper, &saved_mapper, sizeof(mapper)) == 0 &&
	       memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0;
}

static bool test_rejection(void)
{
	static const struct v34_grid_point invalid_channels[] = {
		{ 0, 1 }, { 1, 0 }, { 257, 1 }, { -257, 1 },
		{ 1, 257 }, { 1, -257 },
	};
	static const struct v34_q14_coefficient zero_coefficients
		[V34_PRECODER_TAP_COUNT] = { 0 };
	const struct v34_grid_point channel = { 1, 1 };
	struct v34_precoder precoder;
	struct v34_precoder saved;
	struct v34_grid_point mapper = { 7, 9 };
	struct v34_grid_point saved_mapper = mapper;
	struct v34_precoder_symbol symbol;
	struct v34_precoder_symbol saved_symbol;

	CHECK(initialize_precoder(
		      &precoder, V34_SYMBOL_RATE_3000, 8,
		      zero_coefficients),
	      "rejection fixture did not initialize");
	saved = precoder;
	memset(&symbol, 0xa5, sizeof(symbol));
	saved_symbol = symbol;
	CHECK(v34_precoder_inverse_step(
		      NULL, &channel, &mapper, &symbol) ==
		      V34_PRECODER_INVALID_ARGUMENT &&
	      v34_precoder_inverse_step(
		      &precoder, NULL, &mapper, &symbol) ==
		      V34_PRECODER_INVALID_ARGUMENT &&
	      v34_precoder_inverse_step(
		      &precoder, &channel, NULL, &symbol) ==
		      V34_PRECODER_INVALID_ARGUMENT &&
	      v34_precoder_inverse_step(
		      &precoder, &channel, &mapper, NULL) ==
		      V34_PRECODER_INVALID_ARGUMENT &&
	      precoders_are_equal(&precoder, &saved) &&
	      memcmp(&mapper, &saved_mapper, sizeof(mapper)) == 0 &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0,
	      "invalid inverse arguments changed state or output");
	for (size_t index = 0; index < ARRAY_LENGTH(invalid_channels); ++index)
		CHECK(rejected_inverse_is_transactional(
			      &precoder, &invalid_channels[index],
			      V34_PRECODER_INVALID_ARGUMENT),
		      "invalid channel point %zu changed state or output", index);
	precoder.grid_step = (enum v34_precoder_grid_step)3;
	CHECK(rejected_inverse_is_transactional(
		      &precoder, &channel,
		      V34_PRECODER_INVALID_ARGUMENT),
	      "invalid precoder state was accepted");

	precoder = (struct v34_precoder){
		.coefficients = { { V34_Q14_SCALE, 0 } },
		.previous_symbols = {
			{ -32768 * V34_Q7_SCALE, 0 },
		},
		.grid_step = V34_PRECODER_GRID_STEP_2,
	};
	CHECK(rejected_inverse_is_transactional(
		      &precoder,
		      &(const struct v34_grid_point){ 255, 1 },
		      V34_PRECODER_OUT_OF_RANGE),
	      "recovered mapper overflow changed state or output");
	precoder = (struct v34_precoder){
		.coefficients = {
			{ INT16_MAX, 0 }, { INT16_MAX, 0 }, { INT16_MAX, 0 },
		},
		.previous_symbols = {
			{ INT32_MAX, 0 }, { INT32_MAX, 0 }, { INT32_MAX, 0 },
		},
		.grid_step = V34_PRECODER_GRID_STEP_2,
	};
	CHECK(rejected_inverse_is_transactional(
		      &precoder, &channel,
		      V34_PRECODER_OUT_OF_RANGE),
	      "inverse prediction overflow changed state or output");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "known inverse", test_known_inverse },
		{ "generated round trip", test_generated_round_trip },
		{ "transactional rejection", test_rejection },
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
