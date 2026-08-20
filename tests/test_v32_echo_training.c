// SPDX-License-Identifier: BSD-3-Clause

#include <carrier.h>
#include <constellation.h>
#include <echo_training.h>
#include <qam.h>

#include "test.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
	TEST_CALLING_S_EXTENSION = 96,
};

static const struct qam_channel V32_CHANNEL = {
	.sample_rate = V32_CARRIER_SAMPLE_RATE,
	.symbol_rate_numerator = V32_CARRIER_SYMBOL_RATE,
	.symbol_rate_denominator = 1,
	.carrier_frequency_numerator = V32_CARRIER_FREQUENCY_HZ,
	.carrier_frequency_denominator = 1,
	.rolloff = 0.25,
	.pulse_span_symbols = V32_CARRIER_PULSE_SPAN_SYMBOLS,
};

static uint16_t role_trn_symbols(enum v32_transmitter_role role)
{
	return role == V32_CALLING_TRANSMITTER ?
		V32_CALLING_ECHO_TRAINING_SYMBOLS :
		V32_ANSWERING_ECHO_TRAINING_SYMBOLS;
}

static uint16_t role_s_extension(enum v32_transmitter_role role)
{
	return role == V32_CALLING_TRANSMITTER ?
		TEST_CALLING_S_EXTENSION : 0;
}

static uint16_t emitted_trn_symbols(
	const struct v32_echo_training *training)
{
	uint16_t fixed = V32_CONDITIONING_S_SYMBOLS +
		V32_CONDITIONING_S_BAR_SYMBOLS;

	return training->conditioning.emitted_symbols > fixed ?
		(uint16_t)(training->conditioning.emitted_symbols - fixed) :
		0;
}

static bool coefficients_are_zero(const struct v32_echo_canceller *echo)
{
	for (size_t phase = 0; phase < V32_ECHO_PHASE_COUNT; ++phase) {
		for (size_t window = 0; window < V32_ECHO_WINDOW_COUNT;
		     ++window) {
			for (size_t tap = 0; tap < V32_ECHO_WINDOW_SYMBOLS;
			     ++tap) {
				const struct qam_point *coefficient =
					&echo->coefficients[phase][window][tap];

				if (coefficient->in_phase != 0.0 ||
				    coefficient->quadrature != 0.0)
					return false;
			}
		}
	}
	return true;
}

static bool run_role(
	enum v32_transmitter_role role,
	struct v32_echo_training *completed_training,
	struct v32_receive_path *completed_path)
{
	struct v32_echo_training training;
	struct v32_conditioning_transmitter reference_conditioning;
	struct v32_receive_path path;
	struct v32_line_receiver initial_line;
	struct v32_equalizer initial_equalizer;
	struct v32_symbol_encoder encoder;
	struct qam_modulator modulator;
	struct qam_point point = { 0 };
	uint16_t limit = role_trn_symbols(role);
	uint16_t extension = role_s_extension(role);
	uint16_t fixed = V32_CONDITIONING_S_SYMBOLS +
		V32_CONDITIONING_S_BAR_SYMBOLS;
	size_t training_symbols = 0;
	size_t line_samples = 0;
	bool handed_off = false;
	bool saw_conditioning_start = false;
	bool saw_fixed_end = false;
	bool saw_trn_start = false;
	bool saw_prefill_end = false;
	bool saw_coarse = false;
	bool saw_coarse_end = false;
	bool saw_fine = false;
	bool saw_trn_end = false;

