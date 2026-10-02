// SPDX-License-Identifier: BSD-3-Clause

#include <echo_frontend.h>
#include <line_receiver.h>
#include <qam.h>

#include "test.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
	STREAM_SAMPLES = 197,
	STREAM_OUTPUT_SAMPLES = 256,
	ECHO_HISTORY = 32,
	ECHO_TRAIN_INPUTS = 4000,
	ECHO_MEASURE_INPUTS = 400,
};

static const struct qam_channel V32_CHANNEL = {
	.sample_rate = V32_ECHO_FRONTEND_INPUT_RATE,
	.symbol_rate_numerator = 2400,
	.symbol_rate_denominator = 1,
	.carrier_frequency_numerator = 1800,
	.carrier_frequency_denominator = 1,
	.rolloff = 0.25,
	.pulse_span_symbols = 6,
};

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static void fill_stream(double *samples, size_t count, uint32_t seed)
{
	for (size_t index = 0; index < count; ++index) {
		uint32_t value = fixture_random(&seed);
		int32_t centered = (int32_t)(value % 24001u) - 12000;

		samples[index] = centered + ((value >> 28) & 1u ? 0.25 : -0.25);
	}
}

static uint64_t hash_i16(
	uint64_t hash, const int16_t *samples, size_t count)
{
	for (size_t index = 0; index < count; ++index) {
		uint16_t word = (uint16_t)samples[index];

		hash = (hash ^ (word & UINT16_C(0xff))) *
			UINT64_C(1099511628211);
		hash = (hash ^ (word >> 8)) * UINT64_C(1099511628211);
	}
	return hash;
}

static bool run_stream(
	struct v32_line_receiver *receiver,
	const double *samples, size_t count,
	const size_t *chunks, size_t chunk_count,
	int16_t *output, size_t *output_count)
{
	size_t consumed = 0;
	size_t produced = 0;
	size_t chunk = 0;

	while (consumed < count) {
		size_t current = chunks[chunk++ % chunk_count];
		size_t emitted;

		if (current > count - consumed)
			current = count - consumed;
		CHECK(current <= V32_LINE_RECEIVER_EPOCH_SAMPLES,
		      "test chunk exceeded line-receiver bound");
		CHECK(v32_line_receiver_process(
			      receiver, &samples[consumed], current, false,
			      &output[produced],
			      STREAM_OUTPUT_SAMPLES - produced,
			      &emitted) == V32_LINE_RECEIVER_OK,
		      "line receiver rejected chunk %zu", current);
		consumed += current;
		produced += emitted;
	}
	*output_count = produced;
	return true;
}

static bool test_epochs_fragmentation_and_reentrancy(void)
{
	static const size_t whole_chunks[] = {
		V32_LINE_RECEIVER_EPOCH_SAMPLES,
	};
	static const size_t fragments[] = { 1, 7, 19, 3, 36, 2, 11 };
	double samples[STREAM_SAMPLES];
	int16_t reference[STREAM_OUTPUT_SAMPLES] = {0};
	int16_t fragmented[STREAM_OUTPUT_SAMPLES] = {0};
	struct v32_line_receiver sequential;
	struct v32_line_receiver independent;
	size_t reference_count;
	size_t fragmented_count;

	fill_stream(samples, ARRAY_LENGTH(samples), UINT32_C(0x10203040));
	CHECK(v32_line_receiver_init(&sequential) == V32_LINE_RECEIVER_OK &&
	      v32_line_receiver_init(&independent) == V32_LINE_RECEIVER_OK &&
	      run_stream(
		      &sequential, samples, ARRAY_LENGTH(samples),
		      whole_chunks, ARRAY_LENGTH(whole_chunks), reference,
		      &reference_count) &&
	      run_stream(
		      &independent, samples, ARRAY_LENGTH(samples),
		      fragments, ARRAY_LENGTH(fragments), fragmented,
		      &fragmented_count),
	      "independent line streams failed");
	CHECK(reference_count == 180 &&
	      hash_i16(UINT64_C(14695981039346656037), reference,
		       reference_count) == UINT64_C(0x531105c9933a32b9) &&
	      fragmented_count == reference_count &&
	      memcmp(reference, fragmented,
		     reference_count * sizeof(reference[0])) == 0 &&
	      memcmp(&sequential, &independent, sizeof(sequential)) == 0,
	      "fragmentation changed an independent line owner");
	return true;
}

