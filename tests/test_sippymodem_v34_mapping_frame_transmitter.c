// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_mapping_frame_transmitter.h>

#include "test.h"
#include "v34_encoder_test_helpers.h"

#include <stddef.h>
#include <stdint.h>

static const struct sippymodem_v34_q14_coefficient COEFFICIENTS[] = {
	{ 4096, 2048 },
	{ -2048, 1024 },
	{ 1024, -2048 },
};

static bool geometries_are_equal(
	const struct sippymodem_v34_frame_geometry *left,
	const struct sippymodem_v34_frame_geometry *right)
{
	return left->symbol_rate == right->symbol_rate &&
	       left->data_bits_per_frame == right->data_bits_per_frame &&
	       left->primary_data_rate_2400_bps_units ==
		       right->primary_data_rate_2400_bps_units &&
	       left->data_frames_per_superframe ==
		       right->data_frames_per_superframe &&
	       left->mapping_frames_per_data_frame ==
		       right->mapping_frames_per_data_frame &&
	       left->high_mapping_frame_bits ==
		       right->high_mapping_frame_bits &&
	       left->high_mapping_frame_count ==
		       right->high_mapping_frame_count &&
	       left->auxiliary_bits_per_frame ==
		       right->auxiliary_bits_per_frame &&
	       left->auxiliary_channel_enabled ==
		       right->auxiliary_channel_enabled;
}

static bool schedules_are_equal(
	const struct sippymodem_v34_frame_schedule *left,
	const struct sippymodem_v34_frame_schedule *right)
{
	return geometries_are_equal(&left->geometry, &right->geometry) &&
	       left->data_frame_in_superframe ==
		       right->data_frame_in_superframe &&
	       left->next_mapping_frame == right->next_mapping_frame;
}

static bool scramblers_are_equal(
	const struct sippymodem_v34_scrambler *left,
	const struct sippymodem_v34_scrambler *right)
{
	return left->history == right->history &&
	       left->polynomial == right->polynomial;
}

static bool mapping_frames_are_equal(
	const struct sippymodem_v34_mapping_frame *left,
	const struct sippymodem_v34_mapping_frame *right)
{
	return left->index == right->index &&
	       left->data_bits == right->data_bits &&
	       left->primary_bits == right->primary_bits &&
	       left->inversion_4d_mask == right->inversion_4d_mask &&
	       left->high_frame == right->high_frame &&
	       left->carries_auxiliary_bit == right->carries_auxiliary_bit &&
	       left->completes_data_frame == right->completes_data_frame;
}

static bool transmitted_frames_are_equal(
	const struct sippymodem_v34_transmitted_mapping_frame *left,
	const struct sippymodem_v34_transmitted_mapping_frame *right)
{
	if (!mapping_frames_are_equal(
		    &left->mapping_frame, &right->mapping_frame) ||
	    !v34_test_encoded_frames_are_equal(
		    &left->encoded, &right->encoded))
		return false;
	for (size_t symbol = 0;
	     symbol < SIPPYMODEM_V34_SYMBOLS_PER_MAPPING_FRAME; ++symbol) {
		if (left->projected_symbol[symbol].in_phase !=
			    right->projected_symbol[symbol].in_phase ||
		    left->projected_symbol[symbol].quadrature !=
			    right->projected_symbol[symbol].quadrature)
			return false;
	}
	return true;
}

static bool model_transmit_mapping_frame(
	struct sippymodem_v34_frame_schedule *schedule,
	struct sippymodem_v34_scrambler *scrambler,
	struct sippymodem_v34_4d_encoder *encoder,
	const uint8_t *primary_bits, size_t primary_bit_count,
	const uint8_t *auxiliary_bit,
	enum sippymodem_v34_nonlinear_parameter nonlinear_parameter,
	double mean_squared_magnitude,
	struct sippymodem_v34_transmitted_mapping_frame *transmitted)
{
	struct sippymodem_v34_frame_schedule next_schedule = *schedule;
	struct sippymodem_v34_scrambler next_scrambler = *scrambler;
	struct sippymodem_v34_4d_encoder next_encoder = *encoder;
	struct sippymodem_v34_transmitted_mapping_frame generated = { 0 };
	uint8_t scrambled[SIPPYMODEM_V34_MAX_MAPPING_FRAME_BITS];

