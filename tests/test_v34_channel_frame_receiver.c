// SPDX-License-Identifier: BSD-3-Clause

#include <v34_mapping_frame_transmitter.h>
#include <v34_symbol_power.h>
#include <v34_data_channel_receiver.h>
#include <v34_data_equalizer_receiver.h>

#include "test.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	PIPELINE_MAPPING_FRAMES =
		V34_SURVIVOR_TRACEBACK_INTERVALS /
		V34_4D_INTERVALS_PER_MAPPING_FRAME,
	MAX_GENERATED_MAPPING_FRAMES = 48,
};

static const struct v34_q14_coefficient COEFFICIENTS[] = {
	{ 4096, 2048 },
	{ -2048, 1024 },
	{ 1024, -2048 },
};

struct configuration {
	enum v34_symbol_rate symbol_rate;
	uint8_t primary_rate;
	bool auxiliary;
	bool expanded;
	enum v34_station_role transmitting_role;
	enum v34_trellis_selection trellis;
	enum v34_nonlinear_parameter nonlinear;
};

static const struct configuration CONFIGURATIONS[] = {
	{ V34_SYMBOL_RATE_3000, 8, true, false,
	  V34_CALLING_STATION, V34_TRELLIS_16_STATE,
	  V34_NONLINEAR_THETA_ZERO },
	{ V34_SYMBOL_RATE_3429, 14, false, true,
	  V34_ANSWERING_STATION, V34_TRELLIS_64_STATE,
	  V34_NONLINEAR_THETA_5_OVER_16 },
};

struct fixture {
	struct v34_frame_geometry geometry;
	struct v34_mapping_parameters parameters;
	struct v34_frame_schedule transmit_schedule;
	struct v34_scrambler scrambler;
	struct v34_4d_encoder encoder;
	struct v34_power_normalizer normalizer;
	struct v34_data_channel_receiver receiver;
};

struct expected_frame {
	struct v34_mapping_frame mapping_frame;
	uint8_t primary_bits[V34_MAX_MAPPING_FRAME_BITS];
	uint8_t auxiliary_bit;
};