	CHECK(v32_receive_path_init(
		      &path, 0, V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_RECEIVE_PATH_OK,
	      "echo receive path did not initialize for role %d", role);
	path.line.gain.smoothed_level = 4096;
	path.line.gain.gain_mantissa_q15 = 16384;
	path.line.gain.gain_shift = 1;
	path.line.gain.signal_present = true;
	CHECK(v32_receive_path_is_valid(&path) &&
	      (role != V32_CALLING_TRANSMITTER ||
	       v32_receive_path_freeze_gain(&path) == V32_RECEIVE_PATH_OK) &&
	      v32_echo_training_init(&training, role, extension) ==
		      V32_ECHO_TRAINING_OK &&
	      v32_conditioning_transmitter_init(
		      &reference_conditioning, role, limit) ==
		      V32_CONDITIONING_OK &&
	      v32_echo_training_path_is_valid(&training, &path) &&
	      qam_modulator_init(&modulator, &V32_CHANNEL, 12000) == QAM_OK,
	      "echo training did not initialize for role %d", role);
	initial_line = path.line;
	initial_equalizer = path.equalizer;

	while (!v32_echo_training_is_complete(&training) ||
	       path.line.input_paused) {
		bool can_accept = qam_modulator_can_accept_symbol(&modulator);
		bool accepted;
		struct v32_echo_reference reference;
		struct v32_equalizer_observation observation;
		int16_t transmit;
		enum v32_receive_path_result receive_result;

		if (can_accept) {
			uint8_t quadrant = UINT8_C(0xa5);
			uint8_t expected_quadrant;
			enum v32_echo_training_result result;

			if (!handed_off) {
				result = v32_echo_training_next(
					&training, &path, &quadrant);
				if (result == V32_ECHO_TRAINING_OK) {
					if (training_symbols < extension) {
						expected_quadrant =
							(training_symbols & 1u) != 0 ? 1 : 0;
					} else {
						CHECK(v32_conditioning_transmitter_next(
							      &reference_conditioning,
							      &expected_quadrant) ==
							      V32_CONDITIONING_OK,
						      "role %d reference conditioning stopped at %zu",
						      role, training_symbols);
					}
					CHECK(
					      quadrant == expected_quadrant,
					      "role %d changed training symbol %zu",
					      role, training_symbols);
					training_symbols++;
				} else {
					struct v32_symbol_encoder expected_encoder;

					CHECK(result == V32_ECHO_TRAINING_COMPLETE &&
					      quadrant == UINT8_C(0xa5) &&
					      v32_conditioning_transmitter_is_complete(
						      &reference_conditioning) &&
					      v32_echo_training_framing_state(
						      &training, &encoder) ==
						      V32_ECHO_TRAINING_OK &&
					      v32_conditioning_transmitter_framing_state(
						      &reference_conditioning,
						      &expected_encoder) ==
						      V32_CONDITIONING_OK &&
					      memcmp(&encoder, &expected_encoder,
						     sizeof(encoder)) == 0,
					      "role %d changed its post-TRN handoff", role);
					handed_off = true;
					CHECK(v32_4800_symbol_encode(
						      &encoder, 0, &quadrant) ==
						      V32_FRAMING_OK,
					      "role %d did not emit its first rate symbol",
					      role);
				}
			} else {
				CHECK(v32_4800_symbol_encode(
					      &encoder,
					      (uint8_t)(line_samples & 3u),
					      &quadrant) == V32_FRAMING_OK,
				      "role %d did not continue after TRN", role);
			}
			CHECK(v32_4800_constellation_point(quadrant, &point) ==
				      V32_CONSTELLATION_OK &&
			      v32_echo_reference_init(
				      &reference, &point,
				      modulator.carrier_phase) == V32_ECHO_OK,
			      "role %d did not type transmit symbol %zu", role,
			      training_symbols);

			if (training.s_extension_emitted == 1 ||
			    (extension == 0 &&
			     training.conditioning.emitted_symbols == 1)) {
				CHECK(path.echo_training &&
				      !path.echo.echo.adapting,
				      "role %d did not pause at its first S symbol", role);
				saw_conditioning_start = true;
			} else if (training.conditioning.emitted_symbols == fixed) {
				CHECK(path.echo_training &&
				      !path.echo.echo.adapting &&
				      coefficients_are_zero(&path.echo.echo),
				      "role %d changed the pre-TRN pause", role);
				saw_fixed_end = true;
			} else if (emitted_trn_symbols(&training) == 1) {
				CHECK(path.echo_training &&
				      !path.echo.echo.adapting,
				      "role %d did not start at its first TRN", role);
				saw_trn_start = true;
			} else if (emitted_trn_symbols(&training) ==
				   V32_ECHO_TRAINING_PREFILL_SYMBOLS) {
				CHECK(path.echo_training &&
				      !path.echo.echo.adapting &&
				      coefficients_are_zero(&path.echo.echo),
				      "role %d changed its prefill boundary", role);
				saw_prefill_end = true;
			} else if (emitted_trn_symbols(&training) ==
				   V32_ECHO_TRAINING_PREFILL_SYMBOLS + 1u) {
				CHECK(path.echo.echo.adapting &&
				      path.echo.echo.adaptation == 0.65,
				      "role %d did not enter coarse adaptation", role);
				saw_coarse = true;
			} else if (emitted_trn_symbols(&training) ==
				   V32_ECHO_TRAINING_PREFILL_SYMBOLS +
					   V32_ECHO_TRAINING_COARSE_SYMBOLS) {
				CHECK(path.echo.echo.adapting &&
				      path.echo.echo.adaptation == 0.65,
				      "role %d shortened coarse adaptation", role);
				saw_coarse_end = true;
			} else if (emitted_trn_symbols(&training) ==
				   V32_ECHO_TRAINING_PREFILL_SYMBOLS +
					   V32_ECHO_TRAINING_COARSE_SYMBOLS + 1u) {
				CHECK(path.echo.echo.adapting &&
				      path.echo.echo.adaptation == 0.065,
				      "role %d did not reduce adaptation", role);
				saw_fine = true;
			}
			if (!training.complete &&
			    emitted_trn_symbols(&training) == limit) {
				CHECK(path.echo.echo.adapting &&
				      path.echo.echo.adaptation == 0.065,
				      "role %d changed its final TRN symbol", role);
				saw_trn_end = true;
			}
			if (training.complete)
				CHECK(!path.echo_training &&
				      !path.echo.echo.adapting,
				      "role %d crossed the handoff late", role);
		}
		CHECK(qam_modulator_sample(
			      &modulator, &point, &transmit, &accepted) == QAM_OK &&
		      accepted == can_accept,
		      "role %d transmit cadence failed at %zu", role,
		      line_samples);
		receive_result = v32_receive_path_sample(
			&path, transmit, accepted ? &reference : NULL,
			&observation);
		CHECK(receive_result == V32_RECEIVE_PATH_OK,
		      "role %d exposed a paused decision at %zu: %d", role,
		      line_samples, receive_result);
		CHECK(v32_echo_training_path_is_valid(&training, &path),
		      "role %d lost training ownership at %zu", role,
		      line_samples);
		line_samples++;
		CHECK(line_samples < 40000,
		      "role %d exceeded its line-sample bound", role);
	}
	CHECK(training_symbols == (size_t)extension + fixed + limit &&
	      training.s_extension_emitted == extension &&
	      v32_conditioning_transmitter_is_complete(
		      &reference_conditioning) &&
	      v32_echo_training_is_complete(&training) && handed_off &&
	      saw_conditioning_start && saw_fixed_end && saw_trn_start &&
	      saw_prefill_end && saw_coarse && saw_coarse_end && saw_fine &&
	      saw_trn_end &&
	      !path.echo_training && !path.echo.echo.adapting &&
	      !path.line.input_paused &&
	      !coefficients_are_zero(&path.echo.echo) &&
	      path.line.gain.gain_mode == AGC_GAIN_TRACKING &&
	      path.line.gain.gain_mantissa_q15 ==
		      initial_line.gain.gain_mantissa_q15 &&
	      path.line.gain.gain_shift == initial_line.gain.gain_shift &&
	      (role == V32_CALLING_TRANSMITTER ?
		       (path.line.gain.smoothed_level == 0 &&
			!path.line.gain.signal_present) :
		       memcmp(&path.line.gain, &initial_line.gain,
			      sizeof(path.line.gain)) == 0) &&
	      memcmp(&path.line.timing, &initial_line.timing,
		     sizeof(path.line.timing)) == 0 &&
	      memcmp(&path.equalizer, &initial_equalizer,
		     sizeof(path.equalizer)) == 0,
	      "role %d changed its complete echo-training lineage", role);
	*completed_training = training;
	*completed_path = path;
	return true;
}

static bool test_contracts(
	const struct v32_echo_training *complete,
	const struct v32_receive_path *complete_path)
{
	struct v32_echo_training training;
	struct v32_echo_training saved_training;
	struct v32_receive_path path;
	struct v32_receive_path saved_path;
	struct v32_symbol_encoder encoder = { .phase_quadrant = 3 };
	struct v32_symbol_encoder saved_encoder = encoder;
	uint8_t quadrant = UINT8_C(0xa5);
	uint8_t saved_quadrant = quadrant;

	CHECK(v32_echo_training_init(
		      NULL, V32_CALLING_TRANSMITTER, 0) ==
		      V32_ECHO_TRAINING_INVALID_ARGUMENT &&
	      v32_echo_training_init(
		      &training, (enum v32_transmitter_role)2, 0) ==
		      V32_ECHO_TRAINING_INVALID_ARGUMENT &&
	      v32_echo_training_init(
		      &training, V32_CALLING_TRANSMITTER, 1) ==
		      V32_ECHO_TRAINING_INVALID_ARGUMENT &&
	      v32_echo_training_init(
		      &training, V32_CALLING_TRANSMITTER,
		      V32_ECHO_MAX_ROUND_TRIP_SYMBOLS + 2u) ==
		      V32_ECHO_TRAINING_INVALID_ARGUMENT &&
	      v32_echo_training_init(
		      &training, V32_ANSWERING_TRANSMITTER, 2) ==
		      V32_ECHO_TRAINING_INVALID_ARGUMENT &&
	      v32_echo_training_init(
		      &training, V32_CALLING_TRANSMITTER, 2) ==
		      V32_ECHO_TRAINING_OK &&
	      v32_receive_path_init(
		      &path, 0, V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_RECEIVE_PATH_OK,
	      "echo-training contract fixture did not initialize");
	saved_training = training;
	saved_path = path;
	CHECK(!v32_echo_training_path_is_valid(&training, &path) &&
	      v32_echo_training_next(&training, &path, &quadrant) ==
		      V32_ECHO_TRAINING_INVALID_STATE &&
	      memcmp(&training, &saved_training, sizeof(training)) == 0 &&
	      memcmp(&path, &saved_path, sizeof(path)) == 0 &&
	      quadrant == saved_quadrant &&
	      v32_receive_path_freeze_gain(&path) == V32_RECEIVE_PATH_OK &&
	      !v32_echo_training_path_is_valid(&training, &path),
	      "calling echo training accepted an unlearned gain");
	path.line.gain.smoothed_level = 4096;
	path.line.gain.gain_mantissa_q15 = 16384;
	path.line.gain.gain_shift = 1;
	path.line.gain.signal_present = true;
	CHECK(v32_echo_training_path_is_valid(&training, &path),
	      "calling echo training rejected a learned frozen gain");
	saved_path = path;
	CHECK(v32_echo_training_next(NULL, &path, &quadrant) ==
		      V32_ECHO_TRAINING_INVALID_ARGUMENT &&
	      v32_echo_training_next(&training, NULL, &quadrant) ==
		      V32_ECHO_TRAINING_INVALID_ARGUMENT &&
	      v32_echo_training_next(&training, &path, NULL) ==
		      V32_ECHO_TRAINING_INVALID_ARGUMENT &&
	      v32_echo_training_next(
		      (struct v32_echo_training *)&path, &path, &quadrant) ==
		      V32_ECHO_TRAINING_INVALID_ARGUMENT &&
	      v32_echo_training_next(
		      &training, &path, (uint8_t *)&training) ==
		      V32_ECHO_TRAINING_INVALID_ARGUMENT &&
	      v32_echo_training_next(
		      &training, &path, (uint8_t *)&path) ==
		      V32_ECHO_TRAINING_INVALID_ARGUMENT &&
	      v32_echo_training_framing_state(&training, &encoder) ==
		      V32_ECHO_TRAINING_INVALID_STATE &&
	      memcmp(&training, &saved_training, sizeof(training)) == 0 &&
	      memcmp(&path, &saved_path, sizeof(path)) == 0 &&
	      quadrant == saved_quadrant &&
	      memcmp(&encoder, &saved_encoder, sizeof(encoder)) == 0,
	      "rejected echo-training call changed an owner or output");
	CHECK(v32_echo_training_next(&training, &path, &quadrant) ==
		      V32_ECHO_TRAINING_OK &&
	      path.echo_training && !path.echo.echo.adapting,
	      "first extended S symbol did not pause the receive path");
	saved_path = path;
	CHECK(v32_echo_training_next(&training, &path, &quadrant) ==
		      V32_ECHO_TRAINING_OK &&
	      memcmp(&path, &saved_path, sizeof(path)) == 0,
	      "extended S copied or changed the receive path");
	saved_path = path;
	CHECK(v32_echo_training_next(&training, &path, &quadrant) ==
		      V32_ECHO_TRAINING_OK &&
	      training.conditioning.emitted_symbols == 1 &&
	      memcmp(&path, &saved_path, sizeof(path)) == 0,
	      "ordinary S restarted or changed the receive path");

	path.echo_training = false;
	saved_training = training;
	saved_path = path;
	quadrant = saved_quadrant;
	CHECK(!v32_echo_training_path_is_valid(&training, &path) &&
	      v32_echo_training_next(&training, &path, &quadrant) ==
		      V32_ECHO_TRAINING_INVALID_STATE &&
	      memcmp(&training, &saved_training, sizeof(training)) == 0 &&
	      memcmp(&path, &saved_path, sizeof(path)) == 0 &&
	      quadrant == saved_quadrant,
	      "mismatched conditioning and echo owners were spliced");

	training = *complete;
	path = *complete_path;
	saved_training = training;
	saved_path = path;
	quadrant = saved_quadrant;
	CHECK(v32_echo_training_next(&training, &path, &quadrant) ==
		      V32_ECHO_TRAINING_COMPLETE &&
	      memcmp(&training, &saved_training, sizeof(training)) == 0 &&
	      memcmp(&path, &saved_path, sizeof(path)) == 0 &&
	      quadrant == saved_quadrant &&
	      v32_echo_training_framing_state(&training, &encoder) ==
		      V32_ECHO_TRAINING_OK &&
	      v32_echo_training_framing_state(NULL, &encoder) ==
		      V32_ECHO_TRAINING_INVALID_ARGUMENT &&
	      v32_echo_training_framing_state(&training, NULL) ==
		      V32_ECHO_TRAINING_INVALID_ARGUMENT &&
	      v32_echo_training_framing_state(
		      &training, (struct v32_symbol_encoder *)&training) ==
		      V32_ECHO_TRAINING_INVALID_ARGUMENT,
	      "complete echo training advanced twice or aliased its handoff");
	training.conditioning.trn_symbols++;
	CHECK(!v32_echo_training_is_valid(&training),
	      "echo training accepted an out-of-range conditioning count");
	return true;
}

int main(void)
{
	struct v32_echo_training calling;
	struct v32_echo_training answering;
	struct v32_receive_path calling_path;
	struct v32_receive_path answering_path;

	if (!run_role(
		    V32_CALLING_TRANSMITTER, &calling, &calling_path) ||
	    !run_role(
		    V32_ANSWERING_TRANSMITTER, &answering,
		    &answering_path) ||
	    !test_contracts(&calling, &calling_path))
		return 1;
	CHECK(calling.conditioning.trn_symbols ==
		      V32_CALLING_ECHO_TRAINING_SYMBOLS &&
	      calling.s_extension_symbols == TEST_CALLING_S_EXTENSION &&
	      calling.s_extension_emitted == TEST_CALLING_S_EXTENSION &&
	      answering.conditioning.trn_symbols ==
		      V32_ANSWERING_ECHO_TRAINING_SYMBOLS &&
	      answering.s_extension_symbols == 0 &&
	      answering.s_extension_emitted == 0,
	      "role-specific echo-training lengths changed");
	puts("V.32 echo training tests passed");
	return 0;
}