	if (sippymodem_v34_frame_schedule_next(
		    &next_schedule, &generated.mapping_frame) !=
		    SIPPYMODEM_V34_FRAME_SCHEDULE_OK ||
	    primary_bit_count != generated.mapping_frame.primary_bits)
		return false;
	for (size_t index = 0; index < primary_bit_count; ++index) {
		if (sippymodem_v34_scramble_group(
			    &next_scrambler, primary_bits[index], 1,
			    &scrambled[index]) != SIPPYMODEM_V34_FRAMING_OK)
			return false;
	}
	if (sippymodem_v34_encode_mapping_frame(
		    &next_encoder, &generated.mapping_frame, scrambled,
		    primary_bit_count, auxiliary_bit, &generated.encoded) !=
	    SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_OK)
		return false;
	for (size_t interval = 0;
	     interval < SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME;
	     ++interval) {
		for (size_t symbol = 0;
		     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL;
		     ++symbol) {
			size_t index =
				(size_t)interval *
					SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL +
				symbol;

			if (sippymodem_v34_nonlinear_encode(
				    nonlinear_parameter, mean_squared_magnitude,
				    &generated.encoded.interval[interval]
					     .precoded[symbol]
					     .precoded_symbol,
				    &generated.projected_symbol[index]) !=
			    SIPPYMODEM_V34_NONLINEAR_ENCODER_OK)
				return false;
		}
	}
	*schedule = next_schedule;
	*scrambler = next_scrambler;
	*encoder = next_encoder;
	*transmitted = generated;
	return true;
}

static void generate_primary_bits(
	uint8_t bits[SIPPYMODEM_V34_MAX_MAPPING_FRAME_BITS], size_t count,
	uint32_t seed)
{
	uint32_t state = seed | 1u;

	for (size_t index = 0; index < count; ++index) {
		state = state * UINT32_C(1664525) + UINT32_C(1013904223);
		bits[index] = (uint8_t)(state >> 31);
	}
}

