// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_mapping_frame_transmitter.h>

#include "test.h"
#include "v34_encoder_test_helpers.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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

struct transmitter_fixture {
	struct sippymodem_v34_frame_geometry geometry;
	struct sippymodem_v34_mapping_parameters parameters;
	struct sippymodem_v34_frame_schedule schedule;
	struct sippymodem_v34_scrambler scrambler;
	struct sippymodem_v34_4d_encoder encoder;
	struct sippymodem_v34_mapping_frame mapping_frame;
	uint8_t primary_bits[SIPPYMODEM_V34_MAX_MAPPING_FRAME_BITS];
	uint8_t auxiliary_bit;
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

static bool initialize_transmitter_fixture(
	struct transmitter_fixture *fixture, bool carries_auxiliary_bit)
{
	struct transmitter_fixture generated = { 0 };
	struct sippymodem_v34_frame_schedule search;
	bool found = false;

	if (sippymodem_v34_frame_geometry_init(
		    &generated.geometry, SIPPYMODEM_V34_SYMBOL_RATE_3000, 8,
		    true) != SIPPYMODEM_V34_FRAME_SCHEDULE_OK ||
	    sippymodem_v34_mapping_parameters_init(
		    &generated.parameters, &generated.geometry, false) !=
		    SIPPYMODEM_V34_MAPPING_PARAMETERS_OK ||
	    sippymodem_v34_frame_schedule_init(
		    &generated.schedule, &generated.geometry, 3) !=
		    SIPPYMODEM_V34_FRAME_SCHEDULE_OK ||
	    sippymodem_v34_transmit_scrambler_init(
		    &generated.scrambler, SIPPYMODEM_V34_CALLING_STATION) !=
		    SIPPYMODEM_V34_FRAMING_OK ||
	    sippymodem_v34_4d_encoder_init(
		    &generated.encoder, &generated.parameters, COEFFICIENTS,
		    SIPPYMODEM_V34_TRELLIS_16_STATE) !=
		    SIPPYMODEM_V34_4D_ENCODER_OK)
		return false;
	search = generated.schedule;
	while (!sippymodem_v34_frame_schedule_is_complete(&search)) {
		struct sippymodem_v34_frame_schedule candidate = search;

		if (sippymodem_v34_frame_schedule_next(
			    &search, &generated.mapping_frame) !=
		    SIPPYMODEM_V34_FRAME_SCHEDULE_OK)
			return false;
		if (generated.mapping_frame.carries_auxiliary_bit ==
		    carries_auxiliary_bit) {
			generated.schedule = candidate;
			found = true;
			break;
		}
	}
	if (!found)
		return false;
	generate_primary_bits(
		generated.primary_bits, generated.mapping_frame.primary_bits,
		UINT32_C(0xf2a04e61));
	generated.auxiliary_bit = 1;
	*fixture = generated;
	return true;
}

static bool rejected_transmission_is_transactional(
	struct sippymodem_v34_frame_schedule *schedule,
	struct sippymodem_v34_scrambler *scrambler,
	struct sippymodem_v34_4d_encoder *encoder,
	const uint8_t *primary_bits, size_t primary_bit_count,
	const uint8_t *auxiliary_bit,
	enum sippymodem_v34_nonlinear_parameter nonlinear_parameter,
	double mean_squared_magnitude,
	enum sippymodem_v34_mapping_frame_transmitter_result expected_result)
{
	struct sippymodem_v34_frame_schedule saved_schedule = *schedule;
	struct sippymodem_v34_scrambler saved_scrambler = *scrambler;
	struct sippymodem_v34_4d_encoder saved_encoder = *encoder;
	struct sippymodem_v34_transmitted_mapping_frame transmitted;

