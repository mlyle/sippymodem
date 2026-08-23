// SPDX-License-Identifier: BSD-3-Clause

#include <v34_4d_encoder.h>

#include "test.h"

#include <limits.h>
#include <string.h>

static const struct v34_q14_coefficient COEFFICIENTS[] = {
	{ 4096, 2048 },
	{ -2048, 1024 },
	{ 1024, -2048 },
};

struct symbol_rate_fixture {
	enum v34_symbol_rate symbol_rate;
	uint8_t minimum_rate;
	uint8_t maximum_rate;
};

static const struct symbol_rate_fixture SYMBOL_RATES[] = {
	{ V34_SYMBOL_RATE_2400, 1, 9 },
	{ V34_SYMBOL_RATE_2743, 2, 11 },
	{ V34_SYMBOL_RATE_2800, 2, 11 },
	{ V34_SYMBOL_RATE_3000, 2, 12 },
	{ V34_SYMBOL_RATE_3200, 2, 13 },
	{ V34_SYMBOL_RATE_3429, 2, 14 },
};

static bool initialize_parameters(
	enum v34_symbol_rate symbol_rate, uint8_t primary_rate,
	bool auxiliary, bool expanded,
	struct v34_mapping_parameters *parameters)
{
	struct v34_frame_geometry geometry;

	return v34_frame_geometry_init(
		       &geometry, symbol_rate, primary_rate, auxiliary) ==
		       V34_FRAME_SCHEDULE_OK &&
	       v34_mapping_parameters_init(
		       parameters, &geometry, expanded) ==
		       V34_MAPPING_PARAMETERS_OK;
}

static bool grid_points_are_equal(
	const struct v34_grid_point *left,
	const struct v34_grid_point *right)
{
	return left->in_phase == right->in_phase &&
	       left->quadrature == right->quadrature;
}

static bool q7_points_are_equal(
	const struct v34_q7_point *left,
	const struct v34_q7_point *right)
{
	return left->in_phase == right->in_phase &&
	       left->quadrature == right->quadrature;
}

