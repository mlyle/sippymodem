// SPDX-License-Identifier: BSD-3-Clause

#include <v34_data_line_receiver.h>
#include <v34_data_stream.h>
#include <v34_datapump.h>

#include "test.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	ACQUISITION_SETTLE_SYMBOLS = 96,
	REFERENCE_SETTLE_SYMBOLS = 32,
	TRAINING_SAMPLE_LIMIT = 16000,
	HANDOFF_SEARCH_SAMPLES = 512,
	HANDOFF_POINT_CAPACITY = 256,
	HANDOFF_MATCH_POINTS = 64,
	LINE_SAMPLE_LIMIT = 30000,
	EXPECTED_FRAME_CAPACITY = 256,
	VERIFIED_DATA_FRAMES = 4,
	VERIFIED_DRIFT_FRAMES = 192,
};

static const struct qam_point TRAINING_POINT = {
	.in_phase = 0.3141592653589793,
	.quadrature = -0.2718281828459045,
};

static struct v34_data_stream_configuration stream_configuration(void)
{
	return (struct v34_data_stream_configuration){
		.profile = {
			.symbol_rate_numerator = 3000,
			.symbol_rate_denominator = 1,
			.carrier_frequency_numerator = 2000,
			.carrier_frequency_denominator = 1,
			.symbol_rate = V34_SYMBOL_RATE_3000,
			.local_role = V34_CALLING_STATION,
			.preemphasis_filter_index = 0,
			.projected_data_rate_2400_bps_units = 8,
			.high_carrier = true,
		},
		.precoder_coefficients = {
			{ 4096, 2048 },
			{ -2048, 1024 },
			{ 1024, -2048 },
		},
		.sample_rate = 8000,
		.nominal_peak_amplitude = 5000,
		.trellis_selection = V34_TRELLIS_16_STATE,
		.nonlinear_parameter = V34_NONLINEAR_THETA_ZERO,
		.primary_data_rate_2400_bps_units = 7,
	};
}

static struct v34_data_line_receiver_configuration receiver_configuration(
	const struct v34_data_stream *stream,
	const struct v34_data_stream_configuration *configuration,
	double equalizer_adaptation)
{
	struct v34_data_line_receiver_configuration receiver = {
		.transmitting_profile = configuration->profile,
		.geometry = stream->transmitter.points.schedule.geometry,
		.normalizer = stream->transmitter.line.carrier.normalizer,
		.transmit_point_gain =
			stream->transmitter.line.carrier.point_gain,
		.equalizer_adaptation = equalizer_adaptation,
		.trellis_selection = configuration->trellis_selection,
		.expanded_shaping = configuration->expanded_shaping,
	};

	memcpy(receiver.precoder_coefficients,
	       configuration->precoder_coefficients,
	       sizeof(receiver.precoder_coefficients));
	return receiver;
}

static bool training_line_sample(
	struct v34_data_stream *stream, const struct qam_point *point,
	int16_t *sample, bool *symbol_accepted)
{
	struct v34_data_line_transmitter *line =
		&stream->transmitter.line;
	double carrier_sample;
	double filtered_sample;
	double scaled_sample;
	long rounded;
	bool accepted;

	if (point == NULL || symbol_accepted == NULL ||
	    qam_modulator_sample_f64(
		    &line->carrier.modulator, point,
		    &carrier_sample, &accepted) != QAM_OK ||
	    v34_preemphasis_sample(
		    &line->preemphasis, carrier_sample,
		    &filtered_sample) != V34_PREEMPHASIS_OK)
		return false;
	(void)accepted;
	scaled_sample = filtered_sample * line->output_gain;
	if (!isfinite(scaled_sample) || scaled_sample <= INT16_MIN - 0.5 ||
	    scaled_sample >= INT16_MAX + 0.5 ||
	    line->generated_samples == UINT64_MAX)
		return false;
	rounded = lround(scaled_sample);
	if (rounded < INT16_MIN || rounded > INT16_MAX)
		return false;
	line->generated_samples++;
	*sample = (int16_t)rounded;
	*symbol_accepted = accepted;
	return v34_data_stream_is_valid(stream);
}

