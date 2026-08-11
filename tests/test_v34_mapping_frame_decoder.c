// SPDX-License-Identifier: BSD-3-Clause

#include <v34_mapping_frame_encoder.h>
#include <v34_mapping_frame_decoder.h>

#include "test.h"

#include <string.h>

static const struct sippymodem_v34_q14_coefficient ZERO_COEFFICIENTS[] = {
	{ 0, 0 },
	{ 0, 0 },
	{ 0, 0 },
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

static void generate_primary_bits(
	uint8_t bits[SIPPYMODEM_V34_MAX_MAPPING_FRAME_BITS], uint8_t count,
	uint32_t seed)
{
	uint32_t state = seed | 1u;

	for (uint8_t index = 0; index < count; ++index) {
		state = state * UINT32_C(1664525) + UINT32_C(1013904223);
		bits[index] = (uint8_t)(state >> 31);
	}
}

static void decisions_from_encoded(
	const struct sippymodem_v34_encoded_mapping_frame *encoded,
	struct sippymodem_v34_decided_4d_interval
		decided[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME])
{
	for (uint8_t interval = 0;
	     interval < SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME;
	     ++interval) {
		decided[interval].trellis_output =
			encoded->interval[interval].trellis_output;
		for (uint8_t symbol = 0;
		     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL;
		     ++symbol)
			decided[interval].mapper_output[symbol] =
				encoded->interval[interval]
					.mapped[symbol]
					.mapper_output;
	}
}

static bool recovered_matches(
	const struct sippymodem_v34_recovered_mapping_frame *recovered,
	const struct sippymodem_v34_mapping_frame *frame,
	const uint8_t primary[SIPPYMODEM_V34_MAX_MAPPING_FRAME_BITS],
	uint8_t auxiliary)
{
	if (recovered->primary_bit_count != frame->primary_bits ||
	    recovered->auxiliary_bit_count !=
		    (uint8_t)frame->carries_auxiliary_bit ||
	    memcmp(recovered->primary_bits, primary, frame->primary_bits) != 0 ||
	    (frame->carries_auxiliary_bit &&
	     recovered->auxiliary_bit != auxiliary))
		return false;
	for (uint8_t index = frame->primary_bits;
	     index < SIPPYMODEM_V34_MAX_MAPPING_FRAME_BITS; ++index) {
		if (recovered->primary_bits[index] != 0)
			return false;
	}
	return true;
}

static bool test_all_supported_mapping_frames(void)
{
	bool point_width_seen[6] = { false };
	bool high_frame_seen[2] = { false };
	bool auxiliary_seen[2] = { false };
	bool inversion_seen[2] = { false };
	bool nonzero_shell_seen = false;
	size_t configuration_count = 0;
	size_t mapping_frame_count = 0;

	for (size_t symbol_index = 0;
	     symbol_index < ARRAY_LENGTH(SYMBOL_RATES); ++symbol_index) {
		const struct symbol_rate_fixture *fixture =
			&SYMBOL_RATES[symbol_index];

		for (uint8_t rate = fixture->minimum_rate;
		     rate <= fixture->maximum_rate; ++rate) {
			for (uint8_t auxiliary = 0; auxiliary < 2u; ++auxiliary) {
				struct sippymodem_v34_frame_geometry geometry;

				CHECK(sippymodem_v34_frame_geometry_init(
					      &geometry, fixture->symbol_rate, rate,
					      auxiliary != 0u) ==
					      SIPPYMODEM_V34_FRAME_SCHEDULE_OK,
				      "decoder geometry %zu/%u/%u failed",
				      symbol_index, rate, auxiliary);
				for (uint8_t expanded = 0; expanded < 2u; ++expanded) {
					struct sippymodem_v34_mapping_parameters parameters;
					struct sippymodem_v34_frame_schedule schedule;
					struct sippymodem_v34_4d_encoder encoder;
					struct sippymodem_v34_4d_decoder decoder;

					CHECK(sippymodem_v34_mapping_parameters_init(
						      &parameters, &geometry,
						      expanded != 0u) ==
						      SIPPYMODEM_V34_MAPPING_PARAMETERS_OK &&
					      sippymodem_v34_frame_schedule_init(
						      &schedule, &geometry,
						      (uint8_t)((rate + auxiliary + expanded) %
							geometry.data_frames_per_superframe)) ==
						      SIPPYMODEM_V34_FRAME_SCHEDULE_OK &&
					      sippymodem_v34_4d_encoder_init(
						      &encoder, &parameters,
						      ZERO_COEFFICIENTS,
						      SIPPYMODEM_V34_TRELLIS_16_STATE) ==
						      SIPPYMODEM_V34_4D_ENCODER_OK &&
					      sippymodem_v34_4d_decoder_init(
						      &decoder, &parameters) ==
						      SIPPYMODEM_V34_4D_DECODER_OK,
					      "decoder configuration %zu/%u/%u/%u failed",
					      symbol_index, rate, auxiliary, expanded);
					point_width_seen[parameters
							 .point_index_bits_per_2d_symbol] = true;
					configuration_count++;
					while (!sippymodem_v34_frame_schedule_is_complete(
						&schedule)) {
						struct sippymodem_v34_mapping_frame frame;
						struct sippymodem_v34_encoded_mapping_frame encoded;
						struct sippymodem_v34_decided_4d_interval
							decided[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME];
						struct sippymodem_v34_recovered_mapping_frame recovered;
						uint8_t primary[SIPPYMODEM_V34_MAX_MAPPING_FRAME_BITS] = { 0 };
						uint8_t auxiliary_bit = (uint8_t)(
							(symbol_index + rate + expanded +
							 mapping_frame_count) &
							1u);
						const uint8_t *auxiliary_pointer;

						CHECK(sippymodem_v34_frame_schedule_next(
							      &schedule, &frame) ==
							      SIPPYMODEM_V34_FRAME_SCHEDULE_OK,
						      "decoder schedule %zu/%u/%u/%u stopped",
						      symbol_index, rate, auxiliary, expanded);
						generate_primary_bits(
							primary, frame.primary_bits,
							(uint32_t)(configuration_count * 4099u +
								   frame.index));
						auxiliary_pointer = frame.carries_auxiliary_bit ?
							&auxiliary_bit :
							NULL;
						CHECK(sippymodem_v34_encode_mapping_frame(
							      &encoder, &frame, primary,
							      frame.primary_bits, auxiliary_pointer,
							      &encoded) ==
							      SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_OK,
						      "mapping frame %zu/%u/%u/%u/%u did not encode",
						      symbol_index, rate, auxiliary, expanded,
						      frame.index);
						decisions_from_encoded(&encoded, decided);
						CHECK(sippymodem_v34_decode_mapping_frame(
							      &decoder, &frame, decided, &recovered) ==
							      SIPPYMODEM_V34_MAPPING_FRAME_DECODER_OK &&
						      recovered_matches(
							      &recovered, &frame, primary,
							      auxiliary_bit) &&
						      decoder.previous_differential_output ==
							      encoder.previous_differential_output,
						      "mapping frame %zu/%u/%u/%u/%u did not round trip",
						      symbol_index, rate, auxiliary, expanded,
						      frame.index);
						high_frame_seen[frame.high_frame] = true;
						auxiliary_seen[frame.carries_auxiliary_bit] = true;
						inversion_seen[frame.inversion_4d_mask != 0] = true;
						nonzero_shell_seen |= encoded.parsed.shell_label != 0;
						mapping_frame_count++;
					}
				}
			}
		}
	}
	CHECK(configuration_count == 260u && mapping_frame_count == 3680u &&
	      high_frame_seen[0] && high_frame_seen[1] && auxiliary_seen[0] &&
	      auxiliary_seen[1] && inversion_seen[0] && inversion_seen[1] &&
	      nonzero_shell_seen,
	      "mapping-frame decoder coverage changed at %zu/%zu",
	      configuration_count, mapping_frame_count);
	for (size_t width = 0; width < ARRAY_LENGTH(point_width_seen); ++width)
		CHECK(point_width_seen[width],
		      "point-index width %zu was not recovered", width);
	return true;
}

static bool decoders_are_equal(
	const struct sippymodem_v34_4d_decoder *left,
	const struct sippymodem_v34_4d_decoder *right)
{
	return left->parameters.constellation_points ==
		       right->parameters.constellation_points &&
	       left->parameters.mapping_frame_bits ==
		       right->parameters.mapping_frame_bits &&
	       left->parameters.shell_mapper_bits ==
		       right->parameters.shell_mapper_bits &&
	       left->parameters.point_index_bits_per_2d_symbol ==
		       right->parameters.point_index_bits_per_2d_symbol &&
	       left->parameters.ring_count == right->parameters.ring_count &&
	       left->parameters.expanded_shaping ==
		       right->parameters.expanded_shaping &&
	       left->previous_differential_output ==
		       right->previous_differential_output;
}

static bool initialize_rejection_fixture(
	struct sippymodem_v34_4d_decoder *decoder,
	struct sippymodem_v34_mapping_frame *frame,
	struct sippymodem_v34_decided_4d_interval
		decided[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME])
{
	struct sippymodem_v34_frame_geometry geometry;
	struct sippymodem_v34_mapping_parameters parameters;
	struct sippymodem_v34_frame_schedule schedule;
	struct sippymodem_v34_4d_encoder encoder;
	uint8_t primary[SIPPYMODEM_V34_MAX_MAPPING_FRAME_BITS] = { 0 };

	if (sippymodem_v34_frame_geometry_init(
		    &geometry, SIPPYMODEM_V34_SYMBOL_RATE_3000, 8, true) !=
		    SIPPYMODEM_V34_FRAME_SCHEDULE_OK ||
	    sippymodem_v34_mapping_parameters_init(
		    &parameters, &geometry, false) !=
		    SIPPYMODEM_V34_MAPPING_PARAMETERS_OK ||
	    sippymodem_v34_frame_schedule_init(&schedule, &geometry, 0) !=
		    SIPPYMODEM_V34_FRAME_SCHEDULE_OK ||
	    sippymodem_v34_4d_encoder_init(
		    &encoder, &parameters, ZERO_COEFFICIENTS,
		    SIPPYMODEM_V34_TRELLIS_16_STATE) !=
		    SIPPYMODEM_V34_4D_ENCODER_OK ||
	    sippymodem_v34_4d_decoder_init(decoder, &parameters) !=
		    SIPPYMODEM_V34_4D_DECODER_OK)
		return false;
	while (!sippymodem_v34_frame_schedule_is_complete(&schedule)) {
		struct sippymodem_v34_encoded_mapping_frame encoded;
		struct sippymodem_v34_recovered_mapping_frame recovered;
		uint8_t auxiliary_bit = 1;
		const uint8_t *auxiliary_pointer;

		if (sippymodem_v34_frame_schedule_next(&schedule, frame) !=
		    SIPPYMODEM_V34_FRAME_SCHEDULE_OK)
			return false;
		generate_primary_bits(primary, frame->primary_bits, frame->index + 7u);
		auxiliary_pointer = frame->carries_auxiliary_bit ?
			&auxiliary_bit :
			NULL;
		if (sippymodem_v34_encode_mapping_frame(
			    &encoder, frame, primary, frame->primary_bits,
			    auxiliary_pointer, &encoded) !=
		    SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_OK)
			return false;
		decisions_from_encoded(&encoded, decided);
		if (frame->carries_auxiliary_bit)
			return true;
		if (sippymodem_v34_decode_mapping_frame(
			    decoder, frame, decided, &recovered) !=
		    SIPPYMODEM_V34_MAPPING_FRAME_DECODER_OK)
			return false;
	}
	return false;
}

static bool test_transactional_rejection(void)
{
	struct sippymodem_v34_4d_decoder decoder;
	struct sippymodem_v34_4d_decoder saved;
	struct sippymodem_v34_mapping_frame frame;
	struct sippymodem_v34_mapping_frame malformed_frame;
	struct sippymodem_v34_decided_4d_interval
		decided[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME];
	struct sippymodem_v34_decided_4d_interval malformed[4];
	struct sippymodem_v34_recovered_mapping_frame recovered;
	struct sippymodem_v34_recovered_mapping_frame recovered_saved;

	CHECK(initialize_rejection_fixture(&decoder, &frame, decided),
	      "mapping-frame rejection fixture did not initialize");
	saved = decoder;
	memset(&recovered, 0xa5, sizeof(recovered));
	recovered_saved = recovered;
	memcpy(malformed, decided, sizeof(malformed));
	malformed[3].trellis_output = 2;
	CHECK(sippymodem_v34_decode_mapping_frame(
		      &decoder, &frame, malformed, &recovered) ==
		      SIPPYMODEM_V34_MAPPING_FRAME_DECODER_INVALID_ARGUMENT &&
	      decoders_are_equal(&decoder, &saved) &&
	      memcmp(&recovered, &recovered_saved, sizeof(recovered)) == 0,
	      "late interval rejection changed decoder state or output");
	malformed_frame = frame;
	malformed_frame.data_bits--;
	CHECK(sippymodem_v34_decode_mapping_frame(
		      &decoder, &malformed_frame, decided, &recovered) ==
		      SIPPYMODEM_V34_MAPPING_FRAME_DECODER_INVALID_ARGUMENT &&
	      decoders_are_equal(&decoder, &saved) &&
	      memcmp(&recovered, &recovered_saved, sizeof(recovered)) == 0,
	      "late frame-layout rejection changed decoder state or output");
	malformed_frame = frame;
	malformed_frame.inversion_4d_mask =
		1u << SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME;
	CHECK(sippymodem_v34_decode_mapping_frame(
		      &decoder, &malformed_frame, decided, &recovered) ==
		      SIPPYMODEM_V34_MAPPING_FRAME_DECODER_INVALID_ARGUMENT &&
	      sippymodem_v34_decode_mapping_frame(
		      NULL, &frame, decided, &recovered) ==
		      SIPPYMODEM_V34_MAPPING_FRAME_DECODER_INVALID_ARGUMENT &&
	      sippymodem_v34_decode_mapping_frame(
		      &decoder, NULL, decided, &recovered) ==
		      SIPPYMODEM_V34_MAPPING_FRAME_DECODER_INVALID_ARGUMENT &&
	      sippymodem_v34_decode_mapping_frame(
		      &decoder, &frame, NULL, &recovered) ==
		      SIPPYMODEM_V34_MAPPING_FRAME_DECODER_INVALID_ARGUMENT &&
	      sippymodem_v34_decode_mapping_frame(
		      &decoder, &frame, decided, NULL) ==
		      SIPPYMODEM_V34_MAPPING_FRAME_DECODER_INVALID_ARGUMENT &&
	      decoders_are_equal(&decoder, &saved) &&
	      memcmp(&recovered, &recovered_saved, sizeof(recovered)) == 0,
	      "invalid mapping-frame arguments changed state or output");
	CHECK(strcmp(sippymodem_v34_mapping_frame_decoder_result_name(
		     SIPPYMODEM_V34_MAPPING_FRAME_DECODER_OK),
		     "ok") == 0 &&
	      strcmp(sippymodem_v34_mapping_frame_decoder_result_name(
		     SIPPYMODEM_V34_MAPPING_FRAME_DECODER_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(sippymodem_v34_mapping_frame_decoder_result_name(
		     (enum sippymodem_v34_mapping_frame_decoder_result)2),
		     "unknown") == 0,
	      "mapping-frame decoder result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "all supported mapping frames", test_all_supported_mapping_frames },
		{ "transactional rejection", test_transactional_rejection },
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