static bool precoder_symbols_are_equal(
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

static bool encoders_are_equal(
	const struct v34_4d_encoder *left,
	const struct v34_4d_encoder *right)
{
	return memcmp(
		       &left->parameters, &right->parameters,
		       sizeof(left->parameters)) == 0 &&
	       precoders_are_equal(&left->precoder, &right->precoder) &&
	       left->convolutional_encoder.state ==
		       right->convolutional_encoder.state &&
	       left->convolutional_encoder.selection ==
		       right->convolutional_encoder.selection &&
	       left->previous_differential_output ==
		       right->previous_differential_output;
}

static bool outputs_are_equal(
	const struct v34_4d_encoder_output *left,
	const struct v34_4d_encoder_output *right)
{
	if (left->differential_output != right->differential_output ||
	    left->modulo_output != right->modulo_output ||
	    left->convolutional_output != right->convolutional_output ||
	    left->trellis_output != right->trellis_output ||
	    memcmp(
		    &left->convolutional_input, &right->convolutional_input,
		    sizeof(left->convolutional_input)) != 0)
		return false;
	for (uint8_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		if (left->subset_label[symbol] != right->subset_label[symbol] ||
		    left->mapped[symbol].constellation_label !=
			    right->mapped[symbol].constellation_label ||
		    !grid_points_are_equal(
			    &left->mapped[symbol].mapper_output,
			    &right->mapped[symbol].mapper_output) ||
		    !precoder_symbols_are_equal(
			    &left->precoded[symbol], &right->precoded[symbol]))
			return false;
	}
	return true;
}

static bool model_step(
	struct v34_4d_encoder *encoder,
	const struct v34_4d_encoder_input *input,
	struct v34_4d_encoder_output *output)
{
	struct v34_4d_encoder next = *encoder;
	struct v34_4d_encoder_output generated = { 0 };
	struct v34_precoder_prediction second_prediction;
	struct v34_2d_mapping_input mapping;

	generated.differential_output = (uint8_t)(
		(next.previous_differential_output +
		 input->mapping.differential_input) &
		3u);
	mapping = (struct v34_2d_mapping_input){
		.ring_index = input->ring_index[0],
		.point_index = input->mapping.point_index[0],
		.clockwise_quarter_turns = generated.differential_output,
	};
	if (v34_map_2d_symbol(
		    &next.parameters, &mapping, &generated.mapped[0]) !=
		    V34_MAPPER_OK ||
	    v34_precoder_step(
		    &next.precoder, &generated.mapped[0].mapper_output,
		    &generated.precoded[0]) != V34_PRECODER_OK ||
	    v34_precoder_predict(
		    &next.precoder, &second_prediction) !=
		    V34_PRECODER_OK ||
	    v34_modulo_encoder_output(
		    &generated.precoded[0].lattice_offset,
		    &second_prediction.lattice_offset,
		    &generated.modulo_output) != V34_TRELLIS_OK ||
	    v34_convolutional_encoder_output(
		    &next.convolutional_encoder,
		    &generated.convolutional_output) != V34_TRELLIS_OK)
		return false;
	generated.trellis_output =
		generated.modulo_output ^ generated.convolutional_output ^
		input->inversion_bit;
	mapping = (struct v34_2d_mapping_input){
		.ring_index = input->ring_index[1],
		.point_index = input->mapping.point_index[1],
		.clockwise_quarter_turns = (uint8_t)(
			(generated.differential_output +
			 2u * input->mapping.rotation_bit +
			 generated.trellis_output) &
			3u),
	};
	if (v34_map_2d_symbol(
		    &next.parameters, &mapping, &generated.mapped[1]) !=
		    V34_MAPPER_OK ||
	    v34_precoder_step(
		    &next.precoder, &generated.mapped[1].mapper_output,
		    &generated.precoded[1]) != V34_PRECODER_OK)
		return false;
	for (uint8_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		if (v34_grid_subset_label(
			    &generated.precoded[symbol].channel_output,
			    &generated.subset_label[symbol]) !=
		    V34_TRELLIS_OK)
			return false;
	}
	if (v34_convolutional_input_from_subset_labels(
		    generated.subset_label[0], generated.subset_label[1],
		    &generated.convolutional_input) != V34_TRELLIS_OK ||
	    v34_convolutional_encoder_advance(
		    &next.convolutional_encoder,
		    &generated.convolutional_input) != V34_TRELLIS_OK)
		return false;
	next.previous_differential_output = generated.differential_output;
	*encoder = next;
	*output = generated;
	return true;
}

static bool test_initialization(void)
{
	struct v34_mapping_parameters parameters;

	CHECK(initialize_parameters(
		      V34_SYMBOL_RATE_3000, 8, false, false,
		      &parameters),
	      "initialization parameters failed");
	for (enum v34_trellis_selection selection =
		     V34_TRELLIS_16_STATE;
	     selection <= V34_TRELLIS_64_STATE; ++selection) {
		struct v34_4d_encoder encoder;

		CHECK(v34_4d_encoder_init(
			      &encoder, &parameters, COEFFICIENTS, selection) ==
			      V34_4D_ENCODER_OK &&
		      v34_4d_encoder_is_valid(&encoder) &&
		      encoder.previous_differential_output == 0u &&
		      encoder.convolutional_encoder.state == 0u &&
		      encoder.convolutional_encoder.selection == selection,
		      "trellis selection %u did not initialize", selection);
		for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT;
		     ++tap) {
			CHECK(encoder.precoder.coefficients[tap].real ==
				      COEFFICIENTS[tap].real &&
			      encoder.precoder.coefficients[tap].imaginary ==
				      COEFFICIENTS[tap].imaginary &&
			      encoder.precoder.previous_symbols[tap].in_phase == 0 &&
			      encoder.precoder.previous_symbols[tap].quadrature == 0,
			      "precoder tap %zu did not initialize", tap);
		}
	}
	return true;
}