static bool test_sample_rounding(void)
{
	const struct {
		double input;
		int16_t expected;
	} cases[] = {
		{ nextafter(0.5, 0.0), 0 },
		{ 0.5, 1 },
		{ nextafter(0.5, 1.0), 1 },
		{ nextafter(-0.5, 0.0), 0 },
		{ -0.5, -1 },
		{ nextafter(-0.5, -1.0), -1 },
		{ nextafter(1.5, 0.0), 1 },
		{ 1.5, 2 },
		{ nextafter(-1.5, 0.0), -1 },
		{ -1.5, -2 },
		{ 32766.5, INT16_MAX },
		{ -32767.5, INT16_MIN },
		{ 1e100, INT16_MAX },
		{ -1e100, INT16_MIN },
	};
	struct v32_line_receiver receiver;
	size_t produced = 123;

	CHECK(v32_line_receiver_init(&receiver) == V32_LINE_RECEIVER_OK,
	      "rounding fixture did not initialize");
	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		CHECK(v32_line_receiver_process(
			      &receiver, &cases[index].input, 1, false,
			      NULL, 0, &produced) == V32_LINE_RECEIVER_OK &&
		      produced == 0 && receiver.epoch_fill == index + 1u &&
		      receiver.epoch[index] == cases[index].expected,
		      "sample %.17g rounded to %d instead of %d",
		      cases[index].input, receiver.epoch[index],
		      cases[index].expected);
	}
	return true;
}

