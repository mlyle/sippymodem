// SPDX-License-Identifier: BSD-3-Clause

#include <carrier.h>
#include <qam.h>
#include <receive_path.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
	COMPOSED_LINE_SAMPLES = 6000,
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

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static bool sample_random_qam(
	struct v32_receive_path *path, struct qam_modulator *modulator,
	struct qam_point *point, uint32_t *random,
	struct v32_equalizer_observation *observation,
	enum v32_receive_path_result *result)
{
	bool can_accept = qam_modulator_can_accept_symbol(modulator);
	bool accepted;
	struct v32_echo_reference reference;
	int16_t transmit;

	if (can_accept) {
		uint32_t bits = fixture_random(random);

		point->in_phase = (bits & 1u) != 0 ? -0.75 : 0.75;
		point->quadrature = (bits & 2u) != 0 ? -0.75 : 0.75;
		if (v32_echo_reference_init(
			    &reference, point, modulator->carrier_phase) !=
		    V32_ECHO_OK)
			return false;
	}
	if (qam_modulator_sample(
		    modulator, point, &transmit, &accepted) != QAM_OK ||
	    accepted != can_accept)
		return false;
	*result = v32_receive_path_sample(
		path, transmit, accepted ? &reference : NULL, observation);
	return true;
}

static struct v32_equalizer_decision self_decision(
	const struct v32_equalizer_observation *observation)
{
	return (struct v32_equalizer_decision){
		.observation = *observation,
		.ideal_phase = observation->phase,
		.ideal_magnitude = observation->magnitude,
	};
}

static uint64_t hash_word(uint64_t hash, uint16_t word)
{
	hash = (hash ^ (word & UINT16_C(0xff))) *
		UINT64_C(1099511628211);
	return (hash ^ (word >> 8)) * UINT64_C(1099511628211);
}

static uint64_t hash_observation(
	uint64_t hash, const struct v32_equalizer_observation *observation)
{
	hash = hash_word(hash, (uint16_t)observation->i);
	hash = hash_word(hash, (uint16_t)observation->q);
	hash = hash_word(hash, observation->phase);
	hash = hash_word(hash, observation->magnitude);
	for (unsigned int shift = 0; shift < 64; shift += 16)
		hash = hash_word(hash, (uint16_t)(observation->epoch >> shift));
	for (unsigned int shift = 0; shift < 64; shift += 16)
		hash = hash_word(hash, (uint16_t)(observation->lineage >> shift));
	return hash;
}

static bool consume_control_samples(
	struct v32_equalizer *equalizer,
	const int16_t *samples, size_t count,
	uint64_t *hash, size_t *observations)
{
	size_t index = 0;

	while (index < count) {
		struct v32_equalizer_observation observation;
		struct v32_equalizer_decision decision;
		size_t consumed;
		enum v32_equalizer_result result = v32_equalizer_process(
			equalizer, samples + index, count - index,
			&consumed, &observation);

		CHECK(result == V32_EQUALIZER_OK ||
		      result == V32_EQUALIZER_OBSERVATION_READY,
		      "control equalizer rejected recovered samples");
		CHECK(consumed != 0, "control equalizer did not advance");
		index += consumed;
		if (result != V32_EQUALIZER_OBSERVATION_READY)
			continue;
		*hash = hash_observation(*hash, &observation);
		(*observations)++;
		decision = self_decision(&observation);
		CHECK(v32_equalizer_commit(equalizer, &decision) ==
			      V32_EQUALIZER_OK,
		      "control equalizer rejected its decision");
	}
	return true;
}

static bool consume_path_observations(
	struct v32_receive_path *path,
	enum v32_receive_path_result result,
	struct v32_equalizer_observation *observation,
	uint64_t *hash, size_t *observations)
{
	while (result == V32_RECEIVE_PATH_OBSERVATION_READY) {
		struct v32_equalizer_decision decision =
			self_decision(observation);

		*hash = hash_observation(*hash, observation);
		(*observations)++;
		result = v32_receive_path_commit(
			path, &decision, observation);
	}
	CHECK(result == V32_RECEIVE_PATH_OK,
	      "receive path stopped while draining: %d", result);
	return true;
}

