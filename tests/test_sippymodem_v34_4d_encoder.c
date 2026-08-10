// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_4d_encoder.h>

#include "test.h"

#include <limits.h>
#include <string.h>

static const struct sippymodem_v34_q14_coefficient COEFFICIENTS[] = {
	{ 4096, 2048 },
	{ -2048, 1024 },
	{ 1024, -2048 },
};

struct symbol_rate_fixture {
	enum sippymodem_v34_symbol_rate symbol_rate;
	uint8_t minimum_rate;
	uint8_t maximum_rate;
};

static const struct symbol_rate_fixture SYMBOL_RATES[] = {
	{ SIPPYMODEM_V34_SYMBOL_RATE_2400, 1, 9 },
	{ SIPPYMODEM_V34_SYMBOL_RATE_2743, 2, 11 },
	{ SIPPYMODEM_V34_SYMBOL_RATE_2800, 2, 11 },
	{ SIPPYMODEM_V34_SYMBOL_RATE_3000, 2, 12 },
	{ SIPPYMODEM_V34_SYMBOL_RATE_3200, 2, 13 },
	{ SIPPYMODEM_V34_SYMBOL_RATE_3429, 2, 14 },
};

static bool initialize_parameters(
	enum sippymodem_v34_symbol_rate symbol_rate, uint8_t primary_rate,
	bool auxiliary, bool expanded,
	struct sippymodem_v34_mapping_parameters *parameters)
{
	struct sippymodem_v34_frame_geometry geometry;

	return sippymodem_v34_frame_geometry_init(
		       &geometry, symbol_rate, primary_rate, auxiliary) ==
		       SIPPYMODEM_V34_FRAME_SCHEDULE_OK &&
	       sippymodem_v34_mapping_parameters_init(
		       parameters, &geometry, expanded) ==
		       SIPPYMODEM_V34_MAPPING_PARAMETERS_OK;
}

static bool grid_points_are_equal(
	const struct sippymodem_v34_grid_point *left,
	const struct sippymodem_v34_grid_point *right)
{
	return left->in_phase == right->in_phase &&
	       left->quadrature == right->quadrature;
}

static bool q7_points_are_equal(
	const struct sippymodem_v34_q7_point *left,
	const struct sippymodem_v34_q7_point *right)
{
	return left->in_phase == right->in_phase &&
	       left->quadrature == right->quadrature;
}

static bool precoder_symbols_are_equal(
	const struct sippymodem_v34_precoder_symbol *left,
	const struct sippymodem_v34_precoder_symbol *right)
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
	const struct sippymodem_v34_precoder *left,
	const struct sippymodem_v34_precoder *right)
{
	if (left->grid_step != right->grid_step)
		return false;
	for (size_t tap = 0; tap < SIPPYMODEM_V34_PRECODER_TAP_COUNT;
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
	const struct sippymodem_v34_4d_encoder *left,
	const struct sippymodem_v34_4d_encoder *right)
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
	const struct sippymodem_v34_4d_encoder_output *left,
	const struct sippymodem_v34_4d_encoder_output *right)
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
	     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
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
	struct sippymodem_v34_4d_encoder *encoder,
	const struct sippymodem_v34_4d_encoder_input *input,
	struct sippymodem_v34_4d_encoder_output *output)
{
	struct sippymodem_v34_4d_encoder next = *encoder;
	struct sippymodem_v34_4d_encoder_output generated = { 0 };
	struct sippymodem_v34_precoder_prediction second_prediction;
	struct sippymodem_v34_2d_mapping_input mapping;