static bool test_adaptation_and_gain_boundaries(void)
{
	double samples[V32_LINE_RECEIVER_EPOCH_SAMPLES];
	int16_t output[V32_SYMBOL_TIMING_MAX_OUTPUT_SAMPLES];
	struct v32_line_receiver receiver;
	struct v32_line_receiver saved;
	struct agc gain_before_adaptation;
	struct v32_symbol_timing timing_before_adaptation;
	int16_t trained_mantissa;
	int16_t trained_shift;
	size_t produced;

	for (size_t index = 0; index < ARRAY_LENGTH(samples); ++index)
		samples[index] = 4252.0;
	CHECK(v32_line_receiver_init(&receiver) == V32_LINE_RECEIVER_OK &&
	      v32_line_receiver_process(
		      &receiver, samples, 11, false, output,
		      ARRAY_LENGTH(output), &produced) == V32_LINE_RECEIVER_OK &&
	      produced == 0 && receiver.epoch_fill == 11,
	      "line receiver did not retain a partial epoch");
	saved = receiver;
	CHECK(v32_line_receiver_freeze_gain(&receiver) ==
		      V32_LINE_RECEIVER_INVALID_STATE &&
	      v32_line_receiver_reacquire_gain(&receiver) ==
		      V32_LINE_RECEIVER_INVALID_STATE &&
	      v32_line_receiver_reacquire_timing(&receiver) ==
		      V32_LINE_RECEIVER_INVALID_STATE &&
	      v32_line_receiver_set_timing_tracking(&receiver, false) ==
		      V32_LINE_RECEIVER_INVALID_STATE &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "mid-epoch receive transition lost hidden timing state");
	gain_before_adaptation = receiver.gain;
	timing_before_adaptation = receiver.timing;
	CHECK(v32_line_receiver_process(
		      &receiver, samples, ARRAY_LENGTH(samples), true,
		      output, ARRAY_LENGTH(output), &produced) ==
		      V32_LINE_RECEIVER_OK &&
	      produced == 0 && receiver.input_paused &&
	      receiver.epoch_fill == 0 &&
	      memcmp(&receiver.gain, &gain_before_adaptation,
		     sizeof(receiver.gain)) == 0 &&
	      memcmp(&receiver.timing, &timing_before_adaptation,
		     sizeof(receiver.timing)) == 0,
	      "echo adaptation clocked receive state or retained an epoch");
	gain_before_adaptation = receiver.gain;
	CHECK(v32_line_receiver_reacquire_timing(&receiver) ==
		      V32_LINE_RECEIVER_OK &&
	      receiver.input_paused && receiver.epoch_fill == 0 &&
	      receiver.timing.samples_until_output == 1 &&
	      receiver.timing.history_index == 0 &&
	      memcmp(&receiver.gain, &gain_before_adaptation,
		     sizeof(receiver.gain)) == 0,
	      "paused line owner did not restart timing at its boundary");
	saved = receiver;
	CHECK(v32_line_receiver_set_timing_tracking(&receiver, false) ==
		      V32_LINE_RECEIVER_INVALID_STATE &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "paused line owner changed timing tracking in place");
	CHECK(v32_line_receiver_process(
		      &receiver, samples, ARRAY_LENGTH(samples) - 1u, false,
		      output, ARRAY_LENGTH(output), &produced) ==
		      V32_LINE_RECEIVER_OK &&
	      produced == 0 && !receiver.input_paused &&
	      receiver.epoch_fill == ARRAY_LENGTH(samples) - 1u &&
	      v32_line_receiver_process(
		      &receiver, samples, 1, false, output,
		      ARRAY_LENGTH(output), &produced) ==
		      V32_LINE_RECEIVER_OK &&
	      produced > 0 && receiver.gain.signal_present &&
	      memcmp(&receiver.timing, &timing_before_adaptation,
		     sizeof(receiver.timing)) != 0,
	      "post-adaptation epoch did not start from fresh evidence");
	trained_mantissa = receiver.gain.gain_mantissa_q15;
	trained_shift = receiver.gain.gain_shift;
	timing_before_adaptation = receiver.timing;
	CHECK(v32_line_receiver_freeze_gain(&receiver) ==
		      V32_LINE_RECEIVER_OK &&
	      memcmp(&receiver.timing, &timing_before_adaptation,
		     sizeof(receiver.timing)) == 0 &&
	      v32_line_receiver_process(
		      &receiver, samples, ARRAY_LENGTH(samples), false,
		      output, ARRAY_LENGTH(output), &produced) ==
		      V32_LINE_RECEIVER_OK &&
	      receiver.gain.gain_mantissa_q15 == trained_mantissa &&
	      receiver.gain.gain_shift == trained_shift &&
	      v32_line_receiver_reacquire_gain(&receiver) ==
		      V32_LINE_RECEIVER_OK &&
	      receiver.gain.gain_mode == AGC_GAIN_TRACKING &&
	      receiver.gain.smoothed_level == 0 &&
	      !receiver.gain.signal_present &&
	      receiver.gain.gain_mantissa_q15 == trained_mantissa &&
	      receiver.gain.gain_shift == trained_shift,
	      "line gain freeze/reacquire did not preserve trained gain");
	timing_before_adaptation = receiver.timing;
	CHECK(v32_line_receiver_set_timing_tracking(&receiver, false) ==
		      V32_LINE_RECEIVER_OK &&
	      !receiver.timing.tracking_enabled,
	      "line timing tracking did not freeze at an epoch boundary");
	timing_before_adaptation.tracking_enabled = false;
	CHECK(memcmp(&receiver.timing, &timing_before_adaptation,
		     sizeof(receiver.timing)) == 0 &&
	      v32_line_receiver_reacquire_timing(&receiver) ==
		      V32_LINE_RECEIVER_OK &&
	      receiver.timing.tracking_enabled &&
	      receiver.timing.samples_until_output == 1 &&
	      receiver.timing.history_index == 0,
	      "line timing reacquire did not reset only at its boundary");
	return true;
}