static bool test_composed_path(void)
{
	struct v32_receive_path path;
	struct v32_echo_frontend control_echo;
	struct v32_line_receiver control_line;
	struct v32_equalizer control_equalizer;
	struct qam_modulator modulator;
	struct qam_point point = { 0.75, 0.75 };
	uint32_t random = UINT32_C(0x2468ace1);
	uint64_t path_hash = UINT64_C(14695981039346656037);
	uint64_t control_hash = UINT64_C(14695981039346656037);
	size_t path_observations = 0;
	size_t control_observations = 0;

	CHECK(v32_receive_path_init(
		      &path, 0, V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_RECEIVE_PATH_OK &&
	      v32_echo_frontend_init(
		      &control_echo, 0,
		      V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) == V32_ECHO_OK &&
	      v32_line_receiver_init(&control_line) ==
		      V32_LINE_RECEIVER_OK &&
	      v32_equalizer_init(&control_equalizer) == V32_EQUALIZER_OK &&
	      qam_modulator_init(&modulator, &V32_CHANNEL, 12000) == QAM_OK,
	      "composed receive paths did not initialize");

	for (size_t sample = 0; sample < COMPOSED_LINE_SAMPLES; ++sample) {
		bool can_accept = qam_modulator_can_accept_symbol(&modulator);
		bool accepted;
		bool cleaned_ready;
		struct v32_echo_reference reference;
		struct v32_equalizer_observation observation;
		int16_t transmit;
		int16_t recovered[V32_RECEIVE_PATH_RECOVERED_CAPACITY];
		double cleaned;
		size_t recovered_count;
		enum v32_receive_path_result result;

		if (can_accept) {
			uint32_t bits = fixture_random(&random);

			point.in_phase = (bits & 1u) != 0 ? -0.75 : 0.75;
			point.quadrature = (bits & 2u) != 0 ? -0.75 : 0.75;
			CHECK(v32_echo_reference_init(
				      &reference, &point,
				      modulator.carrier_phase) == V32_ECHO_OK,
			      "composed reference failed at %zu", sample);
		}
		CHECK(qam_modulator_sample(
			      &modulator, &point, &transmit, &accepted) == QAM_OK &&
		      accepted == can_accept,
		      "composed transmit clock failed at %zu", sample);
		CHECK(v32_echo_frontend_sample(
			      &control_echo, transmit,
			      accepted ? &reference : NULL,
			      &cleaned, &cleaned_ready) == V32_ECHO_OK,
		      "control echo path failed at %zu", sample);
		if (cleaned_ready) {
			CHECK(v32_line_receiver_process(
				      &control_line, &cleaned, 1, false,
				      recovered, ARRAY_LENGTH(recovered),
				      &recovered_count) == V32_LINE_RECEIVER_OK &&
			      consume_control_samples(
				      &control_equalizer, recovered,
				      recovered_count, &control_hash,
				      &control_observations),
			      "control receive path failed at %zu", sample);
		}
		result = v32_receive_path_sample(
			&path, transmit, accepted ? &reference : NULL,
			&observation);
		CHECK(consume_path_observations(
			      &path, result, &observation,
			      &path_hash, &path_observations),
		      "owned receive path failed at %zu", sample);
	}

	CHECK(path_observations > 100 &&
	      path_observations == control_observations &&
	      path_hash == control_hash &&
	      memcmp(&path.echo, &control_echo, sizeof(control_echo)) == 0 &&
	      memcmp(&path.line, &control_line, sizeof(control_line)) == 0 &&
	      memcmp(
		      &path.equalizer, &control_equalizer,
		      sizeof(control_equalizer)) == 0 &&
	      path.recovered_count == 0 && path.recovered_index == 0 &&
	      v32_receive_path_is_valid(&path),
	      "composed path diverged after %zu/%zu observations",
	      path_observations, control_observations);
	return true;
}

static bool test_buffered_commits(void)
{
	union {
		struct v32_equalizer_decision decision;
		struct v32_equalizer_observation observation;
	} overlapping = { 0 };
	struct v32_receive_path path;
	struct v32_receive_path saved;
	struct v32_equalizer_observation observation;
	struct v32_equalizer_observation output;
	struct v32_equalizer_observation saved_output;
	struct v32_equalizer_decision decision;
	unsigned char saved_overlap[sizeof(overlapping)];
	int16_t first = 123;
	size_t consumed;

	CHECK(v32_receive_path_init(
		      &path, 0, V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_RECEIVE_PATH_OK &&
	      v32_equalizer_process(
		      &path.equalizer, &first, 1,
		      &consumed, &observation) ==
		      V32_EQUALIZER_OBSERVATION_READY && consumed == 1,
	      "buffered path did not reach its first decision");
	for (size_t index = 0; index < 6; ++index)
		path.recovered[index] = (int16_t)(200 + 17 * (int)index);
	path.recovered_count = 6;
	CHECK(v32_receive_path_is_valid(&path),
	      "buffered path was not valid");

	decision = self_decision(&observation);
	memset(&output, 0x5a, sizeof(output));
	saved_output = output;
	saved = path;
	decision.observation.epoch++;
	CHECK(v32_receive_path_commit(&path, &decision, &output) ==
		      V32_RECEIVE_PATH_INVALID_ARGUMENT &&
	      memcmp(&path, &saved, sizeof(path)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0,
	      "rejected buffered decision changed state or output");
	decision = self_decision(&observation);
	overlapping.decision = decision;
	memcpy(saved_overlap, &overlapping, sizeof(saved_overlap));
	CHECK(v32_receive_path_sample(&path, 0, NULL, &output) ==
		      V32_RECEIVE_PATH_DECISION_PENDING &&
	      memcmp(&path, &saved, sizeof(path)) == 0 &&
	      v32_receive_path_commit(&path, NULL, &output) ==
		      V32_RECEIVE_PATH_INVALID_ARGUMENT &&
	      v32_receive_path_commit(&path, &decision, NULL) ==
		      V32_RECEIVE_PATH_INVALID_ARGUMENT &&
	      v32_receive_path_commit(
		      &path,
		      (const struct v32_equalizer_decision *)&path,
		      &output) == V32_RECEIVE_PATH_INVALID_ARGUMENT &&
	      v32_receive_path_commit(
		      &path, &decision,
		      (struct v32_equalizer_observation *)&path) ==
		      V32_RECEIVE_PATH_INVALID_ARGUMENT &&
	      v32_receive_path_commit(
		      &path, &overlapping.decision,
		      &overlapping.observation) ==
		      V32_RECEIVE_PATH_INVALID_ARGUMENT &&
	      memcmp(&path, &saved, sizeof(path)) == 0 &&
	      memcmp(&overlapping, saved_overlap,
		     sizeof(saved_overlap)) == 0,
	      "pending path accepted another sample or overlapping commit");

	CHECK(v32_receive_path_commit(&path, &decision, &output) ==
		      V32_RECEIVE_PATH_OBSERVATION_READY &&
	      path.recovered_index == 3 && path.recovered_count == 6,
	      "first buffered decision did not retain three samples");
	decision = self_decision(&output);
	CHECK(v32_receive_path_commit(&path, &decision, &output) ==
		      V32_RECEIVE_PATH_OBSERVATION_READY &&
	      path.recovered_index == 0 && path.recovered_count == 0,
	      "second buffered decision did not consume the remainder");
	decision = self_decision(&output);
	memset(&output, 0x5a, sizeof(output));
	saved_output = output;
	CHECK(v32_receive_path_commit(&path, &decision, &output) ==
		      V32_RECEIVE_PATH_OK &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0 &&
	      !path.equalizer.decision_pending &&
	      v32_receive_path_is_valid(&path),
	      "final buffered decision did not reopen the line path");
	return true;
}

static bool test_boundary_adaptation(void)
{
	struct v32_receive_path path;
	struct v32_receive_path saved;
	struct v32_equalizer_observation observation;
	struct v32_equalizer_observation output;
	struct v32_equalizer_observation saved_output;
	struct v32_equalizer_decision decision;
	int16_t first = 123;
	int16_t pending[3] = { 123, 234, 345 };
	size_t consumed;
	size_t pending_count;

	CHECK(v32_receive_path_init(
		      &path, 0, V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_RECEIVE_PATH_OK &&
	      v32_equalizer_process(
		      &path.equalizer, &first, 1,
		      &consumed, &observation) ==
		      V32_EQUALIZER_OBSERVATION_READY && consumed == 1,
	      "adaptation boundary did not reach its first decision");
	for (size_t index = 0; index < 6; ++index)
		path.recovered[index] = (int16_t)(200 + 17 * (int)index);
	path.recovered_count = 6;
	decision = self_decision(&observation);
	CHECK(v32_receive_path_commit_adaptation(
		      &path, &decision,
		      V32_EQUALIZER_ADAPTATION_FINE, &output) ==
		      V32_RECEIVE_PATH_OBSERVATION_READY &&
	      path.equalizer.adaptation ==
		      V32_EQUALIZER_ADAPTATION_FINE &&
	      path.recovered_index == 3 && path.recovered_count == 6 &&
	      output.lineage == path.equalizer.lineage,
	      "fine adaptation did not precede buffered samples");

	decision = self_decision(&output);
	saved = path;
	saved_output = output;
	CHECK(v32_receive_path_commit_adaptation(
		      &path, &decision,
		      (enum v32_equalizer_adaptation)3, &output) ==
		      V32_RECEIVE_PATH_INVALID_ARGUMENT &&
	      memcmp(&path, &saved, sizeof(path)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0,
	      "rejected boundary adaptation changed state or output");
	CHECK(v32_receive_path_commit(&path, &decision, &output) ==
		      V32_RECEIVE_PATH_OBSERVATION_READY,
	      "boundary path did not resume after rejection");
	decision = self_decision(&output);
	memset(&output, 0x5a, sizeof(output));
	saved_output = output;
	CHECK(v32_receive_path_commit(&path, &decision, &output) ==
		      V32_RECEIVE_PATH_OK &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0 &&
	      path.equalizer.adaptation ==
		      V32_EQUALIZER_ADAPTATION_FINE &&
	      v32_receive_path_is_valid(&path),
	      "boundary path did not drain after adaptation");
	CHECK(v32_receive_path_set_adaptation(
		      &path, V32_EQUALIZER_ADAPTATION_DISABLED) ==
		      V32_RECEIVE_PATH_OK &&
	      path.equalizer.adaptation ==
		      V32_EQUALIZER_ADAPTATION_DISABLED,
	      "open receive path did not disable adaptation");
	saved = path;
	CHECK(v32_receive_path_set_adaptation(
		      &path, (enum v32_equalizer_adaptation)3) ==
		      V32_RECEIVE_PATH_INVALID_ARGUMENT &&
	      memcmp(&path, &saved, sizeof(path)) == 0,
	      "invalid open-path adaptation changed state");
	pending_count = path.equalizer.samples_until_observation;
	CHECK(v32_equalizer_process(
		      &path.equalizer, pending, pending_count,
		      &consumed, &observation) ==
		      V32_EQUALIZER_OBSERVATION_READY &&
	      consumed == pending_count &&
	      v32_receive_path_set_adaptation(
		      &path, V32_EQUALIZER_ADAPTATION_FINE) ==
		      V32_RECEIVE_PATH_DECISION_PENDING &&
	      path.equalizer.adaptation ==
		      V32_EQUALIZER_ADAPTATION_DISABLED,
	      "pending receive path accepted a lifecycle change");
	return true;
}

static bool test_echo_adaptation_lifecycle(void)
{
	struct v32_receive_path path;
	struct v32_receive_path saved;
	struct v32_echo_frontend expected_echo;
	struct v32_line_receiver initial_line;
	struct v32_equalizer initial_equalizer;
	struct v32_equalizer_observation observation;
	struct qam_modulator modulator;
	struct qam_point point = { 0.75, 0.75 };
	uint32_t random = UINT32_C(0x13579bdf);
	size_t training_samples = 0;
	bool prefill_changed = false;
	bool coefficients_changed = false;

	CHECK(v32_receive_path_init(
		      &path, 0, V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_RECEIVE_PATH_OK &&
	      qam_modulator_init(&modulator, &V32_CHANNEL, 12000) == QAM_OK,
	      "echo lifecycle fixture did not initialize");
	initial_line = path.line;
	initial_equalizer = path.equalizer;
	CHECK(v32_receive_path_begin_echo_training(&path) ==
		      V32_RECEIVE_PATH_OK && path.echo_training &&
	      !path.echo.echo.adapting,
	      "receive path did not start its echo prefill");

	while (path.echo.echo.reference_fill < 64u ||
	       path.echo.echo.reference_pending) {
		enum v32_receive_path_result result =
			V32_RECEIVE_PATH_INVALID_STATE;

		CHECK(sample_random_qam(
			      &path, &modulator, &point, &random,
			      &observation, &result) &&
		      result == V32_RECEIVE_PATH_OK,
		      "echo prefill exposed receive data at %zu: %d",
		      training_samples, result);
		training_samples++;
	}
	for (size_t phase = 0; phase < V32_ECHO_PHASE_COUNT; ++phase) {
		for (size_t window = 0; window < V32_ECHO_WINDOW_COUNT;
		     ++window) {
			for (size_t tap = 0; tap < V32_ECHO_WINDOW_SYMBOLS;
			     ++tap) {
				const struct qam_point *coefficient =
					&path.echo.echo.coefficients[phase][window][tap];

				prefill_changed |= coefficient->in_phase != 0.0 ||
					coefficient->quadrature != 0.0;
			}
		}
	}
	CHECK(!prefill_changed && path.line.input_paused &&
	      memcmp(&path.line.gain, &initial_line.gain,
		     sizeof(path.line.gain)) == 0 &&
	      memcmp(&path.line.timing, &initial_line.timing,
		     sizeof(path.line.timing)) == 0 &&
	      memcmp(&path.equalizer, &initial_equalizer,
		     sizeof(path.equalizer)) == 0,
	      "echo prefill trained or clocked the downstream lane");
	CHECK(v32_receive_path_start_echo_adaptation(&path, 0.65) ==
		      V32_RECEIVE_PATH_OK && path.echo.echo.adapting,
	      "receive path did not start echo adaptation after prefill");

	while (path.echo.echo.reference_fill < 192u ||
	       path.echo.echo.reference_pending) {
		enum v32_receive_path_result result =
			V32_RECEIVE_PATH_INVALID_STATE;

		CHECK(sample_random_qam(
			      &path, &modulator, &point, &random,
			      &observation, &result) &&
		      result == V32_RECEIVE_PATH_OK,
		      "echo adaptation exposed receive data at %zu: %d",
		      training_samples, result);
		training_samples++;
		CHECK(training_samples < 2000,
		      "echo lifecycle did not reach a group boundary");
	}
	for (size_t phase = 0; phase < V32_ECHO_PHASE_COUNT; ++phase) {
		for (size_t window = 0; window < V32_ECHO_WINDOW_COUNT;
		     ++window) {
			for (size_t tap = 0; tap < V32_ECHO_WINDOW_SYMBOLS;
			     ++tap) {
				const struct qam_point *coefficient =
					&path.echo.echo.coefficients[phase][window][tap];

				coefficients_changed |= coefficient->in_phase != 0.0 ||
					coefficient->quadrature != 0.0;
			}
		}
	}
	CHECK(coefficients_changed && path.line.input_paused &&
	      path.line.epoch_fill == 0 &&
	      memcmp(&path.line.gain, &initial_line.gain,
		     sizeof(path.line.gain)) == 0 &&
	      memcmp(&path.line.timing, &initial_line.timing,
		     sizeof(path.line.timing)) == 0 &&
	      memcmp(&path.equalizer, &initial_equalizer,
		     sizeof(path.equalizer)) == 0,
	      "echo adaptation changed the paused receive lane");

	expected_echo = path.echo;
	expected_echo.echo.adaptation = 0.065;
	CHECK(v32_receive_path_start_echo_adaptation(&path, 0.065) ==
		      V32_RECEIVE_PATH_OK &&
	      memcmp(&path.echo, &expected_echo, sizeof(expected_echo)) == 0,
	      "reduced echo adaptation changed learned state or cadence");
	expected_echo.echo.adapting = false;
	CHECK(v32_receive_path_finish_echo_training(&path) ==
		      V32_RECEIVE_PATH_OK &&
	      !path.echo_training &&
	      memcmp(&path.echo, &expected_echo, sizeof(expected_echo)) == 0 &&
	      v32_receive_path_is_valid(&path),
	      "echo freeze changed learned state or cadence");

	while (path.line.input_paused) {
		enum v32_receive_path_result result =
			V32_RECEIVE_PATH_INVALID_STATE;

		CHECK(sample_random_qam(
			      &path, &modulator, &point, &random,
			      &observation, &result) &&
		      result == V32_RECEIVE_PATH_OK,
		      "frozen echo path did not resume");
	}
	CHECK(path.line.epoch_fill == 1 &&
	      memcmp(&path.line.gain, &initial_line.gain,
		     sizeof(path.line.gain)) == 0 &&
	      memcmp(&path.line.timing, &initial_line.timing,
		     sizeof(path.line.timing)) == 0 &&
	      memcmp(&path.equalizer, &initial_equalizer,
		     sizeof(path.equalizer)) == 0,
	      "post-echo receive evidence did not resume from its boundary");
	while (path.echo.echo.reference_pending) {
		enum v32_receive_path_result result =
			V32_RECEIVE_PATH_INVALID_STATE;

		CHECK(sample_random_qam(
			      &path, &modulator, &point, &random,
			      &observation, &result) &&
		      result == V32_RECEIVE_PATH_OK,
		      "repeated echo lifecycle did not reach a group boundary");
	}

	CHECK(v32_receive_path_start_echo_adaptation(&path, 0.0) ==
		      V32_RECEIVE_PATH_INVALID_STATE &&
	      v32_receive_path_begin_echo_training(&path) ==
		      V32_RECEIVE_PATH_OK,
	      "finished path did not require a new echo-training boundary");
	saved = path;
	CHECK(v32_receive_path_start_echo_adaptation(&path, 0.0) ==
		      V32_RECEIVE_PATH_INVALID_ARGUMENT &&
	      v32_receive_path_start_echo_adaptation(&path, NAN) ==
		      V32_RECEIVE_PATH_INVALID_ARGUMENT &&
	      memcmp(&path, &saved, sizeof(path)) == 0,
	      "invalid echo adaptation changed the receive path");
	return true;
}

static bool test_contracts(void)
{
	union {
		struct v32_echo_reference reference;
		struct v32_equalizer_observation observation;
	} overlapping = { 0 };
	struct v32_receive_path path;
	struct v32_receive_path saved;
	struct v32_equalizer_observation output;
	struct v32_equalizer_observation saved_output;
	struct v32_echo_reference reference;
	struct qam_point point = { 0.5, -0.5 };
	unsigned char saved_overlap[sizeof(overlapping)];
	int16_t first = 123;
	size_t consumed;

	CHECK(v32_receive_path_init(
		      NULL, 0, V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_RECEIVE_PATH_INVALID_ARGUMENT &&
	      v32_receive_path_init(
		      &path, 0, V32_ECHO_MIN_ROUND_TRIP_SYMBOLS - 1u) ==
		      V32_RECEIVE_PATH_INVALID_ARGUMENT &&
	      v32_receive_path_init(
		      &path, 0, V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_RECEIVE_PATH_OK &&
	      v32_echo_reference_init(&reference, &point, 0) == V32_ECHO_OK,
	      "receive-path validation fixture did not initialize");
	CHECK(v32_receive_path_begin_echo_training(NULL) ==
		      V32_RECEIVE_PATH_INVALID_STATE &&
	      v32_receive_path_start_echo_adaptation(NULL, 0.2) ==
		      V32_RECEIVE_PATH_INVALID_STATE &&
	      v32_receive_path_finish_echo_training(NULL) ==
		      V32_RECEIVE_PATH_INVALID_STATE &&
	      v32_receive_path_start_echo_adaptation(&path, 0.2) ==
		      V32_RECEIVE_PATH_INVALID_STATE &&
	      v32_receive_path_finish_echo_training(&path) ==
		      V32_RECEIVE_PATH_INVALID_STATE &&
	      v32_receive_path_begin_echo_training(&path) ==
		      V32_RECEIVE_PATH_OK &&
	      v32_receive_path_begin_echo_training(&path) ==
		      V32_RECEIVE_PATH_INVALID_STATE &&
	      v32_receive_path_start_echo_adaptation(&path, 0.2) ==
		      V32_RECEIVE_PATH_OK &&
	      v32_receive_path_sample(&path, 0, &reference, &output) ==
		      V32_RECEIVE_PATH_OK,
	      "echo lifecycle boundary fixture did not advance");
	saved = path;
	CHECK(v32_receive_path_finish_echo_training(&path) ==
		      V32_RECEIVE_PATH_INVALID_STATE &&
	      memcmp(&path, &saved, sizeof(path)) == 0,
	      "mid-group echo freeze changed the receive path");
	CHECK(v32_receive_path_init(
		      &path, 0, V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_RECEIVE_PATH_OK &&
	      v32_equalizer_process(
		      &path.equalizer, &first, 1, &consumed, &output) ==
		      V32_EQUALIZER_OBSERVATION_READY && consumed == 1,
	      "pending echo lifecycle fixture did not initialize");
	saved = path;
	CHECK(v32_receive_path_begin_echo_training(&path) ==
		      V32_RECEIVE_PATH_DECISION_PENDING &&
	      v32_receive_path_start_echo_adaptation(&path, 0.2) ==
		      V32_RECEIVE_PATH_DECISION_PENDING &&
	      v32_receive_path_finish_echo_training(&path) ==
		      V32_RECEIVE_PATH_DECISION_PENDING &&
	      memcmp(&path, &saved, sizeof(path)) == 0,
	      "pending decision allowed an echo lifecycle transition");
	CHECK(v32_receive_path_init(
		      &path, 0, V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_RECEIVE_PATH_OK,
	      "receive-path validation fixture did not reset");
	memset(&output, 0x5a, sizeof(output));
	saved_output = output;
	saved = path;
	overlapping.reference = reference;
	memcpy(saved_overlap, &overlapping, sizeof(saved_overlap));
	CHECK(v32_receive_path_sample(NULL, 0, NULL, &output) ==
		      V32_RECEIVE_PATH_INVALID_STATE &&
	      v32_receive_path_sample(&path, 0, NULL, &output) ==
		      V32_RECEIVE_PATH_INVALID_STATE &&
	      v32_receive_path_sample(&path, 0, &reference, NULL) ==
		      V32_RECEIVE_PATH_INVALID_ARGUMENT &&
	      v32_receive_path_sample(
		      &path, 0, &reference,
		      (struct v32_equalizer_observation *)&path) ==
		      V32_RECEIVE_PATH_INVALID_ARGUMENT &&
	      v32_receive_path_sample(
		      &path, 0,
		      (const struct v32_echo_reference *)&path, &output) ==
		      V32_RECEIVE_PATH_INVALID_ARGUMENT &&
	      v32_receive_path_sample(
		      &path, 0, &overlapping.reference,
		      &overlapping.observation) ==
		      V32_RECEIVE_PATH_INVALID_ARGUMENT &&
	      memcmp(&path, &saved, sizeof(path)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0 &&
	      memcmp(&overlapping, saved_overlap,
		     sizeof(saved_overlap)) == 0,
	      "receive-path rejection changed state or output");
	path.recovered_count = V32_RECEIVE_PATH_RECOVERED_CAPACITY + 1u;
	CHECK(!v32_receive_path_is_valid(&path) &&
	      v32_receive_path_sample(&path, 0, &reference, &output) ==
		      V32_RECEIVE_PATH_INVALID_STATE,
	      "receive path accepted corrupt buffer state");
	return true;
}

int main(void)
{
	if (!test_composed_path() || !test_buffered_commits() ||
	    !test_boundary_adaptation() ||
	    !test_echo_adaptation_lifecycle() || !test_contracts())
		return 1;
	puts("V.32 receive path tests passed");
	return 0;
}
