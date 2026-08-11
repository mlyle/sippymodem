// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_mapping_frame_transmitter.h>
#include <sippymodem_v34_symbol_power.h>
#include <v34_channel_frame_receiver.h>

#include "test.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	PIPELINE_MAPPING_FRAMES =
		SIPPYMODEM_V34_SURVIVOR_TRACEBACK_INTERVALS /
		SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME,
	GENERATED_MAPPING_FRAMES = PIPELINE_MAPPING_FRAMES + 6,
};

static const struct sippymodem_v34_q14_coefficient COEFFICIENTS[] = {
	{ 4096, 2048 },
	{ -2048, 1024 },
	{ 1024, -2048 },
};

struct configuration {
	enum sippymodem_v34_symbol_rate symbol_rate;
	uint8_t primary_rate;
	bool auxiliary;
	bool expanded;
	enum sippymodem_v34_station_role transmitting_role;
	enum sippymodem_v34_trellis_selection trellis;
	enum sippymodem_v34_nonlinear_parameter nonlinear;
};

static const struct configuration CONFIGURATIONS[] = {
	{ SIPPYMODEM_V34_SYMBOL_RATE_3000, 8, true, false,
	  SIPPYMODEM_V34_CALLING_STATION, SIPPYMODEM_V34_TRELLIS_16_STATE,
	  SIPPYMODEM_V34_NONLINEAR_THETA_ZERO },
	{ SIPPYMODEM_V34_SYMBOL_RATE_3429, 14, false, true,
	  SIPPYMODEM_V34_ANSWERING_STATION, SIPPYMODEM_V34_TRELLIS_64_STATE,
	  SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16 },
};

struct fixture {
	struct sippymodem_v34_frame_geometry geometry;
	struct sippymodem_v34_mapping_parameters parameters;
	struct sippymodem_v34_frame_schedule transmit_schedule;
	struct sippymodem_v34_scrambler scrambler;
	struct sippymodem_v34_4d_encoder encoder;
	struct sippymodem_v34_power_normalizer normalizer;
	struct sippymodem_v34_channel_frame_receiver receiver;
};

struct expected_frame {
	struct sippymodem_v34_decided_4d_interval
		interval[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME];
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
	const struct sippymodem_v34_symbol_power_measurement precoded = {
		.mean_squared_magnitude = 25.0,
		.symbol_count = 256,
	};
	struct sippymodem_v34_symbol_power_measurement projected = {
		.mean_squared_magnitude = 30.0,
		.symbol_count = 256,
	};
	struct fixture generated = { 0 };

	if (configuration->nonlinear == SIPPYMODEM_V34_NONLINEAR_THETA_ZERO)
		projected.mean_squared_magnitude =
			precoded.mean_squared_magnitude;
	if (sippymodem_v34_frame_geometry_init(
		    &generated.geometry, configuration->symbol_rate,
		    configuration->primary_rate, configuration->auxiliary) !=
		    SIPPYMODEM_V34_FRAME_SCHEDULE_OK ||
	    sippymodem_v34_mapping_parameters_init(
		    &generated.parameters, &generated.geometry,
		    configuration->expanded) !=
		    SIPPYMODEM_V34_MAPPING_PARAMETERS_OK ||
	    sippymodem_v34_b1_frame_schedule_init(
		    &generated.transmit_schedule, &generated.geometry) !=
		    SIPPYMODEM_V34_FRAME_SCHEDULE_OK ||
	    sippymodem_v34_transmit_scrambler_init(
		    &generated.scrambler, configuration->transmitting_role) !=
		    SIPPYMODEM_V34_FRAMING_OK ||
	    sippymodem_v34_4d_encoder_init(
		    &generated.encoder, &generated.parameters, COEFFICIENTS,
		    configuration->trellis) != SIPPYMODEM_V34_4D_ENCODER_OK ||
	    sippymodem_v34_power_normalizer_init(
		    &generated.normalizer, &generated.parameters,
		    configuration->nonlinear, &precoded, &projected) !=
		    SIPPYMODEM_V34_SYMBOL_POWER_OK ||
	    sippymodem_v34_channel_frame_receiver_init(
		    &generated.receiver, &generated.normalizer,
		    &generated.geometry, configuration->expanded, COEFFICIENTS,
		    configuration->trellis) !=
		    SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_OK)
		return false;
	*fixture = generated;
	return true;
}

