// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_mapping_frame_encoder.h>

#include "test.h"

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

static bool interval_outputs_are_equal(
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
		const struct sippymodem_v34_precoder_symbol *left_precoded =
			&left->precoded[symbol];
		const struct sippymodem_v34_precoder_symbol *right_precoded =
			&right->precoded[symbol];

		if (left->subset_label[symbol] != right->subset_label[symbol] ||
		    left->mapped[symbol].constellation_label !=
			    right->mapped[symbol].constellation_label ||
		    !grid_points_are_equal(
			    &left->mapped[symbol].mapper_output,
			    &right->mapped[symbol].mapper_output) ||
		    !q7_points_are_equal(
			    &left_precoded->filter_output,
			    &right_precoded->filter_output) ||
		    !grid_points_are_equal(
			    &left_precoded->lattice_offset,
			    &right_precoded->lattice_offset) ||
		    !grid_points_are_equal(
			    &left_precoded->channel_output,
			    &right_precoded->channel_output) ||
		    !q7_points_are_equal(
			    &left_precoded->precoded_symbol,
			    &right_precoded->precoded_symbol))
			return false;
	}
	return true;
}

static bool encoded_frames_are_equal(
	const struct sippymodem_v34_encoded_mapping_frame *left,
	const struct sippymodem_v34_encoded_mapping_frame *right)
{
	if (left->parsed.shell_label != right->parsed.shell_label)
		return false;
	for (uint8_t interval = 0;
	     interval < SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME;
	     ++interval) {
		const struct sippymodem_v34_4d_mapping_input *left_mapping =
			&left->parsed.interval[interval];
		const struct sippymodem_v34_4d_mapping_input *right_mapping =
			&right->parsed.interval[interval];

		if (left_mapping->rotation_bit != right_mapping->rotation_bit ||
		    left_mapping->differential_input !=
			    right_mapping->differential_input ||
		    !interval_outputs_are_equal(
			    &left->interval[interval],
			    &right->interval[interval]))
			return false;
		for (uint8_t symbol = 0;
		     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL;
		     ++symbol) {
			if (left_mapping->point_index[symbol] !=
				    right_mapping->point_index[symbol] ||
			    left->ring_indices.index[interval][symbol] !=
				    right->ring_indices.index[interval][symbol])
				return false;
		}
	}
	return true;
}

static bool encoders_are_equal(
	const struct sippymodem_v34_4d_encoder *left,
	const struct sippymodem_v34_4d_encoder *right)
{
	if (memcmp(
		    &left->parameters, &right->parameters,
		    sizeof(left->parameters)) != 0 ||
	    left->precoder.grid_step != right->precoder.grid_step ||
	    left->convolutional_encoder.state !=
		    right->convolutional_encoder.state ||
	    left->convolutional_encoder.selection !=
		    right->convolutional_encoder.selection ||
	    left->previous_differential_output !=
		    right->previous_differential_output)
		return false;
	for (size_t tap = 0; tap < SIPPYMODEM_V34_PRECODER_TAP_COUNT;
	     ++tap) {
		if (left->precoder.coefficients[tap].real !=
			    right->precoder.coefficients[tap].real ||
		    left->precoder.coefficients[tap].imaginary !=
			    right->precoder.coefficients[tap].imaginary ||
		    !q7_points_are_equal(
			    &left->precoder.previous_symbols[tap],
			    &right->precoder.previous_symbols[tap]))
			return false;
	}
	return true;
}

static bool model_encode_mapping_frame(
	struct sippymodem_v34_4d_encoder *encoder,
	const struct sippymodem_v34_mapping_frame *mapping_frame,
	const uint8_t *primary_bits, const uint8_t *auxiliary_bit,
	struct sippymodem_v34_encoded_mapping_frame *encoded)
{
	struct sippymodem_v34_4d_encoder next = *encoder;
	struct sippymodem_v34_encoded_mapping_frame generated = { 0 };

	if (sippymodem_v34_parse_mapping_frame(
		    &next.parameters, mapping_frame, primary_bits,
		    mapping_frame->primary_bits, auxiliary_bit,
		    &generated.parsed) != SIPPYMODEM_V34_PARSER_OK ||
	    sippymodem_v34_shell_map(
		    &next.parameters, generated.parsed.shell_label,
		    &generated.ring_indices) != SIPPYMODEM_V34_SHELL_MAPPER_OK)
		return false;
	for (uint8_t interval = 0;
	     interval < SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME;
	     ++interval) {
		const struct sippymodem_v34_4d_encoder_input input = {
			.mapping = generated.parsed.interval[interval],
			.ring_index = {
				generated.ring_indices.index[interval][0],
				generated.ring_indices.index[interval][1],
			},
			.inversion_bit = (uint8_t)(
				(mapping_frame->inversion_4d_mask >> interval) & 1u),
		};

		if (sippymodem_v34_4d_encoder_step(
			    &next, &input, &generated.interval[interval]) !=
		    SIPPYMODEM_V34_4D_ENCODER_OK)
			return false;
	}
	*encoder = next;
	*encoded = generated;
	return true;
}

static void generate_primary_bits(
	uint8_t *bits, uint8_t count, uint32_t seed)
{
	uint32_t state = seed | 1u;

	for (uint8_t index = 0; index < count; ++index) {
		state = state * UINT32_C(1664525) + UINT32_C(1013904223);
		bits[index] = (uint8_t)(state >> 31);
	}
}

