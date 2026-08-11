// SPDX-License-Identifier: BSD-3-Clause

#include <v34_precoder.h>

#include "test.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static const struct v34_q14_coefficient ZERO_COEFFICIENTS
	[V34_PRECODER_TAP_COUNT];

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

static bool prediction_matches_symbol(
	const struct v34_precoder_prediction *prediction,
	const struct v34_precoder_symbol *symbol)
{
	return q7_points_are_equal(
		       &prediction->filter_output, &symbol->filter_output) &&
	       grid_points_are_equal(
		       &prediction->lattice_offset, &symbol->lattice_offset);
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

static int64_t model_round_ties_toward_zero(
	int64_t value, int64_t divisor)
{
	int64_t quotient = value / divisor;
	int64_t remainder = value % divisor;
	int64_t half = divisor / 2;

	if (remainder > half)
		return quotient + 1;
	if (remainder < -half)
		return quotient - 1;
	return quotient;
}

static void model_step(
	struct v34_precoder *precoder,
	const struct v34_grid_point *mapper_output,
	struct v34_precoder_symbol *symbol)
{
	int64_t filter_i = 0;
	int64_t filter_q = 0;
	int64_t quantizer_divisor =
		(int64_t)precoder->grid_step * V34_Q7_SCALE;

	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT;
	     ++tap) {
		filter_i +=
			(int64_t)precoder->previous_symbols[tap].in_phase *
				precoder->coefficients[tap].real -
			(int64_t)precoder->previous_symbols[tap].quadrature *
				precoder->coefficients[tap].imaginary;
		filter_q +=
			(int64_t)precoder->previous_symbols[tap].in_phase *
				precoder->coefficients[tap].imaginary +
			(int64_t)precoder->previous_symbols[tap].quadrature *
				precoder->coefficients[tap].real;
	}
	filter_i = model_round_ties_toward_zero(
		filter_i, V34_Q14_SCALE);
	filter_q = model_round_ties_toward_zero(
		filter_q, V34_Q14_SCALE);
	symbol->filter_output = (struct v34_q7_point){
		(int32_t)filter_i,
		(int32_t)filter_q,
	};
	symbol->lattice_offset = (struct v34_grid_point){
		(int16_t)(model_round_ties_toward_zero(
				  filter_i, quantizer_divisor) *
			  precoder->grid_step),
		(int16_t)(model_round_ties_toward_zero(
				  filter_q, quantizer_divisor) *
			  precoder->grid_step),
	};
	symbol->channel_output = (struct v34_grid_point){
		(int16_t)(mapper_output->in_phase +
			  symbol->lattice_offset.in_phase),
		(int16_t)(mapper_output->quadrature +
			  symbol->lattice_offset.quadrature),
	};
	symbol->precoded_symbol = (struct v34_q7_point){
		(int32_t)symbol->channel_output.in_phase *
				V34_Q7_SCALE -
			(int32_t)filter_i,
		(int32_t)symbol->channel_output.quadrature *
				V34_Q7_SCALE -
			(int32_t)filter_q,
	};
	for (size_t tap = V34_PRECODER_TAP_COUNT - 1; tap > 0;
	     --tap)
		precoder->previous_symbols[tap] =
			precoder->previous_symbols[tap - 1];
	precoder->previous_symbols[0] = symbol->precoded_symbol;
}

static bool test_initialization(void)
{
	static const struct v34_q14_coefficient coefficients[] = {
		{ 8192, -4096 },
		{ -2048, 6144 },
		{ 1024, 3072 },
	};
	struct v34_precoder low;
	struct v34_precoder high;

	CHECK(initialize_precoder(
		      &low, V34_SYMBOL_RATE_3000, 8,
		      coefficients) &&
	      initialize_precoder(
		      &high, V34_SYMBOL_RATE_2743, 8,
		      coefficients),
	      "precoder fixtures did not initialize");
	CHECK(low.grid_step == V34_PRECODER_GRID_STEP_2 &&
	      high.grid_step == V34_PRECODER_GRID_STEP_4,
	      "mapping-frame boundary selected the wrong grid steps");
	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT;
	     ++tap) {
		const struct v34_q7_point zero = { 0 };

		CHECK(low.coefficients[tap].real == coefficients[tap].real &&
		      low.coefficients[tap].imaginary ==
			      coefficients[tap].imaginary &&
		      q7_points_are_equal(&low.previous_symbols[tap], &zero) &&
		      q7_points_are_equal(&high.previous_symbols[tap], &zero),
		      "tap %zu was not copied with zero history", tap);
	}
	CHECK(v34_precoder_is_valid(&low) &&
	      v34_precoder_is_valid(&high),
	      "initialized precoder was invalid");
	return true;
}