static void generate_primary_bits(
	uint8_t bits[SIPPYMODEM_V34_MAX_MAPPING_FRAME_BITS], size_t count,
	uint32_t seed)
{
	uint32_t state = seed | 1u;

	for (size_t index = 0; index < count; ++index)
		bits[index] = (uint8_t)(next_generated_word(&state) >> 31);
}

static bool transmit_frame(
	struct fixture *fixture, const struct configuration *configuration,
	size_t frame_index, struct sippymodem_v34_transmitted_mapping_frame *frame)
{
	struct sippymodem_v34_frame_schedule preview;
	struct sippymodem_v34_mapping_frame mapping_frame;
	uint8_t primary_bits[SIPPYMODEM_V34_MAX_MAPPING_FRAME_BITS];
	uint8_t auxiliary_bit = (uint8_t)(frame_index & 1u);
	const uint8_t *auxiliary;

	if (sippymodem_v34_frame_schedule_is_complete(
		    &fixture->transmit_schedule) &&
	    sippymodem_v34_frame_schedule_advance_data_frame(
		    &fixture->transmit_schedule) !=
		    SIPPYMODEM_V34_FRAME_SCHEDULE_OK)
		return false;
	preview = fixture->transmit_schedule;
	if (sippymodem_v34_frame_schedule_next(&preview, &mapping_frame) !=
	    SIPPYMODEM_V34_FRAME_SCHEDULE_OK)
		return false;
	generate_primary_bits(
		primary_bits, mapping_frame.primary_bits,
		UINT32_C(0xb8074f65) ^ (uint32_t)frame_index);
	auxiliary = mapping_frame.carries_auxiliary_bit ? &auxiliary_bit : NULL;
	return sippymodem_v34_transmit_mapping_frame(
		       &fixture->transmit_schedule, &fixture->scrambler,
		       &fixture->encoder, primary_bits, mapping_frame.primary_bits,
		       auxiliary, configuration->nonlinear,
		       fixture->normalizer.precoded_mean_squared_magnitude,
		       frame) == SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_OK;
}

static void remember_expected_frame(
	const struct sippymodem_v34_transmitted_mapping_frame *transmitted,
	struct expected_frame *expected)
{
	for (size_t interval = 0;
	     interval < SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME;
	     ++interval) {
		expected->interval[interval].trellis_output =
			transmitted->encoded.interval[interval].trellis_output;
		for (size_t symbol = 0;
		     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL;
		     ++symbol)
			expected->interval[interval].mapper_output[symbol] =
				transmitted->encoded.interval[interval]
					.mapped[symbol]
					.mapper_output;
	}
}

static bool current_decision_matches(
	const struct sippymodem_v34_survivor_decision *current,
	const struct sippymodem_v34_4d_encoder_output *encoded,
	uint8_t inversion_bit)
{
	if (current->convolutional_output != encoded->convolutional_output ||
	    current->inversion_bit != inversion_bit)
		return false;
	for (size_t symbol = 0;
	     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		if (memcmp(&current->channel_output[symbol],
			   &encoded->precoded[symbol].channel_output,
			   sizeof(current->channel_output[symbol])) != 0)
			return false;
	}
	return true;
}