static bool test_all_supported_mapping_frames(void)
{
	enum { MAX_PRIMARY_BITS = 79 };
	bool high_frame_seen[2] = { false };
	bool auxiliary_frame_seen[2] = { false };
	bool inversion_seen[2] = { false };
	bool nonzero_shell_seen = false;
	size_t configuration_count = 0;
	size_t mapping_frame_count = 0;

	for (size_t symbol_index = 0; symbol_index < ARRAY_LENGTH(SYMBOL_RATES);
	     ++symbol_index) {
		for (uint8_t rate = SYMBOL_RATES[symbol_index].minimum_rate;
		     rate <= SYMBOL_RATES[symbol_index].maximum_rate; ++rate) {
			for (uint8_t auxiliary = 0; auxiliary < 2; ++auxiliary) {
				struct sippymodem_v34_frame_geometry geometry;

				CHECK(sippymodem_v34_frame_geometry_init(
					      &geometry,
					      SYMBOL_RATES[symbol_index].symbol_rate, rate,
					      auxiliary != 0) ==
					      SIPPYMODEM_V34_FRAME_SCHEDULE_OK,
				      "geometry %zu/%u/%u failed", symbol_index,
				      rate, auxiliary);
				for (uint8_t expanded = 0; expanded < 2; ++expanded) {
					struct sippymodem_v34_mapping_parameters parameters;

					CHECK(sippymodem_v34_mapping_parameters_init(
						      &parameters, &geometry, expanded != 0) ==
						      SIPPYMODEM_V34_MAPPING_PARAMETERS_OK,
					      "parameters %zu/%u/%u/%u failed",
					      symbol_index, rate, auxiliary, expanded);
					for (enum sippymodem_v34_trellis_selection selection =
						     SIPPYMODEM_V34_TRELLIS_16_STATE;
					     selection <= SIPPYMODEM_V34_TRELLIS_64_STATE;
					     ++selection) {
						struct sippymodem_v34_frame_schedule schedule;
						struct sippymodem_v34_4d_encoder actual;
						struct sippymodem_v34_4d_encoder model;

						CHECK(sippymodem_v34_frame_schedule_init(
							      &schedule, &geometry,
							      (uint8_t)((rate + expanded + selection) %
								      geometry
									      .data_frames_per_superframe)) ==
							      SIPPYMODEM_V34_FRAME_SCHEDULE_OK &&
						      sippymodem_v34_4d_encoder_init(
							      &actual, &parameters, COEFFICIENTS,
							      selection) ==
							      SIPPYMODEM_V34_4D_ENCODER_OK,
						      "encoder %zu/%u/%u/%u/%u failed",
						      symbol_index, rate, auxiliary, expanded,
						      selection);
						model = actual;
						configuration_count++;
						while (!sippymodem_v34_frame_schedule_is_complete(
							&schedule)) {
							struct sippymodem_v34_mapping_frame frame;
							struct sippymodem_v34_encoded_mapping_frame expected;
							struct sippymodem_v34_encoded_mapping_frame generated;
							uint8_t primary_bits[MAX_PRIMARY_BITS];
							uint8_t auxiliary_bit = (uint8_t)(
								(mapping_frame_count + rate + selection) &
								1u);
							const uint8_t *auxiliary_pointer;

							CHECK(sippymodem_v34_frame_schedule_next(
								      &schedule, &frame) ==
								      SIPPYMODEM_V34_FRAME_SCHEDULE_OK,
							      "schedule ended early");
							generate_primary_bits(
								primary_bits, frame.primary_bits,
								(uint32_t)mapping_frame_count + rate +
									selection + 1u);
							auxiliary_pointer = frame.carries_auxiliary_bit ?
								&auxiliary_bit :
								NULL;
							CHECK(model_encode_mapping_frame(
								      &model, &frame, primary_bits,
								      auxiliary_pointer, &expected) &&
							      sippymodem_v34_encode_mapping_frame(
								      &actual, &frame, primary_bits,
								      frame.primary_bits, auxiliary_pointer,
								      &generated) ==
								      SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_OK &&
							      encoded_frames_are_equal(
								      &generated, &expected) &&
							      encoders_are_equal(&actual, &model),
							      "mapping frame %zu/%u/%u/%u/%u/%u "
							      "changed",
							      symbol_index, rate, auxiliary, expanded,
							      selection, frame.index);
							high_frame_seen[frame.high_frame] = true;
							auxiliary_frame_seen
								[frame.carries_auxiliary_bit] = true;
							inversion_seen[frame.inversion_4d_mask != 0] =
								true;
							nonzero_shell_seen |=
								generated.parsed.shell_label != 0;
							mapping_frame_count++;
						}
					}
				}
			}
		}
	}
	CHECK(configuration_count == 780u && mapping_frame_count == 11040u &&
	      high_frame_seen[0] && high_frame_seen[1] &&
	      auxiliary_frame_seen[0] && auxiliary_frame_seen[1] &&
	      inversion_seen[0] && inversion_seen[1] && nonzero_shell_seen,
	      "mapping-frame sweep missed a scheduling boundary");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "all supported mapping frames",
		  test_all_supported_mapping_frames },
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
