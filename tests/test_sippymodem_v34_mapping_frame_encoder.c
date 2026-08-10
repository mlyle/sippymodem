// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_mapping_frame_encoder.h>

#include "test.h"
#include "v34_encoder_test_helpers.h"

#include <string.h>

enum {
	MAX_PRIMARY_BITS = SIPPYMODEM_V34_MAX_MAPPING_FRAME_BITS,
};

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

static bool bytes_are_filled(
	const void *object, size_t size, unsigned char value)
{
	const unsigned char *bytes = object;

	for (size_t index = 0; index < size; ++index) {
		if (bytes[index] != value)
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

static bool initialize_frame_fixture(
	struct sippymodem_v34_mapping_parameters *parameters,
	struct sippymodem_v34_4d_encoder *encoder,
	struct sippymodem_v34_mapping_frame *frame,
	uint8_t primary_bits[MAX_PRIMARY_BITS])
{
	struct sippymodem_v34_frame_geometry geometry;

	if (sippymodem_v34_frame_geometry_init(
		    &geometry, SIPPYMODEM_V34_SYMBOL_RATE_3000, 8, false) !=
		    SIPPYMODEM_V34_FRAME_SCHEDULE_OK ||
	    sippymodem_v34_mapping_parameters_init(
		    parameters, &geometry, false) !=
		    SIPPYMODEM_V34_MAPPING_PARAMETERS_OK ||
	    sippymodem_v34_4d_encoder_init(
		    encoder, parameters, COEFFICIENTS,
		    SIPPYMODEM_V34_TRELLIS_16_STATE) !=
		    SIPPYMODEM_V34_4D_ENCODER_OK)
		return false;
	*frame = (struct sippymodem_v34_mapping_frame){
		.data_bits = parameters->mapping_frame_bits,
		.primary_bits = parameters->mapping_frame_bits,
		.high_frame = true,
	};
	memset(primary_bits, 0, MAX_PRIMARY_BITS);
	return true;
}

static bool rejected_frame_is_transactional(
	struct sippymodem_v34_4d_encoder *encoder,
	const struct sippymodem_v34_mapping_frame *frame,
	const uint8_t *primary_bits, size_t primary_bit_count,
	const uint8_t *auxiliary_bit,
	enum sippymodem_v34_mapping_frame_encoder_result expected_result)
{
	struct sippymodem_v34_4d_encoder saved = *encoder;
	struct sippymodem_v34_encoded_mapping_frame encoded;

	memset(&encoded, 0xa5, sizeof(encoded));
	return sippymodem_v34_encode_mapping_frame(
		       encoder, frame, primary_bits, primary_bit_count,
		       auxiliary_bit, &encoded) == expected_result &&
	       v34_test_encoders_are_equal(encoder, &saved) &&
	       bytes_are_filled(&encoded, sizeof(encoded), 0xa5);
}

static bool append_value(
	uint8_t bits[MAX_PRIMARY_BITS], size_t *count, uint32_t value,
	uint8_t bit_count)
{
	if (*count + bit_count > MAX_PRIMARY_BITS)
		return false;
	for (uint8_t index = 0; index < bit_count; ++index)
		bits[(*count)++] = (uint8_t)((value >> index) & 1u);
	return true;
}

static bool build_regular_primary_bits(
	const struct sippymodem_v34_mapping_parameters *parameters,
	const struct sippymodem_v34_mapping_frame *frame,
	const struct sippymodem_v34_parsed_mapping_frame *parsed,
	uint8_t primary_bits[MAX_PRIMARY_BITS], size_t *primary_bit_count)
{
	size_t count = 0;
	uint8_t shell_bits;

	if (parameters->mapping_frame_bits <= 12u ||
	    frame->carries_auxiliary_bit)
		return false;
	shell_bits = (uint8_t)(
		parameters->shell_mapper_bits - !frame->high_frame);
	if (!append_value(primary_bits, &count, parsed->shell_label, shell_bits))
		return false;
	for (uint8_t interval = 0;
	     interval < SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME;
	     ++interval) {
		const struct sippymodem_v34_4d_mapping_input *input =
			&parsed->interval[interval];

		if (!append_value(
			    primary_bits, &count, input->rotation_bit, 1) ||
		    !append_value(
			    primary_bits, &count, input->differential_input, 2))
			return false;
		for (uint8_t symbol = 0;
		     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL;
		     ++symbol) {
			if (!append_value(
				    primary_bits, &count,
				    input->point_index[symbol],
				    parameters
					    ->point_index_bits_per_2d_symbol))
				return false;
		}
	}
	*primary_bit_count = count;
	return count == frame->primary_bits;
}

static bool find_delayed_overflow_inputs(
	const struct sippymodem_v34_mapping_parameters *parameters,
	uint32_t *shell_label, uint8_t *point_index)
{
	uint32_t label_limit = UINT32_C(1) << parameters->shell_mapper_bits;
	uint8_t point_limit = (uint8_t)(
		1u << parameters->point_index_bits_per_2d_symbol);

	for (uint32_t label = 0; label < label_limit && label < 64u; ++label) {
		struct sippymodem_v34_ring_indices rings;

		if (sippymodem_v34_shell_map(parameters, label, &rings) !=
			    SIPPYMODEM_V34_SHELL_MAPPER_OK ||
		    rings.index[0][0] != 0 || rings.index[0][1] != 0 ||
		    rings.index[1][0] == 0)
			continue;
		for (uint8_t point = 0; point < point_limit; ++point) {
			const struct sippymodem_v34_2d_mapping_input input = {
				.ring_index = rings.index[1][0],
				.point_index = point,
			};
			struct sippymodem_v34_2d_mapping_output output;

			if (sippymodem_v34_map_2d_symbol(
				    parameters, &input, &output) ==
				    SIPPYMODEM_V34_MAPPER_OK &&
			    (output.mapper_output.in_phase > 1 ||
			     output.mapper_output.quadrature > 1)) {
				*shell_label = label;
				*point_index = point;
				return true;
			}
		}
	}
	return false;
}

static bool test_all_supported_mapping_frames(void)
{
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
						      (unsigned int)selection);
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
							      v34_test_encoded_frames_are_equal(
								      &generated, &expected) &&
							      v34_test_encoders_are_equal(&actual, &model),
							      "mapping frame %zu/%u/%u/%u/%u/%u "
							      "changed",
							      symbol_index, rate, auxiliary, expanded,
							      (unsigned int)selection, frame.index);
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

static bool test_transactional_failures(void)
{
	struct sippymodem_v34_mapping_parameters parameters;
	struct sippymodem_v34_4d_encoder encoder;
	struct sippymodem_v34_4d_encoder invalid;
	struct sippymodem_v34_4d_encoder saved;
	struct sippymodem_v34_mapping_frame frame;
	struct sippymodem_v34_mapping_frame malformed;
	struct sippymodem_v34_encoded_mapping_frame encoded;
	uint8_t primary_bits[MAX_PRIMARY_BITS];
	uint8_t auxiliary_bit = 0;

	CHECK(initialize_frame_fixture(
		      &parameters, &encoder, &frame, primary_bits),
	      "transactional fixture did not initialize");
	memset(&encoded, 0xa5, sizeof(encoded));
	CHECK(sippymodem_v34_encode_mapping_frame(
		      NULL, &frame, primary_bits, frame.primary_bits, NULL,
		      &encoded) ==
		      SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_INVALID_ARGUMENT &&
	      bytes_are_filled(&encoded, sizeof(encoded), 0xa5),
	      "null encoder changed the destination");
	CHECK(rejected_frame_is_transactional(
		      &encoder, NULL, primary_bits, frame.primary_bits, NULL,
		      SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_INVALID_ARGUMENT),
	      "null frame changed state or output");
	saved = encoder;
	CHECK(sippymodem_v34_encode_mapping_frame(
		      &encoder, &frame, primary_bits, frame.primary_bits, NULL,
		      NULL) ==
		      SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_INVALID_ARGUMENT &&
	      v34_test_encoders_are_equal(&encoder, &saved),
	      "null destination changed state");
	CHECK(rejected_frame_is_transactional(
		      &encoder, &frame, NULL, frame.primary_bits, NULL,
		      SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_INVALID_ARGUMENT) &&
	      rejected_frame_is_transactional(
		      &encoder, &frame, primary_bits,
		      (size_t)frame.primary_bits - 1u, NULL,
		      SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_INVALID_ARGUMENT),
	      "invalid primary buffer changed state or output");
	primary_bits[0] = 2;
	CHECK(rejected_frame_is_transactional(
		      &encoder, &frame, primary_bits, frame.primary_bits, NULL,
		      SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_INVALID_ARGUMENT),
	      "non-bit primary input changed state or output");
	primary_bits[0] = 0;

	malformed = frame;
	malformed.inversion_4d_mask =
		(uint8_t)(1u << SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME);
	CHECK(rejected_frame_is_transactional(
		      &encoder, &malformed, primary_bits,
		      malformed.primary_bits, NULL,
		      SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_INVALID_ARGUMENT),
	      "invalid inversion mask changed state or output");
	malformed = frame;
	malformed.data_bits--;
	CHECK(rejected_frame_is_transactional(
		      &encoder, &malformed, primary_bits,
		      malformed.primary_bits, NULL,
		      SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_INVALID_ARGUMENT),
	      "mismatched frame descriptor changed state or output");
	CHECK(rejected_frame_is_transactional(
		      &encoder, &frame, primary_bits, frame.primary_bits,
		      &auxiliary_bit,
		      SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_INVALID_ARGUMENT),
	      "unexpected auxiliary bit changed state or output");
	malformed = frame;
	malformed.carries_auxiliary_bit = true;
	malformed.primary_bits--;
	CHECK(rejected_frame_is_transactional(
		      &encoder, &malformed, primary_bits,
		      malformed.primary_bits, NULL,
		      SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_INVALID_ARGUMENT),
	      "missing auxiliary bit changed state or output");
	auxiliary_bit = 2;
	CHECK(rejected_frame_is_transactional(
		      &encoder, &malformed, primary_bits,
		      malformed.primary_bits, &auxiliary_bit,
		      SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_INVALID_ARGUMENT),
	      "invalid auxiliary bit changed state or output");

	invalid = encoder;
	invalid.previous_differential_output = 4;
	CHECK(rejected_frame_is_transactional(
		      &invalid, &frame, primary_bits, frame.primary_bits, NULL,
		      SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_INVALID_ARGUMENT),
	      "invalid encoder changed state or output");
	CHECK(strcmp(
		      sippymodem_v34_mapping_frame_encoder_result_name(
			      SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_OK),
		      "ok") == 0 &&
	      strcmp(
		      sippymodem_v34_mapping_frame_encoder_result_name(
			      SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_INVALID_ARGUMENT),
		      "invalid argument") == 0 &&
	      strcmp(
		      sippymodem_v34_mapping_frame_encoder_result_name(
			      SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_OUT_OF_RANGE),
		      "out of range") == 0 &&
	      strcmp(
		      sippymodem_v34_mapping_frame_encoder_result_name(
			      (enum sippymodem_v34_mapping_frame_encoder_result)3),
		      "unknown") == 0,
	      "mapping-frame result names changed");
	return true;
}

static bool test_mid_frame_rollback(void)
{
	struct sippymodem_v34_mapping_parameters parameters;
	struct sippymodem_v34_4d_encoder encoder;
	struct sippymodem_v34_4d_encoder probe;
	struct sippymodem_v34_mapping_frame frame;
	struct sippymodem_v34_parsed_mapping_frame requested = { 0 };
	struct sippymodem_v34_parsed_mapping_frame parsed;
	struct sippymodem_v34_ring_indices rings;
	struct sippymodem_v34_4d_encoder_output interval_output;
	struct sippymodem_v34_q14_coefficient coefficients
		[SIPPYMODEM_V34_PRECODER_TAP_COUNT] = { 0 };
	uint8_t primary_bits[MAX_PRIMARY_BITS];
	size_t primary_bit_count;
	uint32_t shell_label;
	uint8_t overflowing_point;

	CHECK(initialize_frame_fixture(
		      &parameters, &encoder, &frame, primary_bits) &&
	      find_delayed_overflow_inputs(
		      &parameters, &shell_label, &overflowing_point),
	      "mid-frame rollback fixture did not initialize");
	requested.shell_label = shell_label;
	requested.interval[0].differential_input = 2;
	requested.interval[1].differential_input = 2;
	requested.interval[1].point_index[0] = overflowing_point;
	CHECK(build_regular_primary_bits(
		      &parameters, &frame, &requested, primary_bits,
		      &primary_bit_count) &&
	      sippymodem_v34_parse_mapping_frame(
		      &parameters, &frame, primary_bits, primary_bit_count,
		      NULL, &parsed) == SIPPYMODEM_V34_PARSER_OK &&
	      sippymodem_v34_shell_map(
		      &parameters, parsed.shell_label, &rings) ==
		      SIPPYMODEM_V34_SHELL_MAPPER_OK,
	      "delayed-overflow frame did not compose");

	coefficients[2].real = SIPPYMODEM_V34_Q14_SCALE;
	CHECK(sippymodem_v34_4d_encoder_init(
		      &encoder, &parameters, coefficients,
		      SIPPYMODEM_V34_TRELLIS_16_STATE) ==
		      SIPPYMODEM_V34_4D_ENCODER_OK,
	      "overflow encoder did not initialize");
	for (size_t tap = 0; tap < SIPPYMODEM_V34_PRECODER_TAP_COUNT;
	     ++tap) {
		encoder.precoder.previous_symbols[tap] =
			(struct sippymodem_v34_q7_point){
				254 * SIPPYMODEM_V34_Q7_SCALE,
				254 * SIPPYMODEM_V34_Q7_SCALE,
			};
	}
	CHECK(sippymodem_v34_4d_encoder_is_valid(&encoder),
	      "overflow encoder state is invalid");
	probe = encoder;
	for (uint8_t interval = 0; interval < 2; ++interval) {
		const struct sippymodem_v34_4d_encoder_input input = {
			.mapping = parsed.interval[interval],
			.ring_index = {
				rings.index[interval][0],
				rings.index[interval][1],
			},
		};
		enum sippymodem_v34_4d_encoder_result expected = interval == 0 ?
			SIPPYMODEM_V34_4D_ENCODER_OK :
			SIPPYMODEM_V34_4D_ENCODER_OUT_OF_RANGE;

		CHECK(sippymodem_v34_4d_encoder_step(
			      &probe, &input, &interval_output) == expected,
		      "interval %u did not reach the intended boundary",
		      interval);
	}
	CHECK(!v34_test_encoders_are_equal(&probe, &encoder),
	      "successful first interval did not advance probe state");
	CHECK(rejected_frame_is_transactional(
		      &encoder, &frame, primary_bits, primary_bit_count, NULL,
		      SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_OUT_OF_RANGE),
	      "later interval failure did not roll back the whole frame");
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
		{ "transactional failures", test_transactional_failures },
		{ "mid-frame rollback", test_mid_frame_rollback },
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