static bool precoder_histories_match(
	const struct sippymodem_v34_precoder *left,
	const struct sippymodem_v34_precoder *right)
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
		struct expected_frame expected[GENERATED_MAPPING_FRAMES];
		struct fixture fixture;
		uint32_t random = UINT32_C(0x47d25631) ^ (uint32_t)mode;
		size_t received_frames = 0;

		CHECK(initialize_fixture(&fixture, configuration),
		      "channel receiver mode %zu did not initialize", mode);
		for (size_t frame_index = 0;
		     frame_index < ARRAY_LENGTH(expected); ++frame_index) {
			struct sippymodem_v34_transmitted_mapping_frame transmitted;

			CHECK(transmit_frame(
				      &fixture, configuration, frame_index,
				      &transmitted),
			      "channel receiver mode %zu frame %zu did not transmit",
			      mode, frame_index);
			remember_expected_frame(&transmitted, &expected[frame_index]);
			for (size_t interval = 0;
			     interval <
				     SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME;
			     ++interval) {
				struct sippymodem_qam_point normalized
					[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL];
				struct sippymodem_v34_survivor_decision current;
				struct sippymodem_v34_decided_4d_interval decided
					[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME];
				struct sippymodem_v34_decided_4d_interval saved_decided
					[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME];
				bool ready = true;
				uint8_t inversion_bit = (uint8_t)(
					(transmitted.mapping_frame.inversion_4d_mask >>
					 interval) &
					1u);

				for (size_t symbol = 0;
				     symbol <
					     SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL;
				     ++symbol) {
					size_t index = interval *
						       SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL +
					       symbol;

					CHECK(sippymodem_v34_normalize_power(
						      &fixture.normalizer,
						      &transmitted.projected_symbol[index],
						      &normalized[symbol]) ==
						      SIPPYMODEM_V34_SYMBOL_POWER_OK,
					      "mode %zu frame %zu symbol %zu did not normalize",
					      mode, frame_index, index);
					normalized[symbol].in_phase += generated_noise(
						&random,
						fixture.normalizer.normalization_gain);
					normalized[symbol].quadrature += generated_noise(
						&random,
						fixture.normalizer.normalization_gain);
				}
				memset(decided, 0xa5, sizeof(decided));
				memcpy(saved_decided, decided, sizeof(decided));
				CHECK(sippymodem_v34_channel_frame_receiver_advance(
					      &fixture.receiver, normalized, &current,
					      decided, &ready) ==
					      SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_OK &&
				      current_decision_matches(
					      &current,
					      &transmitted.encoded.interval[interval],
					      inversion_bit),
				      "mode %zu frame %zu interval %zu decision changed",
				      mode, frame_index, interval);
				if (!ready) {
					CHECK(memcmp(decided, saved_decided,
						     sizeof(decided)) == 0,
					      "channel receiver emitted a partial frame");
					continue;
				}
				CHECK(received_frames < frame_index &&
					      memcmp(decided,
						     expected[received_frames].interval,
						     sizeof(decided)) == 0,
				      "mode %zu decided frame %zu changed", mode,
				      received_frames);
				received_frames++;
			}
			CHECK(precoder_histories_match(
				      &fixture.receiver.receive_precoder,
				      &fixture.encoder.precoder),
			      "mode %zu frame %zu feedback history drifted", mode,
			      frame_index);
		}
		CHECK(received_frames ==
			      GENERATED_MAPPING_FRAMES - PIPELINE_MAPPING_FRAMES,
		      "mode %zu emitted %zu frames", mode, received_frames);
	}
	return true;
}

static bool outputs_are_unchanged_after(
	struct sippymodem_v34_channel_frame_receiver *receiver,
	const struct sippymodem_qam_point
		normalized[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL],
	enum sippymodem_v34_channel_frame_receiver_result expected_result)
{
	struct sippymodem_v34_channel_frame_receiver saved_receiver = *receiver;
	struct sippymodem_v34_survivor_decision current;
	struct sippymodem_v34_survivor_decision saved_current;
	struct sippymodem_v34_decided_4d_interval decided
		[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME];
	struct sippymodem_v34_decided_4d_interval saved_decided
		[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME];
	bool ready = true;

	memset(&current, 0xa5, sizeof(current));
	saved_current = current;
	memset(decided, 0x5a, sizeof(decided));
	memcpy(saved_decided, decided, sizeof(decided));
	return sippymodem_v34_channel_frame_receiver_advance(
		       receiver, normalized, &current, decided, &ready) ==
		       expected_result &&
	       memcmp(receiver, &saved_receiver, sizeof(*receiver)) == 0 &&
	       memcmp(&current, &saved_current, sizeof(current)) == 0 &&
	       memcmp(decided, saved_decided, sizeof(decided)) == 0 && ready;
}