static bool test_all_supported_sequences(void)
{
	bool grid_step_seen[2] = { false };
	bool trellis_seen[3] = { false };
	bool differential_seen[4] = { false };
	bool trellis_output_seen[2] = { false };
	bool nonzero_offset_seen = false;
	size_t configuration_count = 0;
	size_t interval_count = 0;

	for (size_t rate_index = 0; rate_index < ARRAY_LENGTH(SYMBOL_RATES);
	     ++rate_index) {
		for (uint8_t rate = SYMBOL_RATES[rate_index].minimum_rate;
		     rate <= SYMBOL_RATES[rate_index].maximum_rate; ++rate) {
			for (uint8_t auxiliary = 0; auxiliary < 2; ++auxiliary) {
				for (uint8_t expanded = 0; expanded < 2; ++expanded) {
					struct v34_mapping_parameters parameters;

					CHECK(initialize_parameters(
						      SYMBOL_RATES[rate_index].symbol_rate,
						      rate, auxiliary != 0, expanded != 0,
						      &parameters),
					      "configuration %zu/%u/%u/%u failed",
					      rate_index, rate, auxiliary, expanded);
					for (enum v34_trellis_selection selection =
						     V34_TRELLIS_16_STATE;
					     selection <= V34_TRELLIS_64_STATE;
					     ++selection) {
						struct v34_4d_encoder actual;
						struct v34_4d_encoder model;
						uint8_t point_limit = (uint8_t)(
							1u << parameters
								      .point_index_bits_per_2d_symbol);

						CHECK(v34_4d_encoder_init(
							      &actual, &parameters, COEFFICIENTS,
							      selection) ==
							      V34_4D_ENCODER_OK,
						      "encoder configuration did not initialize");
						model = actual;
						configuration_count++;
						trellis_seen[selection] = true;
						grid_step_seen[actual.precoder.grid_step ==
							       V34_PRECODER_GRID_STEP_4] =
							true;
						for (uint8_t interval = 0; interval < 8;
						     ++interval) {
							struct v34_4d_encoder_input input = {
								.mapping = {
									.rotation_bit = interval & 1u,
									.differential_input =
										(uint8_t)((interval + rate +
										 selection) & 3u),
									.point_index = {
										(uint8_t)((3u * interval + rate) %
											  point_limit),
										(uint8_t)((5u * interval +
											  selection + expanded) %
											 point_limit),
									},
								},
								.ring_index = {
									(uint8_t)((interval + rate_index) %
										  parameters.ring_count),
									(uint8_t)((3u * interval + rate) %
										  parameters.ring_count),
								},
								.inversion_bit =
									(uint8_t)((interval >> 1) & 1u),
							};
							struct v34_4d_encoder_output expected;
							struct v34_4d_encoder_output generated;

							CHECK(model_step(&model, &input, &expected) &&
							      v34_4d_encoder_step(
								      &actual, &input, &generated) ==
								      V34_4D_ENCODER_OK &&
							      outputs_are_equal(&generated, &expected) &&
							      encoders_are_equal(&actual, &model),
							      "sequence %zu/%u/%u/%u/%u/%u changed",
							      rate_index, rate, auxiliary, expanded,
							      selection, interval);
							differential_seen[generated.differential_output] =
								true;
							trellis_output_seen[generated.trellis_output] = true;
							for (uint8_t symbol = 0;
							     symbol <
								     V34_2D_SYMBOLS_PER_4D_INTERVAL;
							     ++symbol) {
								nonzero_offset_seen |=
									generated.precoded[symbol]
										.lattice_offset.in_phase != 0 ||
									generated.precoded[symbol]
										.lattice_offset.quadrature != 0;
							}
							interval_count++;
						}
					}
				}
			}
		}
	}
	CHECK(configuration_count == 780u && interval_count == 6240u &&
	      grid_step_seen[0] && grid_step_seen[1] &&
	      trellis_seen[0] && trellis_seen[1] && trellis_seen[2] &&
	      differential_seen[0] && differential_seen[1] &&
	      differential_seen[2] && differential_seen[3] &&
	      trellis_output_seen[0] && trellis_output_seen[1] &&
	      nonzero_offset_seen,
	      "sequence sweep did not exercise every state boundary");
	return true;
}

