// SPDX-License-Identifier: BSD-3-Clause

#include <v34_data_decision_receiver.h>
#include <v34_data_mapping.h>
#include <v34_mapping_frame_transmitter.h>
#include <v34_symbol_power.h>

#include "test.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	PIPELINE_MAPPING_FRAMES =
		V34_SURVIVOR_TRACEBACK_INTERVALS /
		V34_4D_INTERVALS_PER_MAPPING_FRAME,
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
	struct v34_data_mapping mapping;
	struct v34_data_decision_receiver receiver;
	struct v34_data_channel_receiver model;
	uint64_t epoch;
	uint64_t lineage;
};

static uint32_t next_generated_word(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static bool initialize_fixture(
	struct fixture *fixture, const struct configuration *configuration)
{
	struct v34_phase3_profile profile = {
		.symbol_rate_numerator = configuration->symbol_rate ==
						 V34_SYMBOL_RATE_3429 ?
					 24000u : 3000u,
		.symbol_rate_denominator = configuration->symbol_rate ==
						   V34_SYMBOL_RATE_3429 ?
					   7u : 1u,
		.carrier_frequency_numerator = configuration->symbol_rate ==
						      V34_SYMBOL_RATE_3429 ?
					      96000u : 2000u,
		.carrier_frequency_denominator = configuration->symbol_rate ==
							V34_SYMBOL_RATE_3429 ?
							49u : 1u,
		.symbol_rate = configuration->symbol_rate,
		.local_role = configuration->transmitting_role,
		.projected_data_rate_2400_bps_units =
			configuration->primary_rate,
		.high_carrier = true,
	};
	struct v34_data_direction_parameters parameters = {
		.precoder_coefficients = {
			{ 4096, 2048 },
			{ -2048, 1024 },
			{ 1024, -2048 },
		},
		.trellis_selection = configuration->trellis,
		.nonlinear_parameter = configuration->nonlinear,
		.primary_data_rate_2400_bps_units =
			configuration->primary_rate,
		.expanded_shaping = configuration->expanded,
	};
	struct fixture initialized = {
		.lineage = UINT64_C(14695981039346656037),
	};

	if (v34_data_mapping_prepare(
		    &initialized.mapping, &profile, &parameters,
		    configuration->auxiliary) != V34_DATA_MAPPING_OK ||
	    v34_data_decision_receiver_init(
		    &initialized.receiver, &initialized.mapping.normalizer,
		    &initialized.mapping.schedule.geometry,
		    configuration->expanded,
		    COEFFICIENTS, configuration->trellis,
		    configuration->transmitting_role) !=
		    V34_DATA_RECEIVE_OK)
		return false;
	initialized.model = initialized.receiver.data_receiver;
	*fixture = initialized;
	return true;
}

static bool transmit_frame(
	struct fixture *fixture, const struct configuration *configuration,
	size_t frame_index, struct v34_transmitted_mapping_frame *frame)
{
	uint8_t primary[V34_MAX_MAPPING_FRAME_BITS];
	struct v34_frame_schedule preview;
	struct v34_mapping_frame mapping_frame;
	uint8_t auxiliary_bit = (uint8_t)(frame_index & 1u);
	const uint8_t *auxiliary;
	uint32_t random = UINT32_C(0x4f8d23a1) ^ (uint32_t)frame_index;
	bool b1 = frame_index <
		fixture->mapping.schedule.geometry
			.mapping_frames_per_data_frame;

	if (v34_frame_schedule_prepare_mapping_frame(
		    &fixture->mapping.schedule) != V34_FRAME_SCHEDULE_OK)
		return false;
	preview = fixture->mapping.schedule;
	if (v34_frame_schedule_next(&preview, &mapping_frame) !=
	    V34_FRAME_SCHEDULE_OK)
		return false;
	for (size_t bit = 0; bit < mapping_frame.primary_bits; ++bit)
		primary[bit] = b1 ? 1u :
			(uint8_t)(next_generated_word(&random) >> 31);
	if (b1)
		auxiliary_bit = 1u;
	auxiliary = mapping_frame.carries_auxiliary_bit ?
		&auxiliary_bit : NULL;
	return v34_transmit_mapping_frame(
		       &fixture->mapping.schedule, &fixture->mapping.scrambler,
		       &fixture->mapping.encoder, primary,
		       mapping_frame.primary_bits,
		       auxiliary, configuration->nonlinear,
		       fixture->mapping.normalizer
			       .precoded_mean_squared_magnitude,
		       frame) == V34_MAPPING_FRAME_TRANSMITTER_OK;
}

static bool fixed_coordinate(double normalized, int16_t *fixed)
{
	double scaled = normalized * V34_DATA_DECISION_FIXED_AMPLITUDE;
	long rounded;

	if (!isfinite(scaled) || scaled < INT16_MIN || scaled > INT16_MAX)
		return false;
	rounded = lround(scaled);
	if (rounded < INT16_MIN || rounded > INT16_MAX)
		return false;
	*fixed = (int16_t)rounded;
	return true;
}

static bool make_observation(
	struct fixture *fixture, const struct qam_point *normalized,
	struct qam_point *quantized,
	struct v34_adaptive_equalizer_observation *observation)
{
	if (!fixed_coordinate(
		    normalized->in_phase, &observation->decision.in_phase) ||
	    !fixed_coordinate(
		    normalized->quadrature,
		    &observation->decision.quadrature))
		return false;
	quantized->in_phase = observation->decision.in_phase /
		(double)V34_DATA_DECISION_FIXED_AMPLITUDE;
	quantized->quadrature = observation->decision.quadrature /
		(double)V34_DATA_DECISION_FIXED_AMPLITUDE;
	fixture->epoch++;
	fixture->lineage =
		(fixture->lineage ^
		 (uint16_t)observation->decision.in_phase ^
		 ((uint64_t)(uint16_t)observation->decision.quadrature << 16u)) *
		UINT64_C(1099511628211);
	if (fixture->lineage == 0u)
		fixture->lineage = UINT64_C(14695981039346656037);
	observation->epoch = fixture->epoch;
	observation->lineage = fixture->lineage;
	return true;
}

static bool observations_match(
	const struct v34_adaptive_equalizer_observation *left,
	const struct v34_adaptive_equalizer_observation *right)
{
	return left->decision.in_phase == right->decision.in_phase &&
	       left->decision.quadrature == right->decision.quadrature &&
	       left->epoch == right->epoch && left->lineage == right->lineage;
}

static bool current_matches(
	const struct v34_survivor_decision *left,
	const struct v34_survivor_decision *right)
{
	return memcmp(left->channel_output, right->channel_output,
		      sizeof(left->channel_output)) == 0 &&
	       left->convolutional_output == right->convolutional_output &&
	       left->inversion_bit == right->inversion_bit;
}

static bool received_matches(
	const struct v34_received_mapping_frame *left,
	const struct v34_received_mapping_frame *right)
{
	return left->mapping_frame.index == right->mapping_frame.index &&
	       left->mapping_frame.data_bits == right->mapping_frame.data_bits &&
	       left->mapping_frame.primary_bits ==
		       right->mapping_frame.primary_bits &&
	       left->mapping_frame.inversion_4d_mask ==
		       right->mapping_frame.inversion_4d_mask &&
	       left->mapping_frame.high_frame == right->mapping_frame.high_frame &&
	       left->mapping_frame.carries_auxiliary_bit ==
		       right->mapping_frame.carries_auxiliary_bit &&
	       left->mapping_frame.completes_data_frame ==
		       right->mapping_frame.completes_data_frame &&
	       left->primary_bit_count == right->primary_bit_count &&
	       memcmp(left->primary_bits, right->primary_bits,
		      left->primary_bit_count) == 0 &&
	       left->auxiliary_bit_count == right->auxiliary_bit_count &&
	       left->auxiliary_bit == right->auxiliary_bit;
}

static bool test_generated_pair_path(void)
{
	bool saw_b1_error = false;

	for (size_t mode = 0; mode < ARRAY_LENGTH(CONFIGURATIONS); ++mode) {
		const struct configuration *configuration =
			&CONFIGURATIONS[mode];
		struct fixture fixture;
		size_t b1_complete = 0;
		size_t emitted_frames = 0;
		size_t frame_count;

		CHECK(initialize_fixture(&fixture, configuration),
		      "mode %zu did not initialize", mode);
		frame_count = PIPELINE_MAPPING_FRAMES +
			fixture.mapping.schedule.geometry
				.mapping_frames_per_data_frame + 3u;
		for (size_t frame_index = 0; frame_index < frame_count;
		     ++frame_index) {
			struct v34_transmitted_mapping_frame transmitted;
			bool b1 = frame_index <
				fixture.mapping.schedule.geometry
					.mapping_frames_per_data_frame;

			CHECK(transmit_frame(
				      &fixture, configuration, frame_index,
				      &transmitted),
			      "mode %zu frame %zu did not transmit", mode,
			      frame_index);
			for (size_t interval = 0;
			     interval < V34_4D_INTERVALS_PER_MAPPING_FRAME;
			     ++interval) {
				struct qam_point quantized[2];
				struct v34_adaptive_equalizer_observation input[2];
				struct v34_data_decision_output output;
				struct v34_survivor_decision model_current;
				struct v34_received_mapping_frame model_received;
				struct qam_point targets[2];
				struct v34_analytic_sample fixed_target;
				enum v34_data_receive_result model_result;
				enum v34_data_receive_result result;

				for (size_t symbol = 0; symbol < 2u; ++symbol) {
					struct qam_point normalized;
					size_t index = interval * 2u + symbol;

					CHECK(v34_normalize_power(
						      &fixture.mapping.normalizer,
						      &transmitted.projected_symbol[index],
						      &normalized) == V34_SYMBOL_POWER_OK &&
					      make_observation(
						      &fixture, &normalized,
						      &quantized[symbol],
						      &input[symbol]),
					      "mode %zu frame %zu symbol %zu did not convert",
					      mode, frame_index, index);
					/* B1 remains a received frame.  Corrupt one
					 * interval and require the shared demapper to
					 * retain that evidence. */
					if (b1 && frame_index == 0u && interval == 0u) {
						input[symbol].decision.in_phase =
							symbol == 0u ? 12000 : -12000;
						input[symbol].decision.quadrature =
							symbol == 0u ? -9000 : 9000;
						quantized[symbol] = (struct qam_point){
							.in_phase =
								input[symbol].decision.in_phase /
								(double)
									V34_DATA_DECISION_FIXED_AMPLITUDE,
							.quadrature =
								input[symbol].decision.quadrature /
								(double)
									V34_DATA_DECISION_FIXED_AMPLITUDE,
						};
					}
				}
				result = v34_data_decision_receiver_observe(
					&fixture.receiver, &input[0], &output);
				CHECK(result == V34_DATA_RECEIVE_OK,
				      "mode %zu frame %zu interval %zu first result %d",
				      mode, frame_index, interval, result);
				CHECK(observations_match(
					      &output.equalizer.observation, &input[0]) &&
				      output.equalizer.target.in_phase == 0 &&
				      output.equalizer.target.quadrature == 0 &&
				      output.equalizer.decoder_power_q15 == 0u &&
				      output.equalizer.adaptation ==
					      V34_ADAPTIVE_EQUALIZER_SKIP,
				      "mode %zu frame %zu interval %zu first decision changed",
				      mode, frame_index, interval);
				CHECK(memcmp(
					      &output.current,
					      &(struct v34_survivor_decision){ 0 },
					      sizeof(output.current)) == 0 &&
				      memcmp(
					      &output.received,
					      &(struct v34_received_mapping_frame){ 0 },
					      sizeof(output.received)) == 0,
				      "mode %zu frame %zu interval %zu first outputs changed",
				      mode, frame_index, interval);
				CHECK(memcmp(&fixture.receiver.data_receiver,
					     &fixture.model,
					     sizeof(fixture.model)) == 0,
				      "mode %zu frame %zu interval %zu model state changed",
				      mode, frame_index, interval);
				model_result = v34_data_channel_receiver_advance(
					&fixture.model, quantized, &model_current,
					targets, &model_received);
				result = v34_data_decision_receiver_observe(
					&fixture.receiver, &input[1], &output);
				CHECK(fixed_coordinate(
					      targets[1].in_phase,
					      &fixed_target.in_phase) &&
				      fixed_coordinate(
					      targets[1].quadrature,
					      &fixed_target.quadrature) &&
				      result ==
					      (model_result == V34_DATA_RECEIVE_OK ?
						       V34_DATA_RECEIVE_INTERVAL_READY :
						       model_result) &&
				      observations_match(
					      &output.equalizer.observation, &input[1]) &&
				      output.equalizer.target.in_phase ==
					      fixed_target.in_phase &&
				      output.equalizer.target.quadrature ==
					      fixed_target.quadrature &&
				      output.equalizer.decoder_power_q15 == 0x2000u &&
				      output.equalizer.adaptation ==
					      V34_ADAPTIVE_EQUALIZER_FULL &&
				      current_matches(
					      &output.current, &model_current) &&
				      memcmp(&fixture.receiver.data_receiver,
					     &fixture.model,
					     sizeof(fixture.model)) == 0 &&
				      !fixture.receiver.has_first_symbol,
				      "mode %zu frame %zu interval %zu second symbol changed",
				      mode, frame_index, interval);
				if (model_result == V34_DATA_RECEIVE_OK) {
					CHECK(memcmp(
						      &output.received,
						      &(struct v34_received_mapping_frame){ 0 },
						      sizeof(output.received)) == 0,
					      "mode %zu emitted an incomplete frame",
					      mode);
					continue;
				}
				CHECK(received_matches(
					      &output.received, &model_received),
				      "mode %zu frame output changed", mode);
				emitted_frames++;
				if (result ==
				    V34_DATA_RECEIVE_B1_COMPLETE)
					b1_complete++;
			}
		}
		saw_b1_error = saw_b1_error ||
			fixture.receiver.data_receiver.data_receiver
				.b1_error_bits != 0u;
		CHECK(b1_complete == 1u && emitted_frames >=
			      fixture.mapping.schedule.geometry
				      .mapping_frames_per_data_frame &&
		      !v34_data_decision_receiver_is_receiving_b1(
			      &fixture.receiver),
		      "mode %zu emitted %zu frames and %zu B1 completions",
		      mode, emitted_frames, b1_complete);
	}
	CHECK(saw_b1_error, "corrupted B1 was masked");
	return true;
}

static bool rejected_call_preserves_outputs(
	struct v34_data_decision_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *input,
	struct v34_data_decision_output *output,
	enum v34_data_receive_result expected)
{
	struct v34_data_decision_receiver saved_receiver = *receiver;
	struct v34_data_decision_output saved_output = *output;

	return v34_data_decision_receiver_observe(
		       receiver, input, output) == expected &&
	       memcmp(receiver, &saved_receiver, sizeof(*receiver)) == 0 &&
	       memcmp(output, &saved_output, sizeof(*output)) == 0;
}

static bool test_validation_and_transactionality(void)
{
	struct fixture fixture;
	struct v34_data_decision_receiver saved_receiver;
	struct v34_data_decision_receiver corrupt;
	struct v34_adaptive_equalizer_observation input = {
		.decision = { 3238, -3238 },
		.epoch = 1u,
		.lineage = 2u,
	};
	struct v34_data_decision_output output;
	struct v34_q14_coefficient invalid[V34_PRECODER_TAP_COUNT];

	CHECK(initialize_fixture(&fixture, &CONFIGURATIONS[0]),
	      "validation fixture did not initialize");
	saved_receiver = fixture.receiver;
	memcpy(invalid, COEFFICIENTS, sizeof(invalid));
	invalid[0].real = INT16_MIN;
	CHECK(v34_data_decision_receiver_init(
		      &fixture.receiver, &fixture.mapping.normalizer,
		      &fixture.mapping.schedule.geometry,
		      CONFIGURATIONS[0].expanded, invalid,
		      CONFIGURATIONS[0].trellis,
		      CONFIGURATIONS[0].transmitting_role) ==
		      V34_DATA_RECEIVE_INVALID_ARGUMENT &&
	      memcmp(&fixture.receiver, &saved_receiver,
		     sizeof(fixture.receiver)) == 0,
	      "invalid initialization changed the receiver");
	memset(&output, 0xa5, sizeof(output));
	input.epoch = 0u;
	CHECK(rejected_call_preserves_outputs(
		      &fixture.receiver, &input, &output,
		      V34_DATA_RECEIVE_INVALID_ARGUMENT),
	      "zero epoch was not transactional");
	input.epoch = 1u;
	input.lineage = 0u;
	CHECK(rejected_call_preserves_outputs(
		      &fixture.receiver, &input, &output,
		      V34_DATA_RECEIVE_INVALID_ARGUMENT),
	      "zero lineage was not transactional");
	input.lineage = 2u;
	CHECK(v34_data_decision_receiver_observe(
		      &fixture.receiver,
		      (const struct v34_adaptive_equalizer_observation *)
			      &fixture.receiver,
		      &output) ==
		      V34_DATA_RECEIVE_INVALID_ARGUMENT &&
	      v34_data_decision_receiver_observe(
		      &fixture.receiver, &input,
		      (struct v34_data_decision_output *)&input) ==
		      V34_DATA_RECEIVE_INVALID_ARGUMENT &&
	      memcmp(&fixture.receiver, &saved_receiver,
		     sizeof(fixture.receiver)) == 0,
	      "overlapping regions changed the receiver");
	corrupt = fixture.receiver;
	corrupt.first_normalized.in_phase = NAN;
	CHECK(!v34_data_decision_receiver_is_valid(NULL) &&
	      !v34_data_decision_receiver_is_valid(&corrupt) &&
	      rejected_call_preserves_outputs(
		      &corrupt, &input, &output,
		      V34_DATA_RECEIVE_INVALID_STATE) &&
	      v34_data_decision_receiver_observe(
		      NULL, &input, &output) ==
		      V34_DATA_RECEIVE_INVALID_STATE &&
	      v34_data_decision_receiver_observe(
		      &fixture.receiver, NULL, &output) ==
		      V34_DATA_RECEIVE_INVALID_ARGUMENT &&
	      v34_data_decision_receiver_observe(
		      &fixture.receiver, &input, NULL) ==
		      V34_DATA_RECEIVE_INVALID_ARGUMENT,
	      "invalid arguments were accepted");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated pair path", test_generated_pair_path },
		{ "validation and transactionality",
		  test_validation_and_transactionality },
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