static uint32_t next_generated_word(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static double generated_noise(uint32_t *state, double normalization_gain)
{
	double unit = (double)(next_generated_word(state) & UINT32_C(0xffff)) /
		      65535.0;

	/* Keep the generated impairment well inside the half-grid slicer
	 * boundary after receive scaling and nonlinear inversion. */
	return (unit - 0.5) * 0.16 * normalization_gain;
}

static bool initialize_fixture(
	struct fixture *fixture, const struct configuration *configuration)
{
	const struct v34_symbol_power_measurement precoded = {
		.mean_squared_magnitude = 25.0,
		.symbol_count = 256,
	};
	struct v34_symbol_power_measurement projected = {
		.mean_squared_magnitude = 30.0,
		.symbol_count = 256,
	};
	struct fixture generated = { 0 };

	if (configuration->nonlinear == V34_NONLINEAR_THETA_ZERO)
		projected.mean_squared_magnitude =
			precoded.mean_squared_magnitude;
	if (v34_frame_geometry_init(
		    &generated.geometry, configuration->symbol_rate,
		    configuration->primary_rate, configuration->auxiliary) !=
		    V34_FRAME_SCHEDULE_OK ||
	    v34_mapping_parameters_init(
		    &generated.parameters, &generated.geometry,
		    configuration->expanded) !=
		    V34_MAPPING_PARAMETERS_OK ||
	    v34_b1_frame_schedule_init(
		    &generated.transmit_schedule, &generated.geometry) !=
		    V34_FRAME_SCHEDULE_OK ||
	    v34_transmit_scrambler_init(
		    &generated.scrambler, configuration->transmitting_role) !=
		    V34_FRAMING_OK ||
	    v34_4d_encoder_init(
		    &generated.encoder, &generated.parameters, COEFFICIENTS,
		    configuration->trellis) != V34_4D_ENCODER_OK ||
	    v34_power_normalizer_init(
		    &generated.normalizer, &generated.parameters,
		    configuration->nonlinear, &precoded, &projected) !=
		    V34_SYMBOL_POWER_OK ||
	    v34_data_channel_receiver_init(
		    &generated.receiver, &generated.normalizer,
		    &generated.geometry, configuration->expanded, COEFFICIENTS,
		    configuration->trellis,
		    configuration->transmitting_role) !=
		    V34_DATA_RECEIVE_OK)
		return false;
	*fixture = generated;
	return true;
}

static void generate_primary_bits(
	uint8_t bits[V34_MAX_MAPPING_FRAME_BITS], size_t count,
	uint32_t seed)
{
	uint32_t state = seed | 1u;

	for (size_t index = 0; index < count; ++index)
		bits[index] = (uint8_t)(next_generated_word(&state) >> 31);
}

static bool transmit_frame(
	struct fixture *fixture, const struct configuration *configuration,
	size_t frame_index, struct expected_frame *expected,
	struct v34_transmitted_mapping_frame *frame)
{
	struct v34_frame_schedule preview;
	const uint8_t *auxiliary;
	bool b1 = frame_index <
		fixture->geometry.mapping_frames_per_data_frame;

	if (v34_frame_schedule_is_complete(
		    &fixture->transmit_schedule) &&
	    v34_frame_schedule_advance_data_frame(
		    &fixture->transmit_schedule) !=
		    V34_FRAME_SCHEDULE_OK)
		return false;
	preview = fixture->transmit_schedule;
	if (v34_frame_schedule_next(
		    &preview, &expected->mapping_frame) !=
	    V34_FRAME_SCHEDULE_OK)
		return false;
	if (b1)
		memset(expected->primary_bits, 1,
		       expected->mapping_frame.primary_bits);
	else
		generate_primary_bits(
			expected->primary_bits,
			expected->mapping_frame.primary_bits,
			UINT32_C(0xb8074f65) ^ (uint32_t)frame_index);
	expected->auxiliary_bit = b1 ? 1u : (uint8_t)(frame_index & 1u);
	auxiliary = expected->mapping_frame.carries_auxiliary_bit ?
		&expected->auxiliary_bit :
		NULL;
	return v34_transmit_mapping_frame(
		       &fixture->transmit_schedule, &fixture->scrambler,
		       &fixture->encoder, expected->primary_bits,
		       expected->mapping_frame.primary_bits,
		       auxiliary, configuration->nonlinear,
		       fixture->normalizer.precoded_mean_squared_magnitude,
		       frame) == V34_MAPPING_FRAME_TRANSMITTER_OK;
}

static bool received_matches(
	const struct v34_received_mapping_frame *received,
	const struct expected_frame *expected)
{
	const struct v34_mapping_frame *left =
		&received->mapping_frame;
	const struct v34_mapping_frame *right =
		&expected->mapping_frame;

	return left->index == right->index &&
	       left->data_bits == right->data_bits &&
	       left->primary_bits == right->primary_bits &&
	       left->inversion_4d_mask == right->inversion_4d_mask &&
	       left->high_frame == right->high_frame &&
	       left->carries_auxiliary_bit == right->carries_auxiliary_bit &&
	       left->completes_data_frame == right->completes_data_frame &&
	       received->primary_bit_count == right->primary_bits &&
	       memcmp(received->primary_bits, expected->primary_bits,
		      right->primary_bits) == 0 &&
	       received->auxiliary_bit_count ==
		       (uint8_t)right->carries_auxiliary_bit &&
	       (!right->carries_auxiliary_bit ||
		received->auxiliary_bit == expected->auxiliary_bit);
}

static bool current_decision_matches(
	const struct v34_survivor_decision *current,
	const struct v34_4d_encoder_output *encoded,
	uint8_t inversion_bit)
{
	if (current->convolutional_output != encoded->convolutional_output ||
	    current->inversion_bit != inversion_bit)
		return false;
	for (size_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		if (memcmp(&current->channel_output[symbol],
			   &encoded->precoded[symbol].channel_output,
			   sizeof(current->channel_output[symbol])) != 0)
			return false;
	}
	return true;
}

static bool point_matches(
	const struct qam_point *left,
	const struct qam_point *right)
{
	double in_phase_scale = fmax(
		1.0, fmax(fabs(left->in_phase), fabs(right->in_phase)));
	double quadrature_scale = fmax(
		1.0, fmax(fabs(left->quadrature), fabs(right->quadrature)));

	return fabs(left->in_phase - right->in_phase) <=
		       4.0 * DBL_EPSILON * in_phase_scale &&
	       fabs(left->quadrature - right->quadrature) <=
		       4.0 * DBL_EPSILON * quadrature_scale;
}

static bool precoder_histories_match(
	const struct v34_precoder *left,
	const struct v34_precoder *right)
{
	return left->grid_step == right->grid_step &&
	       memcmp(left->coefficients, right->coefficients,
		      sizeof(left->coefficients)) == 0 &&
	       memcmp(left->previous_symbols, right->previous_symbols,
		      sizeof(left->previous_symbols)) == 0;
}

static bool test_generated_channel_round_trip(void)
{
	for (size_t mode = 0; mode < ARRAY_LENGTH(CONFIGURATIONS); ++mode) {
		const struct configuration *configuration =
			&CONFIGURATIONS[mode];
		struct expected_frame expected[MAX_GENERATED_MAPPING_FRAMES];
		struct fixture fixture;
		uint32_t random = UINT32_C(0x47d25631) ^ (uint32_t)mode;
		size_t received_frames = 0;
		size_t transmitted_frames;

		CHECK(initialize_fixture(&fixture, configuration),
		      "channel receiver mode %zu did not initialize", mode);
		transmitted_frames = PIPELINE_MAPPING_FRAMES +
			fixture.geometry.mapping_frames_per_data_frame + 4u;
		CHECK(transmitted_frames <= ARRAY_LENGTH(expected),
		      "channel receiver mode %zu fixture is too small", mode);
		for (size_t frame_index = 0;
		     frame_index < transmitted_frames; ++frame_index) {
			struct v34_transmitted_mapping_frame transmitted;

			CHECK(transmit_frame(
				      &fixture, configuration, frame_index,
				      &expected[frame_index],
				      &transmitted),
			      "channel receiver mode %zu frame %zu did not transmit",
			      mode, frame_index);
			for (size_t interval = 0;
			     interval <
				     V34_4D_INTERVALS_PER_MAPPING_FRAME;
			     ++interval) {
				struct qam_point normalized
					[V34_2D_SYMBOLS_PER_4D_INTERVAL];
				struct qam_point expected_targets
					[V34_2D_SYMBOLS_PER_4D_INTERVAL];
				struct qam_point decision_targets
					[V34_2D_SYMBOLS_PER_4D_INTERVAL];
				struct v34_survivor_decision current;
				struct v34_received_mapping_frame received;
				struct v34_received_mapping_frame saved_received;
				enum v34_data_receive_result result;
				enum v34_data_receive_result expected_result;
				uint8_t inversion_bit = (uint8_t)(
					(transmitted.mapping_frame.inversion_4d_mask >>
					 interval) &
					1u);

				for (size_t symbol = 0;
				     symbol <
					     V34_2D_SYMBOLS_PER_4D_INTERVAL;
				     ++symbol) {
					size_t index = interval *
						       V34_2D_SYMBOLS_PER_4D_INTERVAL +
					       symbol;

					CHECK(v34_normalize_power(
						      &fixture.normalizer,
						      &transmitted.projected_symbol[index],
						      &normalized[symbol]) ==
						      V34_SYMBOL_POWER_OK,
						      "mode %zu frame %zu symbol %zu did not normalize",
						      mode, frame_index, index);
					expected_targets[symbol] = normalized[symbol];
					normalized[symbol].in_phase += generated_noise(
						&random,
						fixture.normalizer.normalization_gain);
					normalized[symbol].quadrature += generated_noise(
						&random,
						fixture.normalizer.normalization_gain);
				}
				memset(&received, 0xa5, sizeof(received));
				saved_received = received;
				result = v34_data_channel_receiver_advance(
					&fixture.receiver, normalized, &current,
					decision_targets, &received);
				CHECK(result <=
					      V34_DATA_RECEIVE_B1_COMPLETE &&
				      current_decision_matches(
					      &current,
					      &transmitted.encoded.interval[interval],
					      inversion_bit) &&
				      point_matches(
					      &decision_targets[0],
					      &expected_targets[0]) &&
				      point_matches(
					      &decision_targets[1],
					      &expected_targets[1]),
				      "mode %zu frame %zu interval %zu decision changed",
				      mode, frame_index, interval);
				if (result == V34_DATA_RECEIVE_OK) {
					CHECK(memcmp(&received, &saved_received,
						     sizeof(received)) == 0,
					      "channel receiver emitted a partial frame");
					continue;
				}
				expected_result =
					received_frames <
							fixture.geometry
								.mapping_frames_per_data_frame ?
						(received_frames + 1u ==
								 fixture.geometry
									 .mapping_frames_per_data_frame ?
							 V34_DATA_RECEIVE_B1_COMPLETE :
							 V34_DATA_RECEIVE_B1_IN_PROGRESS) :
						V34_DATA_RECEIVE_MAPPING_FRAME_READY;
				CHECK(received_frames < frame_index &&
					      result == expected_result &&
					      received_matches(
						      &received,
						      &expected[received_frames]),
				      "mode %zu received frame %zu changed", mode,
				      received_frames);
				received_frames++;
			}
			CHECK(precoder_histories_match(
				      &fixture.receiver.channel_receiver.receive_precoder,
				      &fixture.encoder.precoder),
			      "mode %zu frame %zu feedback history drifted", mode,
			      frame_index);
		}
		CHECK(received_frames ==
			      transmitted_frames - PIPELINE_MAPPING_FRAMES &&
		      !v34_data_channel_receiver_is_receiving_b1(
			      &fixture.receiver) &&
		      fixture.receiver.data_receiver.b1_error_bits == 0u,
		      "mode %zu emitted %zu frames", mode, received_frames);
	}
	return true;
}

static bool test_buffered_equalizer_handoff(void)
{
	static const struct qam_point reference = { 1.0, 0.0 };
	static const struct qam_point
		lookahead[V34_EQUALIZER_DECISION_DELAY + 1] = {
		{ 0.25, -0.50 },
		{ -0.75, 0.25 },
		{ 0.50, 0.75 },
		{ -0.25, -0.75 },
		{ 0.75, -0.50 },
		{ -0.50, -0.25 },
		{ 0.25, 0.50 },
		{ -0.75, 0.75 },
	};
	struct v34_data_equalizer_receiver receiver;
	struct v34_data_equalizer_receiver saved_receiver;
	struct v34_equalizer_handoff handoff;
	struct v34_equalizer_handoff invalid_handoff;
	struct qam_equalizer equalizer;
	struct fixture fixture;
	struct v34_survivor_decision current;
	struct v34_survivor_decision saved_current;
	struct v34_received_mapping_frame received;
	struct v34_received_mapping_frame saved_received;

	CHECK(initialize_fixture(&fixture, &CONFIGURATIONS[0]) &&
	      qam_equalizer_init_geometry(
		      &equalizer, &reference, &reference,
		      V34_EQUALIZER_TAP_COUNT,
		      V34_EQUALIZER_DECISION_DELAY) ==
		      QAM_EQUALIZER_OK,
	      "buffered handoff fixture did not initialize");
	for (size_t index = 0;
	     index < V34_EQUALIZER_DECISION_DELAY + 1u +
			     V34_EQUALIZER_DECISION_DELAY;
	     ++index) {
		struct qam_equalizer_observation observation = { 0 };
		const struct qam_point *point =
			index < V34_EQUALIZER_DECISION_DELAY + 1u ?
				&reference :
				&lookahead[index -
					   (V34_EQUALIZER_DECISION_DELAY + 1u)];
		bool ready;

		CHECK(qam_equalizer_observe(
			      &equalizer, point, &observation, &ready) ==
			      QAM_EQUALIZER_OK &&
		      (!ready ||
		       qam_equalizer_adapt_observation(
			       &equalizer, &observation,
			       &observation.output, 0.0) == QAM_EQUALIZER_OK),
		      "buffered handoff equalizer stopped at point %zu", index);
	}
	handoff.equalizer = equalizer;
	CHECK(v34_equalizer_handoff_is_valid(&handoff) &&
	      v34_data_equalizer_receiver_init_from_handoff(
		      &receiver, &handoff, &fixture.normalizer,
		      &fixture.geometry, CONFIGURATIONS[0].expanded,
		      COEFFICIENTS, CONFIGURATIONS[0].trellis,
		      CONFIGURATIONS[0].transmitting_role, 0.0) ==
		      V34_DATA_RECEIVE_OK,
	      "buffered equalizer handoff was rejected");
	memset(&current, 0xa5, sizeof(current));
	saved_current = current;
	memset(&received, 0x5a, sizeof(received));
	saved_received = received;
	CHECK(v34_data_equalizer_receive_point(
		      &receiver, &lookahead[V34_EQUALIZER_DECISION_DELAY],
		      &current, &received) == V34_DATA_RECEIVE_OK &&
	      qam_equalizer_observation_is_pending(
		      &receiver.first_observation) &&
	      point_matches(
		      &receiver.first_observation.output, &lookahead[0]) &&
	      memcmp(&current, &saved_current, sizeof(current)) == 0 &&
	      memcmp(&received, &saved_received, sizeof(received)) == 0,
	      "buffered handoff did not make the first B1 point next");
	saved_receiver = receiver;
	invalid_handoff = handoff;
	invalid_handoff.equalizer.fill =
		handoff.equalizer.decision_delay;
	CHECK(!v34_equalizer_handoff_is_valid(NULL) &&
	      !v34_equalizer_handoff_is_valid(&invalid_handoff) &&
	      v34_data_equalizer_receiver_init_from_handoff(
		      &receiver, &invalid_handoff, &fixture.normalizer,
		      &fixture.geometry, CONFIGURATIONS[0].expanded,
		      COEFFICIENTS, CONFIGURATIONS[0].trellis,
		      CONFIGURATIONS[0].transmitting_role, 0.0) ==
		      V34_DATA_RECEIVE_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved_receiver, sizeof(receiver)) == 0,
	      "invalid buffered handoff changed the data receiver");
	return true;
}