static bool rejected_step_is_transactional(
	struct v34_4d_encoder *encoder,
	const struct v34_4d_encoder_input *input,
	enum v34_4d_encoder_result expected_result)
{
	struct v34_4d_encoder saved = *encoder;
	struct v34_4d_encoder_output output;
	struct v34_4d_encoder_output saved_output;

	memset(&output, 0xa5, sizeof(output));
	saved_output = output;
	return v34_4d_encoder_step(encoder, input, &output) ==
		       expected_result &&
	       encoders_are_equal(encoder, &saved) &&
	       memcmp(&output, &saved_output, sizeof(output)) == 0;
}

static bool test_transactional_failures(void)
{
	struct v34_mapping_parameters parameters;
	struct v34_4d_encoder encoder;
	struct v34_4d_encoder invalid;
	struct v34_4d_encoder_input input = { 0 };
	uint8_t point_limit;

	CHECK(initialize_parameters(
		      V34_SYMBOL_RATE_3000, 8, false, false,
		      &parameters) &&
	      v34_4d_encoder_init(
		      &encoder, &parameters, COEFFICIENTS,
		      V34_TRELLIS_16_STATE) ==
		      V34_4D_ENCODER_OK,
	      "transactional fixture did not initialize");
	point_limit = (uint8_t)(
		1u << parameters.point_index_bits_per_2d_symbol);
	input.mapping.rotation_bit = 2;
	CHECK(rejected_step_is_transactional(
		      &encoder, &input,
		      V34_4D_ENCODER_INVALID_ARGUMENT),
	      "invalid rotation changed state or output");
	input.mapping.rotation_bit = 0;
	input.mapping.differential_input = 4;
	CHECK(rejected_step_is_transactional(
		      &encoder, &input,
		      V34_4D_ENCODER_INVALID_ARGUMENT),
	      "invalid differential input changed state or output");
	input.mapping.differential_input = 0;
	input.inversion_bit = 2;
	CHECK(rejected_step_is_transactional(
		      &encoder, &input,
		      V34_4D_ENCODER_INVALID_ARGUMENT),
	      "invalid inversion changed state or output");
	input.inversion_bit = 0;
	input.ring_index[0] = parameters.ring_count;
	CHECK(rejected_step_is_transactional(
		      &encoder, &input,
		      V34_4D_ENCODER_INVALID_ARGUMENT),
	      "invalid ring changed state or output");
	input.ring_index[0] = 0;
	input.mapping.point_index[1] = point_limit;
	CHECK(rejected_step_is_transactional(
		      &encoder, &input,
		      V34_4D_ENCODER_INVALID_ARGUMENT),
	      "invalid point index changed state or output");
	input.mapping.point_index[1] = 0;

	invalid = encoder;
	invalid.precoder.coefficients[0].real = INT16_MAX;
	invalid.precoder.coefficients[1].real = INT16_MAX;
	invalid.precoder.coefficients[2].real = INT16_MAX;
	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT;
	     ++tap) {
		invalid.precoder.previous_symbols[tap].in_phase = INT32_MAX;
		invalid.precoder.previous_symbols[tap].quadrature = INT32_MAX;
	}
	CHECK(v34_4d_encoder_is_valid(&invalid) &&
	      rejected_step_is_transactional(
		      &invalid, &input, V34_4D_ENCODER_OUT_OF_RANGE),
	      "precoder overflow changed interval state or output");

	invalid = encoder;
	memset(invalid.precoder.coefficients, 0,
	       sizeof(invalid.precoder.coefficients));
	invalid.precoder.coefficients[1].real = V34_Q14_SCALE;
	invalid.precoder.previous_symbols[0] =
		(struct v34_q7_point){
			254 * V34_Q7_SCALE,
			254 * V34_Q7_SCALE,
		};
	invalid.precoder.previous_symbols[1] =
		invalid.precoder.previous_symbols[0];
	input.mapping.differential_input = 2;
	input.mapping.rotation_bit = 1;
	input.ring_index[1] = 1;
	CHECK(parameters.ring_count > 1u &&
	      v34_4d_encoder_is_valid(&invalid) &&
	      rejected_step_is_transactional(
		      &invalid, &input, V34_4D_ENCODER_OUT_OF_RANGE),
	      "second-symbol overflow did not roll back the interval");
	return true;
}