static bool test_line_contracts(void)
{
	static const double nonfinite[] = { NAN, INFINITY, -INFINITY };
	double samples[V32_LINE_RECEIVER_EPOCH_SAMPLES + 1u];
	int16_t output[V32_SYMBOL_TIMING_MAX_OUTPUT_SAMPLES];
	int16_t saved_output[V32_SYMBOL_TIMING_MAX_OUTPUT_SAMPLES];
	struct v32_line_receiver receiver;
	struct v32_line_receiver saved;
	size_t produced = 123;
	size_t saved_produced;

	for (size_t index = 0; index < ARRAY_LENGTH(samples); ++index)
		samples[index] = 4252.0;
	memset(output, 0x6b, sizeof(output));
	memcpy(saved_output, output, sizeof(output));
	CHECK(v32_line_receiver_init(&receiver) == V32_LINE_RECEIVER_OK,
	      "line contract fixture did not initialize");
	saved = receiver;
	saved_produced = produced;
#define CHECK_REJECT(call, label)                                           \
	CHECK((call) == V32_LINE_RECEIVER_INVALID_ARGUMENT &&                 \
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&              \
	      memcmp(output, saved_output, sizeof(output)) == 0 &&             \
	      produced == saved_produced, "%s was not transactional", label)
	CHECK_REJECT(v32_line_receiver_process(
		     &receiver, NULL, 1, false, output,
		     ARRAY_LENGTH(output), &produced),
	     "null input");
	CHECK_REJECT(v32_line_receiver_process(
		     &receiver, samples, ARRAY_LENGTH(samples), false,
		     output, ARRAY_LENGTH(output), &produced),
	     "oversize fragment");
	CHECK_REJECT(v32_line_receiver_process(
		     &receiver, samples, V32_LINE_RECEIVER_EPOCH_SAMPLES,
		     false, output, ARRAY_LENGTH(output) - 1u, &produced),
	     "short output");
	for (size_t index = 0; index < ARRAY_LENGTH(nonfinite); ++index) {
		samples[17] = nonfinite[index];
		CHECK_REJECT(v32_line_receiver_process(
			     &receiver, samples, V32_LINE_RECEIVER_EPOCH_SAMPLES,
			     false, output, ARRAY_LENGTH(output), &produced),
		     "nonfinite input");
	}
	samples[17] = 4252.0;
	CHECK_REJECT(v32_line_receiver_process(
		     &receiver, (const double *)(const void *)&receiver, 1,
		     false, output, ARRAY_LENGTH(output), &produced),
	     "owner/input alias");
	CHECK_REJECT(v32_line_receiver_process(
		     &receiver, samples, V32_LINE_RECEIVER_EPOCH_SAMPLES,
		     false, (int16_t *)(void *)&receiver,
		     V32_SYMBOL_TIMING_MAX_OUTPUT_SAMPLES, &produced),
	     "owner/output alias");
	CHECK_REJECT(v32_line_receiver_process(
		     &receiver, samples, V32_LINE_RECEIVER_EPOCH_SAMPLES,
		     false, (int16_t *)(void *)samples,
		     V32_SYMBOL_TIMING_MAX_OUTPUT_SAMPLES, &produced),
	     "input/output alias");
	CHECK_REJECT(v32_line_receiver_process(
		     &receiver, samples, V32_LINE_RECEIVER_EPOCH_SAMPLES,
		     false, NULL, V32_SYMBOL_TIMING_MAX_OUTPUT_SAMPLES,
		     &produced),
	     "null line output");
	CHECK_REJECT(v32_line_receiver_process(
		     &receiver, samples, V32_LINE_RECEIVER_EPOCH_SAMPLES,
		     false, output, ARRAY_LENGTH(output), NULL),
	     "null line output count");
#undef CHECK_REJECT
	CHECK(v32_line_receiver_process(
		      NULL, samples, 1, false, output,
		      ARRAY_LENGTH(output), &produced) ==
		      V32_LINE_RECEIVER_INVALID_STATE,
	      "null line owner was accepted");
	receiver.epoch_fill = V32_LINE_RECEIVER_EPOCH_SAMPLES;
	saved = receiver;
	CHECK(v32_line_receiver_process(
		      &receiver, samples, 1, false, output,
		      ARRAY_LENGTH(output), &produced) ==
		      V32_LINE_RECEIVER_INVALID_STATE &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "invalid line owner changed during rejection");
	return true;
}

struct echo_path {
	int16_t samples[ECHO_HISTORY];
	size_t write;
};

static int16_t echo_age(const struct echo_path *path, size_t age)
{
	size_t index = path->write + ECHO_HISTORY - 1u - age;

	if (index >= ECHO_HISTORY)
		index -= ECHO_HISTORY;
	return path->samples[index];
}

static int16_t channel_sample(
	struct echo_path *path, int16_t transmit, double remote)
{
	double received;

	path->samples[path->write] = transmit;
	path->write = (path->write + 1u) % ECHO_HISTORY;
	received = 0.45 * echo_age(path, 0) + 0.25 * echo_age(path, 7) -
		   0.15 * echo_age(path, 23) + remote;
	if (received >= 32766.5)
		return INT16_MAX;
	if (received <= -32767.5)
		return INT16_MIN;
	return (int16_t)lround(received);
}