static bool outputs_are_unchanged_after(
	struct v34_data_channel_receiver *receiver,
	const struct qam_point
		normalized[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	enum v34_data_receive_result expected_result)
{
	struct v34_data_channel_receiver saved_receiver = *receiver;
	struct v34_survivor_decision current;
	struct v34_survivor_decision saved_current;
	struct qam_point targets
		[V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct qam_point saved_targets
		[V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct v34_received_mapping_frame received;
	struct v34_received_mapping_frame saved_received;

	memset(&current, 0xa5, sizeof(current));
	saved_current = current;
	memset(targets, 0x3c, sizeof(targets));
	memcpy(saved_targets, targets, sizeof(targets));
	memset(&received, 0x5a, sizeof(received));
	saved_received = received;
	return v34_data_channel_receiver_advance(
		       receiver, normalized, &current, targets, &received) ==
		       expected_result &&
	       memcmp(receiver, &saved_receiver, sizeof(*receiver)) == 0 &&
	       memcmp(&current, &saved_current, sizeof(current)) == 0 &&
	       memcmp(targets, saved_targets, sizeof(targets)) == 0 &&
	       memcmp(&received, &saved_received, sizeof(received)) == 0;
}

static bool test_validation_and_rollback(void)
{
	struct fixture fixture;
	struct fixture other;
	struct v34_data_channel_receiver saved_receiver;
	struct v34_q14_coefficient invalid_coefficients
		[V34_PRECODER_TAP_COUNT];
	struct qam_point invalid
		[V34_2D_SYMBOLS_PER_4D_INTERVAL] = {
			{ 0.0, 0.0 },
			{ 0.0, 0.0 },
		};
	struct v34_survivor_decision current;
	struct qam_point targets
		[V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct v34_received_mapping_frame received;

	CHECK(initialize_fixture(&fixture, &CONFIGURATIONS[0]) &&
		      initialize_fixture(&other, &CONFIGURATIONS[1]),
	      "validation fixtures did not initialize");
	saved_receiver = fixture.receiver;
	memcpy(invalid_coefficients, COEFFICIENTS,
	       sizeof(invalid_coefficients));
	invalid_coefficients[0].real = INT16_MIN;
	CHECK(v34_data_channel_receiver_init(
		      &fixture.receiver, &fixture.normalizer, &fixture.geometry,
		      CONFIGURATIONS[0].expanded, invalid_coefficients,
		      CONFIGURATIONS[0].trellis,
		      CONFIGURATIONS[0].transmitting_role) ==
		      V34_DATA_RECEIVE_INVALID_ARGUMENT &&
	      v34_data_channel_receiver_init(
		      &fixture.receiver, &fixture.normalizer, &other.geometry,
		      CONFIGURATIONS[1].expanded, COEFFICIENTS,
		      CONFIGURATIONS[1].trellis,
		      CONFIGURATIONS[1].transmitting_role) ==
		      V34_DATA_RECEIVE_INVALID_ARGUMENT &&
	      memcmp(&fixture.receiver, &saved_receiver,
		     sizeof(fixture.receiver)) == 0,
	      "invalid initialization changed the receiver");
	invalid[1].quadrature = NAN;
	CHECK(outputs_are_unchanged_after(
		      &fixture.receiver, invalid,
		      V34_DATA_RECEIVE_INVALID_ARGUMENT),
	      "non-finite input was not transactional");
	invalid[1].quadrature = 0.0;
	invalid[0].in_phase =
		((double)INT32_MAX / V34_Q7_SCALE + 2.0) *
		fixture.normalizer.normalization_gain;
	CHECK(outputs_are_unchanged_after(
		      &fixture.receiver, invalid,
		      V34_DATA_RECEIVE_OUT_OF_RANGE),
	      "unrepresentable feedback was not transactional");
	invalid[0].in_phase = DBL_MAX;
	CHECK(outputs_are_unchanged_after(
		      &fixture.receiver, invalid,
		      V34_DATA_RECEIVE_NUMERIC_FAILURE),
	      "conditioning overflow was not transactional");
	fixture.receiver.channel_receiver.receive_precoder.coefficients[0].real =
		INT16_MIN;
	CHECK(!v34_data_channel_receiver_is_valid(NULL) &&
	      !v34_data_channel_receiver_is_valid(
		      &fixture.receiver) &&
	      v34_data_channel_receiver_advance(
		      &fixture.receiver, invalid, &current, targets, &received) ==
		      V34_DATA_RECEIVE_INVALID_ARGUMENT &&
	      v34_data_channel_receiver_advance(
		      NULL, invalid, &current, targets, &received) ==
		      V34_DATA_RECEIVE_INVALID_ARGUMENT &&
	      v34_data_channel_receiver_advance(
		      &saved_receiver, NULL, &current, targets, &received) ==
		      V34_DATA_RECEIVE_INVALID_ARGUMENT &&
	      v34_data_channel_receiver_advance(
		      &saved_receiver, invalid, NULL, targets, &received) ==
		      V34_DATA_RECEIVE_INVALID_ARGUMENT &&
	      v34_data_channel_receiver_advance(
		      &saved_receiver, invalid, &current, NULL, &received) ==
		      V34_DATA_RECEIVE_INVALID_ARGUMENT &&
	      v34_data_channel_receiver_advance(
		      &saved_receiver, invalid, &current, targets, NULL) ==
		      V34_DATA_RECEIVE_INVALID_ARGUMENT,
	      "invalid receiver arguments were accepted");
	return true;
}

static bool test_equalizer_receiver_validation(void)
{
	const struct qam_point reference = { 1.0, 0.0 };
	const struct qam_point received_reference = { 0.8, -0.2 };
	const struct qam_point invalid = { NAN, 0.0 };
	struct v34_equalizer_handoff handoff;
	struct v34_equalizer_handoff invalid_handoff;
	struct v34_data_equalizer_receiver receiver;
	struct v34_data_equalizer_receiver saved_receiver;
	struct v34_data_equalizer_receiver corrupt;
	struct v34_survivor_decision current;
	struct v34_survivor_decision saved_current;
	struct v34_received_mapping_frame received;
	struct v34_received_mapping_frame saved_received;
	struct fixture fixture;

	CHECK(initialize_fixture(&fixture, &CONFIGURATIONS[0]) &&
	      qam_equalizer_init_geometry(
		      &handoff.equalizer, &received_reference, &reference,
		      V34_EQUALIZER_TAP_COUNT,
		      V34_EQUALIZER_DECISION_DELAY) == QAM_EQUALIZER_OK,
	      "equalizer validation fixture did not initialize");
	for (size_t index = 0;
	     index <= V34_EQUALIZER_DECISION_DELAY; ++index) {
		struct qam_equalizer_observation observation = { 0 };
		bool ready = false;

		CHECK(qam_equalizer_observe(
			      &handoff.equalizer, &received_reference,
			      &observation, &ready) == QAM_EQUALIZER_OK &&
		      ready == (index == V34_EQUALIZER_DECISION_DELAY),
		      "equalizer handoff stopped at point %zu", index);
	}
	CHECK(v34_equalizer_handoff_is_valid(&handoff) &&
	      v34_data_equalizer_receiver_init_from_handoff(
		      &receiver, &handoff, &fixture.normalizer,
		      &fixture.geometry, CONFIGURATIONS[0].expanded,
		      COEFFICIENTS, CONFIGURATIONS[0].trellis,
		      CONFIGURATIONS[0].transmitting_role, 0.05) ==
		      V34_DATA_RECEIVE_OK,
	      "valid equalizer handoff was rejected");
	saved_receiver = receiver;
	memset(&current, 0xa5, sizeof(current));
	saved_current = current;
	memset(&received, 0x5a, sizeof(received));
	saved_received = received;
	CHECK(v34_data_equalizer_receive_point(
		      &receiver, &invalid, &current, &received) ==
		      V34_DATA_RECEIVE_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved_receiver, sizeof(receiver)) == 0 &&
	      memcmp(&current, &saved_current, sizeof(current)) == 0 &&
	      memcmp(&received, &saved_received, sizeof(received)) == 0,
	      "invalid equalizer point was not transactional");
	invalid_handoff = handoff;
	invalid_handoff.equalizer.coefficients[0].in_phase = INFINITY;
	CHECK(v34_data_equalizer_receiver_init_from_handoff(
		      &receiver, &handoff, &fixture.normalizer,
		      &fixture.geometry, CONFIGURATIONS[0].expanded,
		      COEFFICIENTS, CONFIGURATIONS[0].trellis,
		      CONFIGURATIONS[0].transmitting_role, NAN) ==
		      V34_DATA_RECEIVE_INVALID_ARGUMENT &&
	      v34_data_equalizer_receiver_init_from_handoff(
		      &receiver, &invalid_handoff, &fixture.normalizer,
		      &fixture.geometry, CONFIGURATIONS[0].expanded, COEFFICIENTS,
		      CONFIGURATIONS[0].trellis,
		      CONFIGURATIONS[0].transmitting_role, 0.05) ==
		      V34_DATA_RECEIVE_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved_receiver, sizeof(receiver)) == 0,
	      "invalid equalizer initialization changed the receiver");
	corrupt = receiver;
	corrupt.first_observation.pending = true;
	CHECK(!v34_data_equalizer_receiver_is_valid(NULL) &&
	      !v34_data_equalizer_receiver_is_valid(&corrupt) &&
	      v34_data_equalizer_receive_point(
		      NULL, &reference, &current, &received) ==
		      V34_DATA_RECEIVE_INVALID_ARGUMENT &&
	      v34_data_equalizer_receive_point(
		      &receiver, &reference, NULL, &received) ==
		      V34_DATA_RECEIVE_INVALID_ARGUMENT &&
	      v34_data_equalizer_receive_point(
		      &receiver, &reference, &current, NULL) ==
		      V34_DATA_RECEIVE_INVALID_ARGUMENT,
	      "invalid equalizer receiver arguments were accepted");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated channel round trip",
		  test_generated_channel_round_trip },
		{ "buffered equalizer handoff",
		  test_buffered_equalizer_handoff },
		{ "validation and rollback", test_validation_and_rollback },
		{ "equalizer receiver validation",
		  test_equalizer_receiver_validation },
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