static bool test_complex_sequences(void)
{
	static const struct v34_q14_coefficient coefficients[] = {
		{ 8192, -4096 },
		{ -2048, 6144 },
		{ 1024, 3072 },
	};
	static const struct v34_grid_point mapper_outputs[] = {
		{ 1, 1 },   { 3, -1 },   { -5, 7 },  { 9, -11 },
		{ -13, -15 }, { 17, 19 },  { -21, 23 }, { 25, -27 },
		{ -29, -31 }, { 33, 35 },  { -37, 39 }, { 41, -43 },
		{ -45, 1 }, { 31, -17 }, { -9, -23 }, { 7, 27 },
	};
	static const struct {
		enum v34_symbol_rate symbol_rate;
		uint8_t primary_rate;
	} modes[] = {
		{ V34_SYMBOL_RATE_3000, 8 },
		{ V34_SYMBOL_RATE_2743, 8 },
	};

	for (size_t mode = 0; mode < ARRAY_LENGTH(modes); ++mode) {
		struct v34_precoder actual;
		struct v34_precoder model;

		CHECK(initialize_precoder(
			      &actual, modes[mode].symbol_rate,
			      modes[mode].primary_rate, coefficients),
		      "complex mode %zu did not initialize", mode);
		model = actual;
		for (size_t index = 0; index < ARRAY_LENGTH(mapper_outputs);
		     ++index) {
			struct v34_precoder before_prediction = actual;
			struct v34_precoder_symbol expected;
			struct v34_precoder_symbol generated;
			struct v34_precoder_prediction prediction;

			model_step(&model, &mapper_outputs[index], &expected);
			CHECK(v34_precoder_predict(
				      &actual, &prediction) ==
				      V34_PRECODER_OK &&
			      prediction_matches_symbol(&prediction, &expected) &&
			      precoders_are_equal(&actual, &before_prediction),
			      "complex mode %zu prediction %zu differed from model",
			      mode, index);
			CHECK(v34_precoder_step(
				      &actual, &mapper_outputs[index], &generated) ==
				      V34_PRECODER_OK,
			      "complex mode %zu step %zu failed", mode, index);
			CHECK(symbols_are_equal(&generated, &expected) &&
			      precoders_are_equal(&actual, &model),
			      "complex mode %zu step %zu differed from model",
			      mode, index);
		}
	}
	return true;
}