static bool test_composed_echo_line_owner(void)
{
	struct v32_echo_frontend frontend;
	struct v32_line_receiver receiver;
	struct qam_modulator modulator;
	struct qam_point point = { 0.75, 0.75 };
	struct echo_path path = {0};
	struct agc initial_gain;
	uint32_t random = UINT32_C(0x2468ace1);
	size_t normalized_total = 0;
	uint64_t normalized_hash = UINT64_C(14695981039346656037);
	bool any_normalized = false;

	CHECK(v32_echo_frontend_init(
		      &frontend, 0, V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_ECHO_OK &&
	      v32_echo_frontend_start_adaptation(&frontend, 0.65) ==
		      V32_ECHO_OK &&
	      v32_line_receiver_init(&receiver) == V32_LINE_RECEIVER_OK &&
	      qam_modulator_init(&modulator, &V32_CHANNEL, 12000) == QAM_OK,
	      "composed line fixture did not initialize");
	initial_gain = receiver.gain;
	for (size_t sample = 0;
	     sample < ECHO_TRAIN_INPUTS + ECHO_MEASURE_INPUTS; ++sample) {
		bool can_accept = qam_modulator_can_accept_symbol(&modulator);
		bool accepted;
		bool cleaned_ready;
		struct v32_echo_reference reference;
		int16_t transmit;
		int16_t received;
		int16_t normalized[V32_SYMBOL_TIMING_MAX_OUTPUT_SAMPLES];
		double cleaned;
		double remote = sample < ECHO_TRAIN_INPUTS ? 0.0 :
			12000.0 * sin(2.0 * 3.14159265358979323846 * 600.0 *
					  (double)sample / 8000.0);
		size_t produced;

		if (sample == ECHO_TRAIN_INPUTS) {
			CHECK(v32_echo_frontend_freeze(&frontend) == V32_ECHO_OK,
			      "composed echo owner did not freeze on its group boundary");
		}
		if (can_accept) {
			uint32_t bits = fixture_random(&random);

			point.in_phase = (bits & 1u) != 0 ? -0.75 : 0.75;
			point.quadrature = (bits & 2u) != 0 ? -0.75 : 0.75;
			CHECK(v32_echo_reference_init(
				      &reference, &point, modulator.carrier_phase) ==
				      V32_ECHO_OK,
			      "composed echo reference failed at %zu", sample);
		}
		CHECK(qam_modulator_sample(
			      &modulator, &point, &transmit, &accepted) == QAM_OK &&
		      accepted == can_accept,
		      "composed QAM clock failed at %zu", sample);
		received = channel_sample(&path, transmit, remote);
		CHECK(v32_echo_frontend_sample(
			      &frontend, received, accepted ? &reference : NULL,
			      &cleaned, &cleaned_ready) == V32_ECHO_OK,
		      "composed echo frontend failed at %zu", sample);
		if (!cleaned_ready)
			continue;
		CHECK(v32_line_receiver_process(
			      &receiver, &cleaned, 1,
			      sample < ECHO_TRAIN_INPUTS,
			      normalized, ARRAY_LENGTH(normalized), &produced) ==
			      V32_LINE_RECEIVER_OK,
		      "composed line owner failed at %zu", sample);
		if (sample < ECHO_TRAIN_INPUTS) {
			CHECK(produced == 0 &&
			      memcmp(&receiver.gain, &initial_gain,
				     sizeof(initial_gain)) == 0,
			      "echo adaptation clocked composed AGC");
		} else {
			normalized_total += produced;
			normalized_hash = hash_i16(
				normalized_hash, normalized, produced);
			for (size_t index = 0; index < produced; ++index)
				any_normalized |= normalized[index] != 0;
		}
	}
	CHECK(normalized_total == 360 &&
	      receiver.gain.signal_present && any_normalized,
	      "composed post-echo line path: samples=%zu signal=%d nonzero=%d",
	      normalized_total, receiver.gain.signal_present, any_normalized);
	/* Corrected full-width timing trace after echo cancellation. */
	CHECK(normalized_hash == UINT64_C(0x6c450ae27ce4467f),
	      "composed post-echo line snapshot changed: %#llx",
	      (unsigned long long)normalized_hash);
	return true;
}

int main(void)
{
	if (!test_epochs_fragmentation_and_reentrancy() ||
	    !test_sample_rounding() ||
	    !test_adaptation_and_gain_boundaries() ||
	    !test_line_contracts() || !test_composed_echo_line_owner())
		return 1;
	puts("V.32 line receiver tests passed");
	return 0;
}
