// SPDX-License-Identifier: BSD-3-Clause

#include <carrier.h>
#include <echo_training.h>
#include <qam.h>

#include "test.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const struct qam_channel V32_CHANNEL = {
	.sample_rate = V32_CARRIER_SAMPLE_RATE,
	.symbol_rate_numerator = V32_CARRIER_SYMBOL_RATE,
	.symbol_rate_denominator = 1,
	.carrier_frequency_numerator = V32_CARRIER_FREQUENCY_HZ,
	.carrier_frequency_denominator = 1,
	.rolloff = 0.25,
	.pulse_span_symbols = V32_CARRIER_PULSE_SPAN_SYMBOLS,
};

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
	struct v32_receive_path path;
	struct v32_line_receiver initial_line;
	struct v32_equalizer initial_equalizer;
	struct qam_modulator modulator;
	struct qam_point point = { 0.75, 0.75 };
	uint16_t limit = role == V32_CALLING_TRANSMITTER ?
		V32_CALLING_ECHO_TRAINING_SYMBOLS :
		V32_ANSWERING_ECHO_TRAINING_SYMBOLS;
	size_t line_samples = 0;
	bool saw_prefill_end = false;
	bool saw_coarse = false;
	bool saw_coarse_end = false;
	bool saw_fine = false;
	bool saw_trn_end = false;

	CHECK(v32_echo_training_init(&training, role) ==
		      V32_ECHO_TRAINING_OK &&
	      v32_receive_path_init(
		      &path, 0, V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_RECEIVE_PATH_OK &&
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
			uint16_t symbol = training.symbols;

			point.in_phase = (symbol & 1u) != 0 ? -0.75 : 0.75;
			point.quadrature = (symbol & 2u) != 0 ? -0.75 : 0.75;
			if (!training.complete && symbol < limit) {
				CHECK(v32_echo_training_begin_symbol(
					      &training, &path) ==
					      V32_ECHO_TRAINING_OK,
				      "role %d stopped before TRN symbol %u",
				      role, symbol);
			} else if (!training.complete) {
				CHECK(v32_echo_training_finish(
					      &training, &path) ==
					      V32_ECHO_TRAINING_COMPLETE,
				      "role %d did not finish at %u", role,
				      symbol);
			}
			CHECK(v32_echo_reference_init(
				      &reference, &point,
				      modulator.carrier_phase) == V32_ECHO_OK,
			      "role %d did not type reference %u", role,
			      symbol);

			if (training.symbols ==
			    V32_ECHO_TRAINING_PREFILL_SYMBOLS) {
				CHECK(path.echo_training &&
				      !path.echo.echo.adapting &&
				      coefficients_are_zero(&path.echo.echo),
				      "role %d changed its prefill boundary", role);
				saw_prefill_end = true;
			} else if (training.symbols ==
				   V32_ECHO_TRAINING_PREFILL_SYMBOLS + 1u) {
				CHECK(path.echo.echo.adapting &&
				      path.echo.echo.adaptation == 0.65,
				      "role %d did not enter coarse adaptation", role);
				saw_coarse = true;
			} else if (training.symbols ==
				   V32_ECHO_TRAINING_PREFILL_SYMBOLS +
					   V32_ECHO_TRAINING_COARSE_SYMBOLS) {
				CHECK(path.echo.echo.adapting &&
				      path.echo.echo.adaptation == 0.65,
				      "role %d shortened coarse adaptation", role);
				saw_coarse_end = true;
			} else if (training.symbols ==
				   V32_ECHO_TRAINING_PREFILL_SYMBOLS +
					   V32_ECHO_TRAINING_COARSE_SYMBOLS + 1u) {
				CHECK(path.echo.echo.adapting &&
				      path.echo.echo.adaptation == 0.065,
				      "role %d did not reduce adaptation", role);
				saw_fine = true;
			}
			if (training.symbols == limit && !training.complete) {
				CHECK(path.echo.echo.adapting &&
				      path.echo.echo.adaptation == 0.065,
				      "role %d changed its final TRN symbol", role);
				saw_trn_end = true;
			}
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
	CHECK(training.symbols == limit && training.complete &&
	      saw_prefill_end && saw_coarse && saw_coarse_end && saw_fine &&
	      saw_trn_end && !path.echo_training &&
	      !path.echo.echo.adapting && !path.line.input_paused &&
	      !coefficients_are_zero(&path.echo.echo) &&
	      memcmp(&path.line.gain, &initial_line.gain,
		     sizeof(path.line.gain)) == 0 &&
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

	CHECK(v32_echo_training_init(
		      NULL, V32_CALLING_TRANSMITTER) ==
		      V32_ECHO_TRAINING_INVALID_ARGUMENT &&
	      v32_echo_training_init(
		      &training, (enum v32_transmitter_role)2) ==
		      V32_ECHO_TRAINING_INVALID_ARGUMENT &&
	      v32_echo_training_init(
		      &training, V32_CALLING_TRANSMITTER) ==
		      V32_ECHO_TRAINING_OK &&
	      v32_receive_path_init(
		      &path, 0, V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_RECEIVE_PATH_OK,
	      "echo-training contract fixture did not initialize");
	saved_training = training;
	saved_path = path;
	CHECK(v32_echo_training_begin_symbol(NULL, &path) ==
		      V32_ECHO_TRAINING_INVALID_ARGUMENT &&
	      v32_echo_training_begin_symbol(&training, NULL) ==
		      V32_ECHO_TRAINING_INVALID_ARGUMENT &&
	      v32_echo_training_begin_symbol(
		      (struct v32_echo_training *)&path, &path) ==
		      V32_ECHO_TRAINING_INVALID_ARGUMENT &&
	      v32_echo_training_finish(&training, &path) ==
		      V32_ECHO_TRAINING_INVALID_STATE &&
	      memcmp(&training, &saved_training, sizeof(training)) == 0 &&
	      memcmp(&path, &saved_path, sizeof(path)) == 0,
	      "rejected echo-training call changed an owner");
	CHECK(v32_echo_training_begin_symbol(&training, &path) ==
		      V32_ECHO_TRAINING_OK,
	      "echo-training contract fixture did not start");
	saved_training = training;
	saved_path = path;
	path.echo_training = false;
	CHECK(!v32_echo_training_path_is_valid(&training, &path) &&
	      v32_echo_training_begin_symbol(&training, &path) ==
		      V32_ECHO_TRAINING_INVALID_STATE &&
	      memcmp(&training, &saved_training, sizeof(training)) == 0,
	      "mismatched echo-training owners were spliced");
	path = saved_path;
	training = *complete;
	path = *complete_path;
	saved_training = training;
	saved_path = path;
	CHECK(v32_echo_training_begin_symbol(&training, &path) ==
		      V32_ECHO_TRAINING_INVALID_STATE &&
	      v32_echo_training_finish(&training, &path) ==
		      V32_ECHO_TRAINING_INVALID_STATE &&
	      memcmp(&training, &saved_training, sizeof(training)) == 0 &&
	      memcmp(&path, &saved_path, sizeof(path)) == 0,
	      "complete echo training advanced twice");
	training.symbols++;
	CHECK(!v32_echo_training_is_valid(&training),
	      "echo training accepted an out-of-range count");
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
	CHECK(calling.symbols == V32_CALLING_ECHO_TRAINING_SYMBOLS &&
	      answering.symbols == V32_ANSWERING_ECHO_TRAINING_SYMBOLS,
	      "role-specific echo-training lengths changed");
	puts("V.32 echo training tests passed");
	return 0;
}