static uint32_t next_generated_word(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static int16_t generated_odd_coordinate(uint32_t *state)
{
	return (int16_t)(2 * (int)(next_generated_word(state) & 31u) - 31);
}

static bool test_receive_reconstruction(void)
{
	static const struct v34_q14_coefficient coefficients[] = {
		{ 8192, -4096 },
		{ -2048, 6144 },
		{ 1024, 3072 },
	};
	static const struct {
		enum v34_symbol_rate symbol_rate;
		uint8_t primary_rate;
	} modes[] = {
		{ V34_SYMBOL_RATE_3000, 8 },
		{ V34_SYMBOL_RATE_2743, 8 },
	};

	for (size_t mode = 0; mode < ARRAY_LENGTH(modes); ++mode) {
		struct v34_precoder transmitter;
		struct v34_precoder receiver;
		uint32_t random = UINT32_C(0x34c0de17) ^ (uint32_t)mode;

		CHECK(initialize_precoder(
			      &transmitter, modes[mode].symbol_rate,
			      modes[mode].primary_rate, coefficients) &&
		      initialize_precoder(
			      &receiver, modes[mode].symbol_rate,
			      modes[mode].primary_rate, coefficients),
		      "receive mode %zu did not initialize", mode);
		for (size_t interval = 0; interval < 256; ++interval) {
			struct sippymodem_qam_point precoded[2];
			struct sippymodem_qam_point reconstructed[2];
			struct v34_grid_point expected[2];

			for (size_t symbol = 0; symbol < ARRAY_LENGTH(precoded);
			     ++symbol) {
				struct v34_grid_point mapper = {
					.in_phase = generated_odd_coordinate(&random),
					.quadrature = generated_odd_coordinate(&random),
				};
				struct v34_precoder_symbol encoded;

				CHECK(v34_precoder_step(
					      &transmitter, &mapper, &encoded) ==
					      V34_PRECODER_OK,
				      "receive mode %zu interval %zu symbol %zu did not encode",
				      mode, interval, symbol);
				precoded[symbol] = (struct sippymodem_qam_point){
					.in_phase =
						(double)encoded.precoded_symbol.in_phase /
						V34_Q7_SCALE,
					.quadrature =
						(double)encoded.precoded_symbol.quadrature /
						V34_Q7_SCALE,
				};
				expected[symbol] = encoded.channel_output;
			}
			if ((interval & 1u) != 0u) {
				reconstructed[0] = precoded[0];
				reconstructed[1] = precoded[1];
				CHECK(v34_precoder_reconstruct_interval(
					      &receiver, reconstructed,
					      reconstructed) ==
					      V34_PRECODER_OK,
				      "receive mode %zu in-place interval %zu failed",
				      mode, interval);
			} else {
				CHECK(v34_precoder_reconstruct_interval(
					      &receiver, precoded, reconstructed) ==
					      V34_PRECODER_OK,
				      "receive mode %zu interval %zu failed", mode,
				      interval);
			}
			for (size_t symbol = 0; symbol < ARRAY_LENGTH(reconstructed);
			     ++symbol) {
				CHECK(reconstructed[symbol].in_phase ==
					      expected[symbol].in_phase &&
				      reconstructed[symbol].quadrature ==
					      expected[symbol].quadrature,
				      "receive mode %zu interval %zu symbol %zu reconstructed %.17g/%.17g instead of %d/%d",
				      mode, interval, symbol,
				      reconstructed[symbol].in_phase,
				      reconstructed[symbol].quadrature,
				      expected[symbol].in_phase,
				      expected[symbol].quadrature);
			}
			CHECK(precoders_are_equal(&receiver, &transmitter),
			      "receive mode %zu interval %zu history diverged",
			      mode, interval);
		}
	}
	return true;
}

static bool test_receive_reconstruction_boundaries(void)
{
	const struct sippymodem_qam_point tie_inputs[2] = {
		{ 1.5 / V34_Q7_SCALE,
		  -1.5 / V34_Q7_SCALE },
		{ 2.5 / V34_Q7_SCALE,
		  -2.5 / V34_Q7_SCALE },
	};
	struct v34_precoder receiver;
	struct v34_precoder saved;
	struct v34_precoder invalid;
	struct v34_precoder overflow = {
		.coefficients = {
			{ INT16_MAX, 0 }, { INT16_MAX, 0 }, { INT16_MAX, 0 },
		},
		.previous_symbols = {
			{ INT32_MAX, 0 }, { INT32_MAX, 0 }, { INT32_MAX, 0 },
		},
		.grid_step = V34_PRECODER_GRID_STEP_2,
	};
	struct sippymodem_qam_point rejected[2];
	struct sippymodem_qam_point saved_rejected[2];
	struct sippymodem_qam_point bad[2];

	CHECK(initialize_precoder(
		      &receiver, V34_SYMBOL_RATE_3000, 8,
		      ZERO_COEFFICIENTS),
	      "receive boundary fixture did not initialize");
	CHECK(v34_precoder_reconstruct_interval(
		      &receiver, tie_inputs, rejected) ==
		      V34_PRECODER_OK &&
	      memcmp(rejected, tie_inputs, sizeof(rejected)) == 0 &&
	      receiver.previous_symbols[0].in_phase == 2 &&
	      receiver.previous_symbols[0].quadrature == -2 &&
	      receiver.previous_symbols[1].in_phase == 1 &&
	      receiver.previous_symbols[1].quadrature == -1,
	      "receive Q7 half-way rounding changed");
	saved = receiver;
	memset(rejected, 0xa5, sizeof(rejected));
	memcpy(saved_rejected, rejected, sizeof(rejected));
	bad[0] = tie_inputs[0];
	bad[1] = tie_inputs[1];
	bad[1].quadrature = NAN;
	CHECK(v34_precoder_reconstruct_interval(
		      &receiver, bad, rejected) ==
		      V34_PRECODER_INVALID_ARGUMENT &&
	      precoders_are_equal(&receiver, &saved) &&
	      memcmp(rejected, saved_rejected, sizeof(rejected)) == 0,
	      "invalid second receive symbol was not atomic");
	bad[1] = tie_inputs[1];
	bad[1].in_phase = DBL_MAX;
	CHECK(v34_precoder_reconstruct_interval(
		      &receiver, bad, rejected) ==
		      V34_PRECODER_OUT_OF_RANGE &&
	      precoders_are_equal(&receiver, &saved) &&
	      memcmp(rejected, saved_rejected, sizeof(rejected)) == 0,
	      "out-of-range second receive symbol was not atomic");
	CHECK(v34_precoder_reconstruct_interval(
		      NULL, tie_inputs, rejected) ==
		      V34_PRECODER_INVALID_ARGUMENT &&
	      v34_precoder_reconstruct_interval(
		      &receiver, NULL, rejected) ==
		      V34_PRECODER_INVALID_ARGUMENT &&
	      v34_precoder_reconstruct_interval(
		      &receiver, tie_inputs, NULL) ==
		      V34_PRECODER_INVALID_ARGUMENT &&
	      precoders_are_equal(&receiver, &saved) &&
	      memcmp(rejected, saved_rejected, sizeof(rejected)) == 0,
	      "invalid receive pointers changed state or output");
	invalid = receiver;
	invalid.grid_step = (enum v34_precoder_grid_step)3;
	CHECK(v34_precoder_reconstruct_interval(
		      &invalid, tie_inputs, rejected) ==
		      V34_PRECODER_INVALID_ARGUMENT &&
	      memcmp(rejected, saved_rejected, sizeof(rejected)) == 0,
	      "invalid receive history was accepted");
	CHECK(v34_precoder_is_valid(&overflow),
	      "receive overflow fixture is invalid");
	saved = overflow;
	CHECK(v34_precoder_reconstruct_interval(
		      &overflow, tie_inputs, rejected) ==
		      V34_PRECODER_OUT_OF_RANGE &&
	      precoders_are_equal(&overflow, &saved) &&
	      memcmp(rejected, saved_rejected, sizeof(rejected)) == 0,
	      "receive predictor overflow changed state or output");
	return true;
}

static bool test_rounding_boundaries(void)
{
	static const struct {
		int16_t coefficient;
		int32_t history;
		int32_t expected;
	} filter_cases[] = {
		{ 8191, 1, 0 },  { 8192, 1, 0 },  { 8193, 1, 1 },
		{ -8191, 1, 0 }, { -8192, 1, 0 }, { -8193, 1, -1 },
	};
	static const struct {
		enum v34_precoder_grid_step grid_step;
		int32_t filter;
		int16_t expected_offset;
	} quantizer_cases[] = {
		{ V34_PRECODER_GRID_STEP_2, 127, 0 },
		{ V34_PRECODER_GRID_STEP_2, 128, 0 },
		{ V34_PRECODER_GRID_STEP_2, 129, 2 },
		{ V34_PRECODER_GRID_STEP_2, -127, 0 },
		{ V34_PRECODER_GRID_STEP_2, -128, 0 },
		{ V34_PRECODER_GRID_STEP_2, -129, -2 },
		{ V34_PRECODER_GRID_STEP_4, 255, 0 },
		{ V34_PRECODER_GRID_STEP_4, 256, 0 },
		{ V34_PRECODER_GRID_STEP_4, 257, 4 },
		{ V34_PRECODER_GRID_STEP_4, -255, 0 },
		{ V34_PRECODER_GRID_STEP_4, -256, 0 },
		{ V34_PRECODER_GRID_STEP_4, -257, -4 },
	};
	const struct v34_grid_point mapper_output = { 1, 1 };

	for (size_t index = 0; index < ARRAY_LENGTH(filter_cases); ++index) {
		struct v34_precoder precoder = {
			.coefficients = { { filter_cases[index].coefficient, 0 } },
			.previous_symbols = {
				{ filter_cases[index].history,
				  filter_cases[index].history },
			},
			.grid_step = V34_PRECODER_GRID_STEP_2,
		};
		struct v34_precoder_symbol symbol;

		CHECK(v34_precoder_step(
			      &precoder, &mapper_output, &symbol) ==
			      V34_PRECODER_OK &&
		      symbol.filter_output.in_phase ==
			      filter_cases[index].expected &&
		      symbol.filter_output.quadrature ==
			      filter_cases[index].expected,
		      "filter rounding case %zu changed", index);
	}
	for (size_t index = 0; index < ARRAY_LENGTH(quantizer_cases);
	     ++index) {
		struct v34_precoder precoder = {
			.coefficients = { { V34_Q14_SCALE, 0 } },
			.previous_symbols = {
				{ quantizer_cases[index].filter, 0 },
			},
			.grid_step = quantizer_cases[index].grid_step,
		};
		struct v34_precoder_symbol symbol;

		CHECK(v34_precoder_step(
			      &precoder, &mapper_output, &symbol) ==
			      V34_PRECODER_OK &&
		      symbol.filter_output.in_phase ==
			      quantizer_cases[index].filter &&
		      symbol.lattice_offset.in_phase ==
			      quantizer_cases[index].expected_offset,
		      "quantizer rounding case %zu changed", index);
	}
	return true;
}

static bool test_channel_boundaries(void)
{
	const struct v34_grid_point mapper_output = { 1, -1 };
	struct v34_precoder precoder = {
		.coefficients = { { V34_Q14_SCALE, 0 } },
		.previous_symbols = { { 254 * V34_Q7_SCALE,
					 -254 * V34_Q7_SCALE } },
		.grid_step = V34_PRECODER_GRID_STEP_2,
	};
	struct v34_precoder_symbol symbol;

	CHECK(v34_precoder_step(
		      &precoder, &mapper_output, &symbol) ==
		      V34_PRECODER_OK &&
	      symbol.channel_output.in_phase == 255 &&
	      symbol.channel_output.quadrature == -255 &&
	      symbol.precoded_symbol.in_phase == V34_Q7_SCALE &&
	      symbol.precoded_symbol.quadrature == -V34_Q7_SCALE,
	      "channel coordinate limits changed");
	return true;
}

static bool rejected_step_is_transactional(
	struct v34_precoder *precoder,
	const struct v34_grid_point *mapper_output,
	enum v34_precoder_result expected_result)
{
	struct v34_precoder saved = *precoder;
	struct v34_precoder_symbol symbol;
	struct v34_precoder_symbol saved_symbol;

	memset(&symbol, 0xa5, sizeof(symbol));
	saved_symbol = symbol;
	return v34_precoder_step(
		       precoder, mapper_output, &symbol) == expected_result &&
	       precoders_are_equal(precoder, &saved) &&
	       memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0;
}

static bool test_range_failures(void)
{
	const struct v34_grid_point mapper_output = { 1, 1 };
	struct v34_precoder channel_overflow = {
		.coefficients = { { V34_Q14_SCALE, 0 } },
		.previous_symbols = { { 256 * V34_Q7_SCALE, 0 } },
		.grid_step = V34_PRECODER_GRID_STEP_2,
	};
	struct v34_precoder offset_overflow = {
		.coefficients = { { V34_Q14_SCALE, 0 } },
		.previous_symbols = { { 32768 * V34_Q7_SCALE, 0 } },
		.grid_step = V34_PRECODER_GRID_STEP_2,
	};
	struct v34_precoder filter_overflow = {
		.coefficients = {
			{ INT16_MAX, 0 }, { INT16_MAX, 0 }, { INT16_MAX, 0 },
		},
		.previous_symbols = {
			{ INT32_MAX, 0 }, { INT32_MAX, 0 }, { INT32_MAX, 0 },
		},
		.grid_step = V34_PRECODER_GRID_STEP_2,
	};
	struct v34_precoder_prediction prediction;
	struct v34_precoder_prediction saved_prediction;

	memset(&prediction, 0xa5, sizeof(prediction));
	saved_prediction = prediction;
	CHECK(v34_precoder_predict(
		      &offset_overflow, &prediction) ==
		      V34_PRECODER_OUT_OF_RANGE &&
	      memcmp(&prediction, &saved_prediction, sizeof(prediction)) == 0 &&
	      v34_precoder_predict(
		      &filter_overflow, &prediction) ==
		      V34_PRECODER_OUT_OF_RANGE &&
	      memcmp(&prediction, &saved_prediction, sizeof(prediction)) == 0,
	      "failed prediction changed its destination");

	CHECK(rejected_step_is_transactional(
		      &channel_overflow, &mapper_output,
		      V34_PRECODER_OUT_OF_RANGE),
	      "channel overflow changed state or output");
	CHECK(rejected_step_is_transactional(
		      &offset_overflow, &mapper_output,
		      V34_PRECODER_OUT_OF_RANGE),
	      "offset overflow changed state or output");
	CHECK(rejected_step_is_transactional(
		      &filter_overflow, &mapper_output,
		      V34_PRECODER_OUT_OF_RANGE),
	      "filter overflow changed state or output");
	return true;
}

static bool test_validation(void)
{
	struct v34_frame_geometry geometry;
	struct v34_mapping_parameters parameters;
	struct v34_precoder precoder;
	struct v34_precoder saved;
	struct v34_q14_coefficient invalid_coefficients
		[V34_PRECODER_TAP_COUNT] = { 0 };
	struct v34_grid_point mapper_output = { 1, 1 };
	struct v34_precoder_symbol symbol;
	struct v34_precoder_symbol saved_symbol;
	struct v34_precoder_prediction prediction;
	struct v34_precoder_prediction saved_prediction;

	CHECK(v34_frame_geometry_init(
		      &geometry, V34_SYMBOL_RATE_3000, 8, false) ==
		      V34_FRAME_SCHEDULE_OK &&
	      v34_mapping_parameters_init(
		      &parameters, &geometry, false) ==
		      V34_MAPPING_PARAMETERS_OK &&
	      initialize_precoder(
		      &precoder, V34_SYMBOL_RATE_3000, 8,
		      ZERO_COEFFICIENTS),
	      "validation fixture did not initialize");
	saved = precoder;
	parameters.mapping_frame_bits++;
	CHECK(v34_precoder_init(
		      NULL, &parameters, ZERO_COEFFICIENTS) ==
		      V34_PRECODER_INVALID_ARGUMENT &&
	      v34_precoder_init(
		      &precoder, NULL, ZERO_COEFFICIENTS) ==
		      V34_PRECODER_INVALID_ARGUMENT &&
	      v34_precoder_init(
		      &precoder, &parameters, ZERO_COEFFICIENTS) ==
		      V34_PRECODER_INVALID_ARGUMENT &&
	      v34_precoder_init(&precoder, &parameters, NULL) ==
		      V34_PRECODER_INVALID_ARGUMENT &&
	      precoders_are_equal(&precoder, &saved),
	      "invalid initialization changed its destination");
	parameters.mapping_frame_bits--;
	invalid_coefficients[1].real = INT16_MIN;
	CHECK(v34_precoder_init(
		      &precoder, &parameters, invalid_coefficients) ==
		      V34_PRECODER_INVALID_ARGUMENT &&
	      precoders_are_equal(&precoder, &saved),
	      "invalid real coefficient changed the precoder");
	invalid_coefficients[1].real = 0;
	invalid_coefficients[1].imaginary = INT16_MIN;
	CHECK(v34_precoder_init(
		      &precoder, &parameters, invalid_coefficients) ==
		      V34_PRECODER_INVALID_ARGUMENT &&
	      precoders_are_equal(&precoder, &saved),
	      "invalid imaginary coefficient changed the precoder");
	memset(&prediction, 0xa5, sizeof(prediction));
	saved_prediction = prediction;
	CHECK(v34_precoder_predict(NULL, &prediction) ==
		      V34_PRECODER_INVALID_ARGUMENT &&
	      v34_precoder_predict(&precoder, NULL) ==
		      V34_PRECODER_INVALID_ARGUMENT &&
	      memcmp(&prediction, &saved_prediction, sizeof(prediction)) == 0,
	      "invalid prediction changed its destination");

	memset(&symbol, 0x5a, sizeof(symbol));
	saved_symbol = symbol;
	mapper_output.in_phase = 0;
	CHECK(v34_precoder_step(
		      &precoder, &mapper_output, &symbol) ==
		      V34_PRECODER_INVALID_ARGUMENT &&
	      precoders_are_equal(&precoder, &saved) &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0,
	      "even mapper coordinate changed state or output");
	mapper_output = (struct v34_grid_point){ 1, 0 };
	CHECK(v34_precoder_step(
		      &precoder, &mapper_output, &symbol) ==
		      V34_PRECODER_INVALID_ARGUMENT &&
	      v34_precoder_step(NULL, &mapper_output, &symbol) ==
		      V34_PRECODER_INVALID_ARGUMENT &&
	      v34_precoder_step(&precoder, NULL, &symbol) ==
		      V34_PRECODER_INVALID_ARGUMENT &&
	      v34_precoder_step(
		      &precoder, &mapper_output, NULL) ==
		      V34_PRECODER_INVALID_ARGUMENT &&
	      precoders_are_equal(&precoder, &saved) &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0,
	      "invalid step changed state or output");
	precoder.grid_step = (enum v34_precoder_grid_step)3;
	CHECK(!v34_precoder_is_valid(NULL) &&
	      !v34_precoder_is_valid(&precoder) &&
	      v34_precoder_predict(&precoder, &prediction) ==
		      V34_PRECODER_INVALID_ARGUMENT &&
	      memcmp(&prediction, &saved_prediction, sizeof(prediction)) == 0 &&
	      v34_precoder_step(
		      &precoder, &mapper_output, &symbol) ==
		      V34_PRECODER_INVALID_ARGUMENT,
	      "invalid grid step was accepted");
	precoder = saved;
	precoder.coefficients[0].imaginary = INT16_MIN;
	CHECK(!v34_precoder_is_valid(&precoder),
	      "out-of-range coefficient was accepted");
	CHECK(strcmp(v34_precoder_result_name(
		     V34_PRECODER_OK),
		     "ok") == 0 &&
	      strcmp(v34_precoder_result_name(
		     V34_PRECODER_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(v34_precoder_result_name(
		     V34_PRECODER_OUT_OF_RANGE),
		     "out of range") == 0 &&
	      strcmp(v34_precoder_result_name(
		     (enum v34_precoder_result)3),
		     "unknown") == 0,
	      "precoder result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "initialization", test_initialization },
		{ "complex sequences", test_complex_sequences },
		{ "receive reconstruction", test_receive_reconstruction },
		{ "receive reconstruction boundaries",
		  test_receive_reconstruction_boundaries },
		{ "rounding boundaries", test_rounding_boundaries },
		{ "channel boundaries", test_channel_boundaries },
		{ "range failures", test_range_failures },
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