static bool test_validation(void)
{
	struct v34_mapping_parameters parameters;
	struct v34_mapping_parameters invalid_parameters;
	struct v34_4d_encoder encoder;
	struct v34_4d_encoder saved;
	struct v34_q14_coefficient invalid_coefficients
		[V34_PRECODER_TAP_COUNT] = { 0 };
	struct v34_4d_encoder_input input = { 0 };
	struct v34_4d_encoder_output output;

	CHECK(initialize_parameters(
		      V34_SYMBOL_RATE_3000, 8, false, false,
		      &parameters) &&
	      v34_4d_encoder_init(
		      &encoder, &parameters, COEFFICIENTS,
		      V34_TRELLIS_16_STATE) ==
		      V34_4D_ENCODER_OK,
	      "validation fixture did not initialize");
	saved = encoder;
	invalid_parameters = parameters;
	invalid_parameters.ring_count++;
	invalid_coefficients[0].real = INT16_MIN;
	CHECK(v34_4d_encoder_init(
		      NULL, &parameters, COEFFICIENTS,
		      V34_TRELLIS_16_STATE) ==
		      V34_4D_ENCODER_INVALID_ARGUMENT &&
	      v34_4d_encoder_init(
		      &encoder, NULL, COEFFICIENTS,
		      V34_TRELLIS_16_STATE) ==
		      V34_4D_ENCODER_INVALID_ARGUMENT &&
	      v34_4d_encoder_init(
		      &encoder, &invalid_parameters, COEFFICIENTS,
		      V34_TRELLIS_16_STATE) ==
		      V34_4D_ENCODER_INVALID_ARGUMENT &&
	      v34_4d_encoder_init(
		      &encoder, &parameters, NULL,
		      V34_TRELLIS_16_STATE) ==
		      V34_4D_ENCODER_INVALID_ARGUMENT &&
	      v34_4d_encoder_init(
		      &encoder, &parameters, invalid_coefficients,
		      V34_TRELLIS_16_STATE) ==
		      V34_4D_ENCODER_INVALID_ARGUMENT &&
	      v34_4d_encoder_init(
		      &encoder, &parameters, COEFFICIENTS,
		      (enum v34_trellis_selection)3) ==
		      V34_4D_ENCODER_INVALID_ARGUMENT &&
	      encoders_are_equal(&encoder, &saved),
	      "invalid initialization changed its destination");
	CHECK(!v34_4d_encoder_is_valid(NULL) &&
	      !v34_4d_encoder_input_is_valid(NULL, &parameters) &&
	      !v34_4d_encoder_input_is_valid(
		      &input, &invalid_parameters) &&
	      v34_4d_encoder_step(NULL, &input, &output) ==
		      V34_4D_ENCODER_INVALID_ARGUMENT &&
	      v34_4d_encoder_step(&encoder, NULL, &output) ==
		      V34_4D_ENCODER_INVALID_ARGUMENT &&
	      v34_4d_encoder_step(&encoder, &input, NULL) ==
		      V34_4D_ENCODER_INVALID_ARGUMENT,
	      "null or inconsistent input was accepted");
	encoder.previous_differential_output = 4;
	CHECK(!v34_4d_encoder_is_valid(&encoder),
	      "invalid differential state was accepted");
	encoder = saved;
	encoder.precoder.grid_step =
		encoder.precoder.grid_step == V34_PRECODER_GRID_STEP_2 ?
			V34_PRECODER_GRID_STEP_4 :
			V34_PRECODER_GRID_STEP_2;
	CHECK(!v34_4d_encoder_is_valid(&encoder),
	      "inconsistent precoder grid was accepted");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "initialization", test_initialization },
		{ "all supported sequences", test_all_supported_sequences },
		{ "transactional failures", test_transactional_failures },
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