	generated.differential_output = (uint8_t)(
		(next.previous_differential_output +
		 input->mapping.differential_input) &
		3u);
	mapping = (struct sippymodem_v34_2d_mapping_input){
		.ring_index = input->ring_index[0],
		.point_index = input->mapping.point_index[0],
		.clockwise_quarter_turns = generated.differential_output,
	};
	if (sippymodem_v34_map_2d_symbol(
		    &next.parameters, &mapping, &generated.mapped[0]) !=
		    SIPPYMODEM_V34_MAPPER_OK ||
	    sippymodem_v34_precoder_step(
		    &next.precoder, &generated.mapped[0].mapper_output,
		    &generated.precoded[0]) != SIPPYMODEM_V34_PRECODER_OK ||
	    sippymodem_v34_precoder_predict(
		    &next.precoder, &second_prediction) !=
		    SIPPYMODEM_V34_PRECODER_OK ||
	    sippymodem_v34_modulo_encoder_output(
		    &generated.precoded[0].lattice_offset,
		    &second_prediction.lattice_offset,
		    &generated.modulo_output) != SIPPYMODEM_V34_TRELLIS_OK ||
	    sippymodem_v34_convolutional_encoder_output(
		    &next.convolutional_encoder,
		    &generated.convolutional_output) != SIPPYMODEM_V34_TRELLIS_OK)
		return false;
	generated.trellis_output =
		generated.modulo_output ^ generated.convolutional_output ^
		input->inversion_bit;
	mapping = (struct sippymodem_v34_2d_mapping_input){
		.ring_index = input->ring_index[1],
		.point_index = input->mapping.point_index[1],
		.clockwise_quarter_turns = (uint8_t)(
			(generated.differential_output +
			 2u * input->mapping.rotation_bit +
			 generated.trellis_output) &
			3u),
	};
	if (sippymodem_v34_map_2d_symbol(
		    &next.parameters, &mapping, &generated.mapped[1]) !=
		    SIPPYMODEM_V34_MAPPER_OK ||
	    sippymodem_v34_precoder_step(
		    &next.precoder, &generated.mapped[1].mapper_output,
		    &generated.precoded[1]) != SIPPYMODEM_V34_PRECODER_OK)
		return false;
	for (uint8_t symbol = 0;
	     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		if (sippymodem_v34_grid_subset_label(
			    &generated.precoded[symbol].channel_output,
			    &generated.subset_label[symbol]) !=
		    SIPPYMODEM_V34_TRELLIS_OK)
			return false;
	}
	if (sippymodem_v34_convolutional_input_from_subset_labels(
		    generated.subset_label[0], generated.subset_label[1],
		    &generated.convolutional_input) != SIPPYMODEM_V34_TRELLIS_OK ||
	    sippymodem_v34_convolutional_encoder_advance(
		    &next.convolutional_encoder,
		    &generated.convolutional_input) != SIPPYMODEM_V34_TRELLIS_OK)
		return false;
	next.previous_differential_output = generated.differential_output;
	*encoder = next;
	*output = generated;
	return true;
}

static bool test_initialization(void)
{
	struct sippymodem_v34_mapping_parameters parameters;

	CHECK(initialize_parameters(
		      SIPPYMODEM_V34_SYMBOL_RATE_3000, 8, false, false,
		      &parameters),
	      "initialization parameters failed");
	for (enum sippymodem_v34_trellis_selection selection =
		     SIPPYMODEM_V34_TRELLIS_16_STATE;
	     selection <= SIPPYMODEM_V34_TRELLIS_64_STATE; ++selection) {
		struct sippymodem_v34_4d_encoder encoder;

		CHECK(sippymodem_v34_4d_encoder_init(
			      &encoder, &parameters, COEFFICIENTS, selection) ==
			      SIPPYMODEM_V34_4D_ENCODER_OK &&
		      sippymodem_v34_4d_encoder_is_valid(&encoder) &&
		      encoder.previous_differential_output == 0u &&
		      encoder.convolutional_encoder.state == 0u &&
		      encoder.convolutional_encoder.selection == selection,
		      "trellis selection %u did not initialize", selection);
		for (size_t tap = 0; tap < SIPPYMODEM_V34_PRECODER_TAP_COUNT;
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
					struct sippymodem_v34_mapping_parameters parameters;

					CHECK(initialize_parameters(
						      SYMBOL_RATES[rate_index].symbol_rate,
						      rate, auxiliary != 0, expanded != 0,
						      &parameters),
					      "configuration %zu/%u/%u/%u failed",
					      rate_index, rate, auxiliary, expanded);
					for (enum sippymodem_v34_trellis_selection selection =
						     SIPPYMODEM_V34_TRELLIS_16_STATE;
					     selection <= SIPPYMODEM_V34_TRELLIS_64_STATE;
					     ++selection) {
						struct sippymodem_v34_4d_encoder actual;
						struct sippymodem_v34_4d_encoder model;
						uint8_t point_limit = (uint8_t)(
							1u << parameters
								      .point_index_bits_per_2d_symbol);

						CHECK(sippymodem_v34_4d_encoder_init(
							      &actual, &parameters, COEFFICIENTS,
							      selection) ==
							      SIPPYMODEM_V34_4D_ENCODER_OK,
						      "encoder configuration did not initialize");
						model = actual;
						configuration_count++;
						trellis_seen[selection] = true;
						grid_step_seen[actual.precoder.grid_step ==
							       SIPPYMODEM_V34_PRECODER_GRID_STEP_4] =
							true;
						for (uint8_t interval = 0; interval < 8;
						     ++interval) {
							struct sippymodem_v34_4d_encoder_input input = {
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
							struct sippymodem_v34_4d_encoder_output expected;
							struct sippymodem_v34_4d_encoder_output generated;

							CHECK(model_step(&model, &input, &expected) &&
							      sippymodem_v34_4d_encoder_step(
								      &actual, &input, &generated) ==
								      SIPPYMODEM_V34_4D_ENCODER_OK &&
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
								     SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL;
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

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "initialization", test_initialization },
		{ "all supported sequences", test_all_supported_sequences },
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