	memset(&transmitted, 0xa5, sizeof(transmitted));
	return sippymodem_v34_transmit_mapping_frame(
		       schedule, scrambler, encoder, primary_bits,
		       primary_bit_count, auxiliary_bit, nonlinear_parameter,
		       mean_squared_magnitude, &transmitted) == expected_result &&
	       schedules_are_equal(schedule, &saved_schedule) &&
	       scramblers_are_equal(scrambler, &saved_scrambler) &&
	       v34_test_encoders_are_equal(encoder, &saved_encoder) &&
	       bytes_are_filled(&transmitted, sizeof(transmitted), 0xa5);
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

static bool test_transactional_validation(void)
{
	static const double MEAN_SQUARED_MAGNITUDE = 512.0;
	struct transmitter_fixture fixture;
	struct transmitter_fixture auxiliary_fixture;
	struct sippymodem_v34_frame_schedule saved_schedule;
	struct sippymodem_v34_frame_schedule invalid_schedule;
	struct sippymodem_v34_scrambler saved_scrambler;
	struct sippymodem_v34_scrambler invalid_scrambler;
	struct sippymodem_v34_4d_encoder saved_encoder;
	struct sippymodem_v34_4d_encoder invalid_encoder;
	struct sippymodem_v34_4d_encoder mismatched_encoder;
	struct sippymodem_v34_frame_geometry mismatched_geometry;
	struct sippymodem_v34_mapping_parameters mismatched_parameters;
	struct sippymodem_v34_transmitted_mapping_frame transmitted;
	uint8_t malformed_primary[SIPPYMODEM_V34_MAX_MAPPING_FRAME_BITS];
	uint8_t invalid_auxiliary = 2;

	CHECK(initialize_transmitter_fixture(&fixture, false) &&
	      initialize_transmitter_fixture(&auxiliary_fixture, true),
	      "transactional fixtures did not initialize");

	saved_scrambler = fixture.scrambler;
	saved_encoder = fixture.encoder;
	memset(&transmitted, 0xa5, sizeof(transmitted));
	CHECK(sippymodem_v34_transmit_mapping_frame(
		      NULL, &fixture.scrambler, &fixture.encoder,
		      fixture.primary_bits, fixture.mapping_frame.primary_bits,
		      NULL, SIPPYMODEM_V34_NONLINEAR_THETA_ZERO,
		      MEAN_SQUARED_MAGNITUDE, &transmitted) ==
		      SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT &&
	      scramblers_are_equal(&fixture.scrambler, &saved_scrambler) &&
	      v34_test_encoders_are_equal(&fixture.encoder, &saved_encoder) &&
	      bytes_are_filled(&transmitted, sizeof(transmitted), 0xa5),
	      "null schedule changed state or output");

	saved_schedule = fixture.schedule;
	saved_encoder = fixture.encoder;
	memset(&transmitted, 0xa5, sizeof(transmitted));
	CHECK(sippymodem_v34_transmit_mapping_frame(
		      &fixture.schedule, NULL, &fixture.encoder,
		      fixture.primary_bits, fixture.mapping_frame.primary_bits,
		      NULL, SIPPYMODEM_V34_NONLINEAR_THETA_ZERO,
		      MEAN_SQUARED_MAGNITUDE, &transmitted) ==
		      SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT &&
	      schedules_are_equal(&fixture.schedule, &saved_schedule) &&
	      v34_test_encoders_are_equal(&fixture.encoder, &saved_encoder) &&
	      bytes_are_filled(&transmitted, sizeof(transmitted), 0xa5),
	      "null scrambler changed state or output");

	saved_schedule = fixture.schedule;
	saved_scrambler = fixture.scrambler;
	memset(&transmitted, 0xa5, sizeof(transmitted));
	CHECK(sippymodem_v34_transmit_mapping_frame(
		      &fixture.schedule, &fixture.scrambler, NULL,
		      fixture.primary_bits, fixture.mapping_frame.primary_bits,
		      NULL, SIPPYMODEM_V34_NONLINEAR_THETA_ZERO,
		      MEAN_SQUARED_MAGNITUDE, &transmitted) ==
		      SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT &&
	      schedules_are_equal(&fixture.schedule, &saved_schedule) &&
	      scramblers_are_equal(&fixture.scrambler, &saved_scrambler) &&
	      bytes_are_filled(&transmitted, sizeof(transmitted), 0xa5),
	      "null encoder changed state or output");

	saved_schedule = fixture.schedule;
	saved_scrambler = fixture.scrambler;
	saved_encoder = fixture.encoder;
	CHECK(sippymodem_v34_transmit_mapping_frame(
		      &fixture.schedule, &fixture.scrambler, &fixture.encoder,
		      fixture.primary_bits, fixture.mapping_frame.primary_bits,
		      NULL, SIPPYMODEM_V34_NONLINEAR_THETA_ZERO,
		      MEAN_SQUARED_MAGNITUDE, NULL) ==
		      SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT &&
	      schedules_are_equal(&fixture.schedule, &saved_schedule) &&
	      scramblers_are_equal(&fixture.scrambler, &saved_scrambler) &&
	      v34_test_encoders_are_equal(&fixture.encoder, &saved_encoder),
	      "null destination changed state");

	invalid_schedule = fixture.schedule;
	invalid_schedule.next_mapping_frame = (uint8_t)(
		invalid_schedule.geometry.mapping_frames_per_data_frame + 1u);
	CHECK(rejected_transmission_is_transactional(
		      &invalid_schedule, &fixture.scrambler, &fixture.encoder,
		      fixture.primary_bits, fixture.mapping_frame.primary_bits,
		      NULL, SIPPYMODEM_V34_NONLINEAR_THETA_ZERO,
		      MEAN_SQUARED_MAGNITUDE,
		      SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT),
	      "invalid schedule changed state or output");
	invalid_scrambler = fixture.scrambler;
	invalid_scrambler.polynomial =
		(enum sippymodem_v34_scrambler_polynomial)2;
	CHECK(rejected_transmission_is_transactional(
		      &fixture.schedule, &invalid_scrambler, &fixture.encoder,
		      fixture.primary_bits, fixture.mapping_frame.primary_bits,
		      NULL, SIPPYMODEM_V34_NONLINEAR_THETA_ZERO,
		      MEAN_SQUARED_MAGNITUDE,
		      SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT),
	      "invalid scrambler changed state or output");
	invalid_encoder = fixture.encoder;
	invalid_encoder.previous_differential_output = 4;
	CHECK(rejected_transmission_is_transactional(
		      &fixture.schedule, &fixture.scrambler, &invalid_encoder,
		      fixture.primary_bits, fixture.mapping_frame.primary_bits,
		      NULL, SIPPYMODEM_V34_NONLINEAR_THETA_ZERO,
		      MEAN_SQUARED_MAGNITUDE,
		      SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT),
	      "invalid encoder changed state or output");
	CHECK(sippymodem_v34_frame_geometry_init(
		      &mismatched_geometry, SIPPYMODEM_V34_SYMBOL_RATE_3000, 7,
		      true) == SIPPYMODEM_V34_FRAME_SCHEDULE_OK &&
	      sippymodem_v34_mapping_parameters_init(
		      &mismatched_parameters, &mismatched_geometry, false) ==
		      SIPPYMODEM_V34_MAPPING_PARAMETERS_OK &&
	      sippymodem_v34_4d_encoder_init(
		      &mismatched_encoder, &mismatched_parameters, COEFFICIENTS,
		      SIPPYMODEM_V34_TRELLIS_16_STATE) ==
		      SIPPYMODEM_V34_4D_ENCODER_OK &&
	      rejected_transmission_is_transactional(
		      &fixture.schedule, &fixture.scrambler,
		      &mismatched_encoder, fixture.primary_bits,
		      fixture.mapping_frame.primary_bits, NULL,
		      SIPPYMODEM_V34_NONLINEAR_THETA_ZERO,
		      MEAN_SQUARED_MAGNITUDE,
		      SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT),
	      "mismatched frame geometry changed state or output");

	CHECK(rejected_transmission_is_transactional(
		      &fixture.schedule, &fixture.scrambler, &fixture.encoder,
		      NULL, fixture.mapping_frame.primary_bits, NULL,
		      SIPPYMODEM_V34_NONLINEAR_THETA_ZERO,
		      MEAN_SQUARED_MAGNITUDE,
		      SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT) &&
	      rejected_transmission_is_transactional(
		      &fixture.schedule, &fixture.scrambler, &fixture.encoder,
		      fixture.primary_bits,
		      (size_t)fixture.mapping_frame.primary_bits - 1u, NULL,
		      SIPPYMODEM_V34_NONLINEAR_THETA_ZERO,
		      MEAN_SQUARED_MAGNITUDE,
		      SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT),
	      "invalid primary buffer changed state or output");
	memcpy(
		malformed_primary, fixture.primary_bits,
		fixture.mapping_frame.primary_bits);
	malformed_primary[fixture.mapping_frame.primary_bits / 2u] = 2;
	CHECK(rejected_transmission_is_transactional(
		      &fixture.schedule, &fixture.scrambler, &fixture.encoder,
		      malformed_primary, fixture.mapping_frame.primary_bits, NULL,
		      SIPPYMODEM_V34_NONLINEAR_THETA_ZERO,
		      MEAN_SQUARED_MAGNITUDE,
		      SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT),
	      "partly scrambled invalid input changed state or output");
	CHECK(rejected_transmission_is_transactional(
		      &fixture.schedule, &fixture.scrambler, &fixture.encoder,
		      fixture.primary_bits, fixture.mapping_frame.primary_bits,
		      &fixture.auxiliary_bit,
		      SIPPYMODEM_V34_NONLINEAR_THETA_ZERO,
		      MEAN_SQUARED_MAGNITUDE,
		      SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT),
	      "unexpected auxiliary bit changed state or output");
	CHECK(rejected_transmission_is_transactional(
		      &auxiliary_fixture.schedule, &auxiliary_fixture.scrambler,
		      &auxiliary_fixture.encoder, auxiliary_fixture.primary_bits,
		      auxiliary_fixture.mapping_frame.primary_bits, NULL,
		      SIPPYMODEM_V34_NONLINEAR_THETA_ZERO,
		      MEAN_SQUARED_MAGNITUDE,
		      SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT) &&
	      rejected_transmission_is_transactional(
		      &auxiliary_fixture.schedule, &auxiliary_fixture.scrambler,
		      &auxiliary_fixture.encoder, auxiliary_fixture.primary_bits,
		      auxiliary_fixture.mapping_frame.primary_bits,
		      &invalid_auxiliary,
		      SIPPYMODEM_V34_NONLINEAR_THETA_ZERO,
		      MEAN_SQUARED_MAGNITUDE,
		      SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT),
	      "invalid auxiliary input changed state or output");
	CHECK(rejected_transmission_is_transactional(
		      &fixture.schedule, &fixture.scrambler, &fixture.encoder,
		      fixture.primary_bits, fixture.mapping_frame.primary_bits,
		      NULL, (enum sippymodem_v34_nonlinear_parameter)2,
		      MEAN_SQUARED_MAGNITUDE,
		      SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT) &&
	      rejected_transmission_is_transactional(
		      &fixture.schedule, &fixture.scrambler, &fixture.encoder,
		      fixture.primary_bits, fixture.mapping_frame.primary_bits,
		      NULL, SIPPYMODEM_V34_NONLINEAR_THETA_ZERO, 0.0,
		      SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT) &&
	      rejected_transmission_is_transactional(
		      &fixture.schedule, &fixture.scrambler, &fixture.encoder,
		      fixture.primary_bits, fixture.mapping_frame.primary_bits,
		      NULL, SIPPYMODEM_V34_NONLINEAR_THETA_ZERO, NAN,
		      SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT) &&
	      rejected_transmission_is_transactional(
		      &fixture.schedule, &fixture.scrambler, &fixture.encoder,
		      fixture.primary_bits, fixture.mapping_frame.primary_bits,
		      NULL, SIPPYMODEM_V34_NONLINEAR_THETA_ZERO, INFINITY,
		      SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT),
	      "invalid nonlinear configuration changed state or output");
	return true;
}

static bool test_completion_and_result_names(void)
{
	struct transmitter_fixture fixture;

	CHECK(initialize_transmitter_fixture(&fixture, false),
	      "completion fixture did not initialize");
	fixture.schedule.next_mapping_frame =
		fixture.schedule.geometry.mapping_frames_per_data_frame;
	CHECK(sippymodem_v34_frame_schedule_is_complete(&fixture.schedule) &&
	      rejected_transmission_is_transactional(
		      &fixture.schedule, &fixture.scrambler, &fixture.encoder,
		      NULL, 0, NULL, SIPPYMODEM_V34_NONLINEAR_THETA_ZERO, 1.0,
		      SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_FRAME_COMPLETE),
	      "completed transmission changed state or output");
	CHECK(strcmp(
		      sippymodem_v34_mapping_frame_transmitter_result_name(
			      SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_OK),
		      "ok") == 0 &&
	      strcmp(
		      sippymodem_v34_mapping_frame_transmitter_result_name(
			      SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_FRAME_COMPLETE),
		      "frame complete") == 0 &&
	      strcmp(
		      sippymodem_v34_mapping_frame_transmitter_result_name(
			      SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT),
		      "invalid argument") == 0 &&
	      strcmp(
		      sippymodem_v34_mapping_frame_transmitter_result_name(
			      SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_OUT_OF_RANGE),
		      "out of range") == 0 &&
	      strcmp(
		      sippymodem_v34_mapping_frame_transmitter_result_name(
			      SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_NUMERIC_FAILURE),
		      "numeric failure") == 0 &&
	      strcmp(
		      sippymodem_v34_mapping_frame_transmitter_result_name(
			      (enum sippymodem_v34_mapping_frame_transmitter_result)5),
		      "unknown") == 0,
	      "mapping-frame transmitter result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "composed transmission", test_composition },
		{ "transactional validation", test_transactional_validation },
		{ "completion and result names",
		  test_completion_and_result_names },
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