static uint32_t next_generated_word(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static struct qam_point generated_training_point(uint32_t *state)
{
	return (struct qam_point){
		.in_phase = (next_generated_word(state) >> 31) != 0 ? 0.5 : -0.5,
		.quadrature =
			(next_generated_word(state) >> 31) != 0 ? 0.5 : -0.5,
	};
}

static bool train_front_end(
	struct v34_data_stream *stream,
	struct qam_demodulator *demodulator,
	struct qam_equalizer *equalizer)
{
	struct qam_point current = TRAINING_POINT;
	struct qam_point reference = { 0 };
	uint32_t random = UINT32_C(0xa394d27b);
	size_t settled = 0;

	if (qam_demodulator_init(
		    demodulator,
		    &stream->transmitter.line.carrier.modulator.channel) !=
	    QAM_OK)
		return false;
	for (size_t sample_index = 0; sample_index < TRAINING_SAMPLE_LIMIT;
	     ++sample_index) {
		struct qam_point received;
		int16_t sample;
		bool accepted;
		bool ready;

		if (!training_line_sample(
			    stream, &current, &sample, &accepted) ||
		    qam_demodulator_sample(
			    demodulator, sample, &received, &ready) != QAM_OK)
			return false;
		if (accepted)
			current = generated_training_point(&random);
		if (!ready)
			continue;
		if (++settled == ACQUISITION_SETTLE_SYMBOLS)
			break;
	}
	if (settled != ACQUISITION_SETTLE_SYMBOLS)
		return false;
	settled = 0;
	for (size_t sample_index = 0; sample_index < TRAINING_SAMPLE_LIMIT;
	     ++sample_index) {
		int16_t sample;
		bool accepted;
		bool ready;

		if (!training_line_sample(
			    stream, &TRAINING_POINT, &sample, &accepted) ||
		    qam_demodulator_sample(
			    demodulator, sample, &reference, &ready) != QAM_OK)
			return false;
		(void)accepted;
		if (ready && ++settled == REFERENCE_SETTLE_SYMBOLS)
			break;
	}
	return settled == REFERENCE_SETTLE_SYMBOLS &&
	       qam_demodulator_measure(demodulator).timing_locked &&
	       qam_equalizer_init_geometry(
		       equalizer, &reference, &TRAINING_POINT,
		       V34_EQUALIZER_TAP_COUNT,
		       V34_EQUALIZER_DECISION_DELAY) ==
		       QAM_EQUALIZER_OK &&
	       qam_equalizer_is_valid(equalizer) &&
	       v34_data_stream_is_valid(stream);
}

static double point_distance_squared(
	const struct qam_point *left, const struct qam_point *right)
{
	double in_phase = left->in_phase - right->in_phase;
	double quadrature = left->quadrature - right->quadrature;

	return in_phase * in_phase + quadrature * quadrature;
}

struct timed_point {
	struct qam_point point;
	size_t sample_index;
};

static bool find_handoff_sample(
	const struct v34_data_stream *stream,
	const struct qam_demodulator *demodulator,
	const struct qam_equalizer *equalizer,
	size_t *handoff_sample)
{
	struct v34_data_stream probe_stream = *stream;
	struct qam_demodulator probe_demodulator = *demodulator;
	struct qam_equalizer probe_equalizer = *equalizer;
	struct qam_point transmitted[HANDOFF_POINT_CAPACITY];
	struct timed_point output[HANDOFF_POINT_CAPACITY];
	size_t ready_sample[HANDOFF_POINT_CAPACITY];
	size_t transmitted_count = 0;
	size_t output_count = 0;
	size_t ready_count = 0;

	for (size_t index = 0; index < HANDOFF_SEARCH_SAMPLES; ++index) {
		struct qam_point received;
		struct qam_point equalized;
		int16_t sample;
		bool accepted;
		bool ready;
		bool equalized_ready;

		if (v34_data_stream_sample(
			    &probe_stream, NULL, NULL, &sample, &accepted) !=
			    V34_DATA_STREAM_OK ||
		    qam_demodulator_sample(
			    &probe_demodulator, sample, &received, &ready) !=
			    QAM_OK)
			return false;
		if (accepted) {
			size_t next = probe_stream.transmitter.points.next_point;

			if (next == 0 || next > V34_SYMBOLS_PER_MAPPING_FRAME ||
			    transmitted_count == HANDOFF_POINT_CAPACITY)
				return false;
			transmitted[transmitted_count++] =
				probe_stream.transmitter.points.point[next - 1u];
		}
		if (ready) {
			if (ready_count == HANDOFF_POINT_CAPACITY ||
			    qam_equalizer_push(
				    &probe_equalizer, &received, &equalized,
				    &equalized_ready) != QAM_EQUALIZER_OK)
				return false;
			ready_sample[ready_count] = index;
			if (equalized_ready) {
				if (ready_count < probe_equalizer.decision_delay)
					return false;
				if (output_count == HANDOFF_POINT_CAPACITY)
					return false;
				output[output_count++] = (struct timed_point){
					.point = equalized,
					.sample_index =
						ready_sample[
							ready_count -
							probe_equalizer.decision_delay],
				};
			}
			ready_count++;
		}
	}
	if (transmitted_count < HANDOFF_MATCH_POINTS ||
	    output_count < HANDOFF_MATCH_POINTS)
		return false;
	{
		double best = HUGE_VAL;
		size_t best_offset = 0;

		for (size_t offset = 0;
		     offset <= output_count - HANDOFF_MATCH_POINTS; ++offset) {
			double sum = 0.0;

			for (size_t point = 0; point < HANDOFF_MATCH_POINTS; ++point)
				sum += point_distance_squared(
					&transmitted[point],
					&output[offset + point].point);
			if (sum < best) {
				best = sum;
				best_offset = offset;
			}
		}
		if (!isfinite(best) ||
		    best / HANDOFF_MATCH_POINTS > 0.02)
			return false;
		*handoff_sample = output[best_offset].sample_index;
		return true;
	}
}

static bool advance_training_tail(
	struct v34_data_stream *stream,
	struct qam_demodulator *demodulator,
	struct qam_equalizer *equalizer, size_t handoff_sample)
{
	for (size_t index = 0; index < handoff_sample; ++index) {
		struct qam_point received;
		int16_t sample;
		bool accepted;
		bool ready;

		if (v34_data_stream_sample(
			    stream, NULL, NULL, &sample, &accepted) !=
			    V34_DATA_STREAM_OK ||
		    qam_demodulator_sample(
			    demodulator, sample, &received, &ready) != QAM_OK)
			return false;
		(void)accepted;
		if (ready) {
			struct qam_point output;
			bool output_ready;

			if (qam_equalizer_push(
				    equalizer, &received, &output,
				    &output_ready) != QAM_EQUALIZER_OK ||
			    (output_ready &&
			     qam_equalizer_adapt(
				     equalizer, &output, 0.0) !=
				     QAM_EQUALIZER_OK))
				return false;
		}
	}
	return true;
}

static bool retain_b1_lookahead(
	struct v34_data_stream *stream,
	struct qam_demodulator *demodulator,
	struct qam_equalizer *equalizer)
{
	size_t retained = 0;

	for (size_t index = 0;
	     index < HANDOFF_SEARCH_SAMPLES &&
	     retained < equalizer->decision_delay;
	     ++index) {
		struct qam_point received;
		struct qam_point output;
		int16_t sample;
		bool accepted;
		bool ready;
		bool output_ready;

		if (v34_data_stream_sample(
			    stream, NULL, NULL, &sample, &accepted) !=
			    V34_DATA_STREAM_OK ||
		    qam_demodulator_sample(
			    demodulator, sample, &received, &ready) != QAM_OK)
			return false;
		(void)accepted;
		if (!ready)
			continue;
		if (qam_equalizer_push(
			    equalizer, &received, &output,
			    &output_ready) != QAM_EQUALIZER_OK ||
		    (output_ready &&
		     qam_equalizer_adapt(
			     equalizer, &output, 0.0) != QAM_EQUALIZER_OK))
			return false;
		retained++;
	}
	return retained == equalizer->decision_delay;
}

struct handoff_fixture {
	struct v34_data_stream stream;
	struct v34_data_line_receiver receiver;
	struct v34_data_stream_configuration configuration;
};

static bool prepare_handoff(
	struct handoff_fixture *fixture, double equalizer_adaptation)
{
	struct handoff_fixture prepared = {
		.configuration = stream_configuration(),
	};
	struct v34_data_line_receiver_configuration receiver_config;
	struct v34_equalizer_handoff handoff;
	struct qam_demodulator demodulator;
	struct qam_equalizer equalizer;
	size_t handoff_sample;

	if (v34_data_stream_init(
		    &prepared.stream, &prepared.configuration) !=
		    V34_DATA_STREAM_OK ||
	    !train_front_end(
		    &prepared.stream, &demodulator, &equalizer) ||
	    !find_handoff_sample(
		    &prepared.stream, &demodulator, &equalizer,
		    &handoff_sample) ||
	    !advance_training_tail(
		    &prepared.stream, &demodulator, &equalizer,
		    handoff_sample) ||
	    !retain_b1_lookahead(
		    &prepared.stream, &demodulator, &equalizer))
		return false;
	handoff.equalizer = equalizer;
	if (!v34_equalizer_handoff_is_valid(&handoff))
		return false;
	receiver_config = receiver_configuration(
		&prepared.stream, &prepared.configuration,
		equalizer_adaptation);
	if (v34_data_line_receiver_init(
		    &prepared.receiver, &demodulator, &handoff,
		    &receiver_config) != V34_DATA_LINE_RECEIVER_OK)
		return false;
	for (size_t tap = 0; tap < equalizer.tap_count; ++tap) {
		double in_phase = prepared.receiver.equalizer_receiver.equalizer
			.coefficients[tap].in_phase *
			receiver_config.transmit_point_gain;
		double quadrature = prepared.receiver.equalizer_receiver.equalizer
			.coefficients[tap].quadrature *
			receiver_config.transmit_point_gain;
		double scale = fmax(
			1.0, hypot(equalizer.coefficients[tap].in_phase,
				   equalizer.coefficients[tap].quadrature));

		if (fabs(in_phase - equalizer.coefficients[tap].in_phase) >
			    1e-12 * scale ||
		    fabs(quadrature -
			 equalizer.coefficients[tap].quadrature) >
			    1e-12 * scale)
			return false;
	}
	*fixture = prepared;
	return true;
}

struct expected_frame {
	struct v34_mapping_frame mapping_frame;
	uint8_t primary_bits[V34_MAX_MAPPING_FRAME_BITS];
	uint8_t auxiliary_bit;
};

struct source_fixture {
	struct expected_frame frames[EXPECTED_FRAME_CAPACITY];
	uint16_t delayed_primary_bits;
	size_t count;
};

static bool generated_source(
	void *context, const struct v34_mapping_frame *request,
	uint8_t *primary_bits, uint8_t *auxiliary_bit)
{
	struct source_fixture *source = context;
	uint32_t random;

	if (source == NULL || request == NULL || primary_bits == NULL ||
	    source->count == EXPECTED_FRAME_CAPACITY)
		return false;
	random = UINT32_C(0x8d2f6b31) ^ (uint32_t)source->count;
	for (size_t index = 0; index < request->primary_bits; ++index) {
		random = random * UINT32_C(1664525) + UINT32_C(1013904223);
		primary_bits[index] = (uint8_t)(random >> 31);
	}
	if (auxiliary_bit != NULL)
		*auxiliary_bit = (uint8_t)(source->count & 1u);
	source->frames[source->count] = (struct expected_frame){
		.mapping_frame = *request,
		.auxiliary_bit = auxiliary_bit != NULL ? *auxiliary_bit : 0,
	};
	for (size_t index = 0; index < request->primary_bits; ++index) {
		source->frames[source->count].primary_bits[index] =
			(uint8_t)(source->delayed_primary_bits & 1u);
		source->delayed_primary_bits =
			(uint16_t)((source->delayed_primary_bits >> 1) |
				   ((uint16_t)primary_bits[index] << 15));
	}
	source->count++;
	return true;
}

static bool frames_are_equal(
	const struct v34_received_mapping_frame *received,
	const struct expected_frame *expected)
{
	const struct v34_mapping_frame *left = &received->mapping_frame;
	const struct v34_mapping_frame *right = &expected->mapping_frame;

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

struct drifting_echo {
	int16_t history[2];
	size_t cursor;
	uint32_t samples;
};

static int16_t apply_drifting_echo(
	struct drifting_echo *channel, int16_t sample)
{
	enum {
		RAMP_SAMPLES = 4096,
		FINAL_ECHO_Q15 = 6000,
	};
	uint32_t progress = channel->samples < RAMP_SAMPLES ?
		channel->samples : RAMP_SAMPLES;
	int32_t gain_q15 = (int32_t)(
		(uint64_t)progress * FINAL_ECHO_Q15 / RAMP_SAMPLES);
	int16_t delayed = channel->history[channel->cursor];
	int64_t mixed = (int64_t)sample +
		(int64_t)delayed * gain_q15 / 32768;

	channel->history[channel->cursor] = sample;
	channel->cursor = (channel->cursor + 1u) % ARRAY_LENGTH(channel->history);
	if (channel->samples != UINT32_MAX)
		channel->samples++;
	if (mixed > INT16_MAX)
		return INT16_MAX;
	if (mixed < INT16_MIN)
		return INT16_MIN;
	return (int16_t)mixed;
}

static bool test_generated_channel_drift(void)
{
	struct handoff_fixture fixture;
	struct source_fixture source = {
		.delayed_primary_bits = UINT16_MAX,
	};
	struct drifting_echo channel = { 0 };
	size_t expected_index = 0;
	bool b1_complete = false;

	CHECK(prepare_handoff(
		      &fixture, V34_DATA_EQUALIZER_TRACKING_STEP),
	      "V.34 tracking fixture did not initialize");
	for (size_t sample_index = 0;
	     sample_index < LINE_SAMPLE_LIMIT &&
	     expected_index < VERIFIED_DRIFT_FRAMES;
	     ++sample_index) {
		struct v34_survivor_decision current;
		struct v34_received_mapping_frame received;
		enum v34_data_line_receiver_result result;
		int16_t sample;
		bool accepted;

		CHECK(v34_data_stream_sample(
			      &fixture.stream, generated_source, &source,
			      &sample, &accepted) == V34_DATA_STREAM_OK,
		      "V.34 drift transmitter failed at sample %zu", sample_index);
		(void)accepted;
		result = v34_data_line_receiver_sample(
			&fixture.receiver,
			apply_drifting_echo(&channel, sample),
			&current, &received);
		CHECK(result != V34_DATA_LINE_RECEIVER_FAILED &&
			      result != V34_DATA_LINE_RECEIVER_NUMERIC_FAILURE &&
			      result != V34_DATA_LINE_RECEIVER_OUT_OF_RANGE &&
			      result != V34_DATA_LINE_RECEIVER_INVALID_ARGUMENT,
		      "V.34 tracking receiver failed at sample %zu: %s",
		      sample_index, v34_data_line_receiver_result_name(result));
		if (result == V34_DATA_LINE_RECEIVER_B1_COMPLETE) {
			CHECK(!b1_complete,
			      "V.34 tracking receiver completed B1 twice");
			b1_complete = true;
		} else if (result == V34_DATA_LINE_RECEIVER_MAPPING_FRAME_READY) {
			CHECK(b1_complete && expected_index < source.count &&
				      frames_are_equal(
					      &received,
					      &source.frames[expected_index]),
			      "V.34 tracking frame %zu changed",
			      expected_index);
			expected_index++;
		}
	}
	CHECK(b1_complete && expected_index == VERIFIED_DRIFT_FRAMES &&
		      source.count >= expected_index &&
		      !v34_data_line_receiver_has_failed(&fixture.receiver),
	      "V.34 tracking recovered %zu/%u drifted frames",
	      expected_index, VERIFIED_DRIFT_FRAMES);
	return true;
}

static bool test_generated_line_round_trip(void)
{
	struct handoff_fixture fixture;
	struct source_fixture source = {
		.delayed_primary_bits = UINT16_MAX,
	};
	size_t expected_index = 0;
	bool b1_complete = false;

	CHECK(prepare_handoff(&fixture, 0.01),
	      "V.34 line handoff fixture did not initialize");
	for (size_t sample_index = 0;
	     sample_index < LINE_SAMPLE_LIMIT &&
	     expected_index < VERIFIED_DATA_FRAMES;
	     ++sample_index) {
		struct v34_survivor_decision current;
		struct v34_received_mapping_frame received;
		enum v34_data_line_receiver_result result;
		int16_t sample;
		bool accepted;

		CHECK(v34_data_stream_sample(
			      &fixture.stream, generated_source, &source,
			      &sample, &accepted) == V34_DATA_STREAM_OK,
		      "V.34 line transmitter failed at sample %zu", sample_index);
		(void)accepted;
		result = v34_data_line_receiver_sample(
			&fixture.receiver, sample, &current, &received);
		CHECK(result != V34_DATA_LINE_RECEIVER_FAILED &&
			      result != V34_DATA_LINE_RECEIVER_NUMERIC_FAILURE &&
			      result != V34_DATA_LINE_RECEIVER_OUT_OF_RANGE &&
			      result != V34_DATA_LINE_RECEIVER_INVALID_ARGUMENT,
		      "V.34 line receiver failed at sample %zu: %s",
		      sample_index, v34_data_line_receiver_result_name(result));
		if (result == V34_DATA_LINE_RECEIVER_B1_COMPLETE) {
			CHECK(!b1_complete,
			      "V.34 line receiver completed B1 twice");
			b1_complete = true;
		} else if (result == V34_DATA_LINE_RECEIVER_MAPPING_FRAME_READY) {
			CHECK(b1_complete && expected_index < source.count &&
				      frames_are_equal(
					      &received,
					      &source.frames[expected_index]),
			      "V.34 line data frame %zu changed",
			      expected_index);
			expected_index++;
		}
	}
	CHECK(b1_complete && expected_index == VERIFIED_DATA_FRAMES &&
		      source.count >= expected_index &&
		      fixture.receiver.equalizer_receiver.data_receiver.data_receiver
				      .b1_error_bits == 0u &&
		      !v34_data_line_receiver_has_failed(&fixture.receiver),
	      "V.34 line path recovered %zu/%u data frames",
	      expected_index, VERIFIED_DATA_FRAMES);
	return true;
}

static bool object_is_filled(
	const void *object, size_t size, unsigned char value)
{
	const unsigned char *bytes = object;

	for (size_t index = 0; index < size; ++index)
		if (bytes[index] != value)
			return false;
	return true;
}

static bool test_validation_and_failure_latch(void)
{
	struct handoff_fixture fixture;
	struct v34_data_line_receiver rejected;
	struct v34_data_line_receiver_configuration configuration;
	struct v34_equalizer_handoff handoff;
	struct v34_equalizer_handoff invalid_handoff;
	struct qam_demodulator unlocked;
	struct qam_channel channel;
	struct qam_channel saved_channel;
	struct v34_phase3_profile profile = stream_configuration().profile;
	struct v34_survivor_decision current;
	struct v34_received_mapping_frame received;

	CHECK(prepare_handoff(&fixture, 0.01),
	      "V.34 line validation fixture did not initialize");
	handoff.equalizer = fixture.receiver.equalizer_receiver.equalizer;
	CHECK(v34_equalizer_handoff_is_valid(&handoff),
	      "V.34 line validation handoff is invalid");
	configuration = receiver_configuration(
		&fixture.stream, &fixture.configuration, 0.01);
	CHECK(v34_data_carrier_channel(&profile, 8000, &channel) ==
		      V34_DATA_CARRIER_OK &&
	      qam_demodulator_init(&unlocked, &channel) == QAM_OK &&
	      qam_demodulator_is_valid(&unlocked),
	      "V.34 line validation demodulator did not initialize");
	memset(&rejected, 0xa5, sizeof(rejected));
	CHECK(v34_data_line_receiver_init(
		      &rejected, &unlocked, &handoff,
		      &configuration) == V34_DATA_LINE_RECEIVER_INVALID_ARGUMENT &&
	      object_is_filled(&rejected, sizeof(rejected), 0xa5),
	      "V.34 line receiver accepted an unlocked handoff");
	configuration.transmit_point_gain = 0.0;
	CHECK(v34_data_line_receiver_init(
		      &rejected, &fixture.receiver.line, &handoff,
		      &configuration) == V34_DATA_LINE_RECEIVER_INVALID_ARGUMENT &&
	      object_is_filled(&rejected, sizeof(rejected), 0xa5),
	      "V.34 line receiver accepted a zero point gain");
	configuration = receiver_configuration(
		&fixture.stream, &fixture.configuration, 0.01);
	invalid_handoff = handoff;
	invalid_handoff.equalizer.fill =
		handoff.equalizer.decision_delay;
	CHECK(v34_data_line_receiver_init(
		      &rejected, &fixture.receiver.line,
		      &invalid_handoff, &configuration) ==
		      V34_DATA_LINE_RECEIVER_INVALID_ARGUMENT &&
	      v34_data_line_receiver_init(
		      &rejected, &fixture.receiver.line, NULL,
		      &configuration) ==
		      V34_DATA_LINE_RECEIVER_INVALID_ARGUMENT &&
	      object_is_filled(&rejected, sizeof(rejected), 0xa5),
	      "V.34 line receiver accepted an incomplete handoff");
	saved_channel = channel;
	profile.carrier_frequency_numerator = 0;
	CHECK(v34_data_carrier_channel(&profile, 8000, &channel) ==
		      V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      memcmp(&channel, &saved_channel, sizeof(channel)) == 0,
	      "V.34 waveform configuration changed a rejected output");
	profile = stream_configuration().profile;
	CHECK(v34_data_carrier_channel(&profile, 4000, &channel) ==
		      V34_DATA_CARRIER_OUT_OF_RANGE &&
	      memcmp(&channel, &saved_channel, sizeof(channel)) == 0,
	      "V.34 waveform accepted an occupied band beyond Nyquist");

	for (size_t index = 0; index < 4096; ++index) {
		enum v34_data_line_receiver_result result =
			v34_data_line_receiver_sample(
				&fixture.receiver,
				(index & 1u) != 0 ? INT16_MAX : INT16_MIN,
				&current, &received);

		CHECK(result != V34_DATA_LINE_RECEIVER_INVALID_ARGUMENT &&
			      result != V34_DATA_LINE_RECEIVER_FAILED &&
			      result != V34_DATA_LINE_RECEIVER_OUT_OF_RANGE &&
			      result != V34_DATA_LINE_RECEIVER_NUMERIC_FAILURE,
		      "V.34 line receiver rejected bounded hostile sample %zu",
		      index);
	}
	CHECK(v34_data_line_receiver_is_valid(&fixture.receiver) &&
		      !v34_data_line_receiver_has_failed(&fixture.receiver),
	      "V.34 line receiver did not survive a bounded hostile channel");
	fixture.receiver.failed = true;
	{
		struct v34_data_line_receiver before = fixture.receiver;
		struct v34_survivor_decision saved_current;
		struct v34_received_mapping_frame saved_received;

		memset(&current, 0x5a, sizeof(current));
		memset(&received, 0x6b, sizeof(received));
		saved_current = current;
		saved_received = received;
		CHECK(v34_data_line_receiver_sample(
			      &fixture.receiver, 0, &current, &received) ==
			      V34_DATA_LINE_RECEIVER_FAILED &&
		      memcmp(&fixture.receiver, &before, sizeof(before)) == 0 &&
		      memcmp(&current, &saved_current, sizeof(current)) == 0 &&
		      memcmp(&received, &saved_received, sizeof(received)) == 0,
		      "V.34 failed line receiver consumed another sample");
	}
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated line round trip", test_generated_line_round_trip },
		{ "generated channel drift", test_generated_channel_drift },
		{ "validation and failure latch", test_validation_and_failure_latch },
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