static bool test_validation_and_rollback(void)
{
	struct fixture fixture;
	struct fixture other;
	struct sippymodem_v34_channel_frame_receiver saved_receiver;
	struct sippymodem_v34_q14_coefficient invalid_coefficients
		[SIPPYMODEM_V34_PRECODER_TAP_COUNT];
	struct sippymodem_qam_point invalid
		[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL] = {
			{ 0.0, 0.0 },
			{ 0.0, 0.0 },
		};
	struct sippymodem_v34_survivor_decision current;
	struct sippymodem_v34_decided_4d_interval decided
		[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME];
	bool ready;

	CHECK(initialize_fixture(&fixture, &CONFIGURATIONS[0]) &&
		      initialize_fixture(&other, &CONFIGURATIONS[1]),
	      "validation fixtures did not initialize");
	saved_receiver = fixture.receiver;
	memcpy(invalid_coefficients, COEFFICIENTS,
	       sizeof(invalid_coefficients));
	invalid_coefficients[0].real = INT16_MIN;
	CHECK(sippymodem_v34_channel_frame_receiver_init(
		      &fixture.receiver, &fixture.normalizer, &fixture.geometry,
		      CONFIGURATIONS[0].expanded, invalid_coefficients,
		      CONFIGURATIONS[0].trellis) ==
		      SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT &&
	      sippymodem_v34_channel_frame_receiver_init(
		      &fixture.receiver, &fixture.normalizer, &other.geometry,
		      CONFIGURATIONS[1].expanded, COEFFICIENTS,
		      CONFIGURATIONS[1].trellis) ==
		      SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&fixture.receiver, &saved_receiver,
		     sizeof(fixture.receiver)) == 0,
	      "invalid initialization changed the receiver");
	invalid[1].quadrature = NAN;
	CHECK(outputs_are_unchanged_after(
		      &fixture.receiver, invalid,
		      SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT),
	      "non-finite input was not transactional");
	invalid[1].quadrature = 0.0;
	invalid[0].in_phase =
		((double)INT32_MAX / SIPPYMODEM_V34_Q7_SCALE + 2.0) *
		fixture.normalizer.normalization_gain;
	CHECK(outputs_are_unchanged_after(
		      &fixture.receiver, invalid,
		      SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_OUT_OF_RANGE),
	      "unrepresentable feedback was not transactional");
	invalid[0].in_phase = DBL_MAX;
	CHECK(outputs_are_unchanged_after(
		      &fixture.receiver, invalid,
		      SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_NUMERIC_FAILURE),
	      "conditioning overflow was not transactional");
	fixture.receiver.receive_precoder.coefficients[0].real = INT16_MIN;
	CHECK(!sippymodem_v34_channel_frame_receiver_is_valid(NULL) &&
	      !sippymodem_v34_channel_frame_receiver_is_valid(
		      &fixture.receiver) &&
	      sippymodem_v34_channel_frame_receiver_advance(
		      &fixture.receiver, invalid, &current, decided, &ready) ==
		      SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT &&
	      sippymodem_v34_channel_frame_receiver_advance(
		      NULL, invalid, &current, decided, &ready) ==
		      SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT &&
	      sippymodem_v34_channel_frame_receiver_advance(
		      &saved_receiver, NULL, &current, decided, &ready) ==
		      SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT &&
	      sippymodem_v34_channel_frame_receiver_advance(
		      &saved_receiver, invalid, NULL, decided, &ready) ==
		      SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT &&
	      sippymodem_v34_channel_frame_receiver_advance(
		      &saved_receiver, invalid, &current, NULL, &ready) ==
		      SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT &&
	      sippymodem_v34_channel_frame_receiver_advance(
		      &saved_receiver, invalid, &current, decided, NULL) ==
		      SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT,
	      "invalid receiver arguments were accepted");
	return true;
}

static bool test_result_names(void)
{
	return strcmp(sippymodem_v34_channel_frame_receiver_result_name(
			      SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_OK),
		      "ok") == 0 &&
	       strcmp(sippymodem_v34_channel_frame_receiver_result_name(
			      SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT),
		      "invalid argument") == 0 &&
	       strcmp(sippymodem_v34_channel_frame_receiver_result_name(
			      SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_NUMERIC_FAILURE),
		      "numeric failure") == 0 &&
	       strcmp(sippymodem_v34_channel_frame_receiver_result_name(
			      SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_OUT_OF_RANGE),
		      "out of range") == 0 &&
	       strcmp(sippymodem_v34_channel_frame_receiver_result_name(
			      (enum sippymodem_v34_channel_frame_receiver_result)99),
		      "unknown") == 0;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated channel round trip",
		  test_generated_channel_round_trip },
		{ "validation and rollback", test_validation_and_rollback },
		{ "result names", test_result_names },
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