static bool test_composition(void)
{
	static const enum sippymodem_v34_station_role ROLES[] = {
		SIPPYMODEM_V34_CALLING_STATION,
		SIPPYMODEM_V34_ANSWERING_STATION,
	};
	static const enum sippymodem_v34_nonlinear_parameter NONLINEAR[] = {
		SIPPYMODEM_V34_NONLINEAR_THETA_ZERO,
		SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16,
	};
	static const enum sippymodem_v34_trellis_selection TRELLISES[] = {
		SIPPYMODEM_V34_TRELLIS_16_STATE,
		SIPPYMODEM_V34_TRELLIS_32_STATE,
		SIPPYMODEM_V34_TRELLIS_64_STATE,
	};
	static const double MEAN_SQUARED_MAGNITUDE = 512.0;
	struct sippymodem_v34_frame_geometry geometry;
	struct sippymodem_v34_mapping_parameters parameters;
	bool role_seen[ARRAY_LENGTH(ROLES)] = { false };
	bool nonlinear_seen[ARRAY_LENGTH(NONLINEAR)] = { false };
	bool trellis_seen[ARRAY_LENGTH(TRELLISES)] = { false };
	bool high_frame_seen[2] = { false };
	bool auxiliary_frame_seen[2] = { false };
	bool inversion_seen[2] = { false };
	bool nonzero_symbol_seen = false;
	bool nonlinear_effect_seen = false;
	size_t configuration_count = 0;
	size_t mapping_frame_count = 0;
	size_t symbol_count = 0;

	CHECK(sippymodem_v34_frame_geometry_init(
		      &geometry, SIPPYMODEM_V34_SYMBOL_RATE_3000, 8, true) ==
		      SIPPYMODEM_V34_FRAME_SCHEDULE_OK &&
	      sippymodem_v34_mapping_parameters_init(
		      &parameters, &geometry, false) ==
		      SIPPYMODEM_V34_MAPPING_PARAMETERS_OK,
	      "mapping-frame transmitter geometry did not initialize");
	for (size_t role_index = 0; role_index < ARRAY_LENGTH(ROLES);
	     ++role_index) {
		for (size_t nonlinear_index = 0;
		     nonlinear_index < ARRAY_LENGTH(NONLINEAR);
		     ++nonlinear_index) {
			for (size_t trellis_index = 0;
			     trellis_index < ARRAY_LENGTH(TRELLISES);
			     ++trellis_index) {
				struct sippymodem_v34_frame_schedule actual_schedule;
				struct sippymodem_v34_frame_schedule model_schedule;
				struct sippymodem_v34_scrambler actual_scrambler;
				struct sippymodem_v34_scrambler model_scrambler;
				struct sippymodem_v34_4d_encoder actual_encoder;
				struct sippymodem_v34_4d_encoder model_encoder;
				uint8_t data_frame = (uint8_t)(
					(role_index * 6u + nonlinear_index * 3u +
					 trellis_index) %
					geometry.data_frames_per_superframe);

				CHECK(sippymodem_v34_frame_schedule_init(
					      &actual_schedule, &geometry, data_frame) ==
					      SIPPYMODEM_V34_FRAME_SCHEDULE_OK &&
				      sippymodem_v34_transmit_scrambler_init(
					      &actual_scrambler, ROLES[role_index]) ==
					      SIPPYMODEM_V34_FRAMING_OK &&
				      sippymodem_v34_4d_encoder_init(
					      &actual_encoder, &parameters, COEFFICIENTS,
					      TRELLISES[trellis_index]) ==
					      SIPPYMODEM_V34_4D_ENCODER_OK,
				      "configuration %zu/%zu/%zu did not initialize",
				      role_index, nonlinear_index, trellis_index);
				model_schedule = actual_schedule;
				model_scrambler = actual_scrambler;
				model_encoder = actual_encoder;
				configuration_count++;

				while (!sippymodem_v34_frame_schedule_is_complete(
					&model_schedule)) {
					struct sippymodem_v34_frame_schedule preview =
						model_schedule;
					struct sippymodem_v34_mapping_frame next_frame;
					struct sippymodem_v34_transmitted_mapping_frame
						expected;
					struct sippymodem_v34_transmitted_mapping_frame actual;
					uint8_t primary_bits[
						SIPPYMODEM_V34_MAX_MAPPING_FRAME_BITS];
					uint8_t auxiliary_bit = (uint8_t)(
						(mapping_frame_count + role_index +
						 trellis_index) &
						1u);
					const uint8_t *auxiliary_pointer;

					CHECK(sippymodem_v34_frame_schedule_next(
						      &preview, &next_frame) ==
						      SIPPYMODEM_V34_FRAME_SCHEDULE_OK,
					      "configuration %zu/%zu/%zu ended early",
					      role_index, nonlinear_index, trellis_index);
					generate_primary_bits(
						primary_bits, next_frame.primary_bits,
						UINT32_C(0x64ec105d) ^
							(uint32_t)mapping_frame_count);
					auxiliary_pointer =
						next_frame.carries_auxiliary_bit ?
							&auxiliary_bit :
							NULL;
					CHECK(model_transmit_mapping_frame(
						      &model_schedule, &model_scrambler,
						      &model_encoder, primary_bits,
						      next_frame.primary_bits,
						      auxiliary_pointer,
						      NONLINEAR[nonlinear_index],
						      MEAN_SQUARED_MAGNITUDE, &expected) &&
					      sippymodem_v34_transmit_mapping_frame(
						      &actual_schedule, &actual_scrambler,
						      &actual_encoder, primary_bits,
						      next_frame.primary_bits,
						      auxiliary_pointer,
						      NONLINEAR[nonlinear_index],
						      MEAN_SQUARED_MAGNITUDE, &actual) ==
						      SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_OK &&
					      transmitted_frames_are_equal(
						      &actual, &expected) &&
					      schedules_are_equal(
						      &actual_schedule, &model_schedule) &&
					      scramblers_are_equal(
						      &actual_scrambler, &model_scrambler) &&
					      v34_test_encoders_are_equal(
						      &actual_encoder, &model_encoder),
					      "mapping frame %zu/%zu/%zu/%u changed",
					      role_index, nonlinear_index, trellis_index,
					      next_frame.index);

					high_frame_seen[actual.mapping_frame.high_frame] =
						true;
					auxiliary_frame_seen
						[actual.mapping_frame.carries_auxiliary_bit] =
						true;
					inversion_seen
						[actual.mapping_frame.inversion_4d_mask != 0] =
						true;
					for (size_t symbol = 0;
					     symbol <
						     SIPPYMODEM_V34_SYMBOLS_PER_MAPPING_FRAME;
					     ++symbol) {
						const struct sippymodem_v34_q7_point *raw =
							&actual.encoded
								 .interval[symbol /
									   SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL]
								 .precoded[symbol %
									  SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL]
								 .precoded_symbol;
						double raw_in_phase =
							(double)raw->in_phase /
							SIPPYMODEM_V34_Q7_SCALE;
						double raw_quadrature =
							(double)raw->quadrature /
							SIPPYMODEM_V34_Q7_SCALE;

						nonzero_symbol_seen |=
							raw->in_phase != 0 ||
							raw->quadrature != 0;
						if (NONLINEAR[nonlinear_index] ==
							    SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16 &&
						    (actual.projected_symbol[symbol].in_phase !=
							     raw_in_phase ||
						     actual.projected_symbol[symbol].quadrature !=
							     raw_quadrature))
							nonlinear_effect_seen = true;
					}
					mapping_frame_count++;
					symbol_count +=
						SIPPYMODEM_V34_SYMBOLS_PER_MAPPING_FRAME;
				}
				CHECK(sippymodem_v34_frame_schedule_is_complete(
					      &actual_schedule),
				      "actual schedule %zu/%zu/%zu did not complete",
				      role_index, nonlinear_index, trellis_index);
				role_seen[role_index] = true;
				nonlinear_seen[nonlinear_index] = true;
				trellis_seen[trellis_index] = true;
			}
		}
	}
	CHECK(configuration_count == 12u && mapping_frame_count == 180u &&
	      symbol_count == 1440u && role_seen[0] && role_seen[1] &&
	      nonlinear_seen[0] && nonlinear_seen[1] && trellis_seen[0] &&
	      trellis_seen[1] && trellis_seen[2] && high_frame_seen[0] &&
	      high_frame_seen[1] && auxiliary_frame_seen[0] &&
	      auxiliary_frame_seen[1] && inversion_seen[0] &&
	      inversion_seen[1] && nonzero_symbol_seen &&
	      nonlinear_effect_seen,
	      "mapping-frame transmitter sweep missed a boundary");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "composed transmission", test_composition },
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
