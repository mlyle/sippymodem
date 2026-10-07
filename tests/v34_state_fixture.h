// SPDX-License-Identifier: BSD-3-Clause
#ifndef V34_STATE_FIXTURE_H
#define V34_STATE_FIXTURE_H

#include <v34_data_line_receiver.h>
#include <v34_data_stream.h>
#include <v34_mapping_parameters.h>
#include <v34_training_line.h>
#include "qam_state_fixture.h"
#include <stddef.h>
#include <string.h>

/* Only use on snapshot copies. Preserve every field's exact representation;
 * replace padding with zeros without reading it or changing DSP. */
static inline void v34_snapshot_geometry(struct v34_frame_geometry *geometry)
{
	unsigned char bytes[sizeof(*geometry)] = { 0 };
#define COPY(field) memcpy(bytes + offsetof(struct v34_frame_geometry, field), \
	&geometry->field, sizeof(geometry->field))
	COPY(symbol_rate);
	COPY(data_bits_per_frame);
	COPY(primary_data_rate_2400_bps_units);
	COPY(data_frames_per_superframe);
	COPY(mapping_frames_per_data_frame);
	COPY(high_mapping_frame_bits);
	COPY(high_mapping_frame_count);
	COPY(auxiliary_bits_per_frame);
	COPY(auxiliary_channel_enabled);
#undef COPY
	memcpy(geometry, bytes, sizeof(bytes));
}

static inline void v34_snapshot_parameters(
	struct v34_mapping_parameters *parameters)
{
	unsigned char bytes[sizeof(*parameters)] = { 0 };
#define COPY(field) memcpy(bytes + offsetof(struct v34_mapping_parameters, field), \
	&parameters->field, sizeof(parameters->field))
	COPY(constellation_points);
	COPY(mapping_frame_bits);
	COPY(shell_mapper_bits);
	COPY(point_index_bits_per_2d_symbol);
	COPY(ring_count);
	COPY(expanded_shaping);
#undef COPY
	memcpy(parameters, bytes, sizeof(bytes));
}

static inline void v34_snapshot_points(struct v34_data_points *points)
{
	v34_snapshot_geometry(&points->schedule.geometry);
	v34_snapshot_parameters(&points->encoder.parameters);
	v34_snapshot_parameters(&points->normalizer.parameters);
}

static inline bool v34_data_points_equal(
	const struct v34_data_points *left, const struct v34_data_points *right)
{
	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT; ++tap) {
		const struct v34_q7_point *a =
			&left->encoder.precoder.previous_symbols[tap];
		const struct v34_q7_point *b =
			&right->encoder.precoder.previous_symbols[tap];

		if (a->in_phase != b->in_phase || a->quadrature != b->quadrature)
			return false;
	}
	for (size_t point = 0; point < V34_SYMBOLS_PER_MAPPING_FRAME; ++point)
		if (!qam_points_equal(&left->point[point], &right->point[point]))
			return false;
#define SAME(field) (memcmp(&left->field, &right->field, sizeof(left->field)) == 0)
	bool equal = v34_frame_geometry_is_equal(
			&left->schedule.geometry, &right->schedule.geometry) &&
		left->schedule.data_frame_in_superframe ==
			right->schedule.data_frame_in_superframe &&
		left->schedule.next_mapping_frame ==
			right->schedule.next_mapping_frame &&
		left->scrambler.history == right->scrambler.history &&
		left->scrambler.polynomial == right->scrambler.polynomial &&
		v34_mapping_parameters_are_equal(
			&left->encoder.parameters, &right->encoder.parameters) &&
		v34_precoder_coefficients_are_equal(
			left->encoder.precoder.coefficients,
			right->encoder.precoder.coefficients) &&
		left->encoder.precoder.grid_step ==
			right->encoder.precoder.grid_step &&
		left->encoder.convolutional_encoder.state ==
			right->encoder.convolutional_encoder.state &&
		left->encoder.convolutional_encoder.selection ==
			right->encoder.convolutional_encoder.selection &&
		left->encoder.previous_differential_output ==
			right->encoder.previous_differential_output &&
		v34_mapping_parameters_are_equal(
			&left->normalizer.parameters, &right->normalizer.parameters) &&
		SAME(normalizer.precoded_mean_squared_magnitude) &&
		SAME(normalizer.projected_mean_squared_magnitude) &&
		SAME(normalizer.normalization_gain) &&
		SAME(normalizer.maximum_precoded_point_power) &&
		SAME(normalizer.maximum_projected_point_power) &&
		SAME(normalizer.maximum_normalized_point_power) &&
		left->normalizer.calibration_symbol_count ==
			right->normalizer.calibration_symbol_count &&
		left->normalizer.nonlinear_parameter ==
			right->normalizer.nonlinear_parameter &&
		left->local_role == right->local_role &&
		left->next_point == right->next_point;
#undef SAME
	return equal;
}

static inline void v34_snapshot_data_receiver(
	struct v34_data_line_receiver *receiver)
{
	struct v34_data_channel_receiver *data = &receiver->decisions.data_receiver;
	struct v34_channel_frame_decoder *decoder =
		&data->channel_receiver.frame_decoder;
	struct v34_adaptive_equalizer_observation *observation =
		&receiver->decisions.first_observation;
	unsigned char bytes[sizeof(*observation)] = { 0 };

	v34_snapshot_geometry(&receiver->geometry);
	v34_snapshot_geometry(&decoder->channel_decoder.geometry);
	v34_snapshot_geometry(&decoder->channel_schedule.geometry);
	v34_snapshot_geometry(&data->data_receiver.schedule.geometry);
	v34_snapshot_parameters(&decoder->channel_decoder.parameters);
	v34_snapshot_parameters(
		&data->channel_receiver.conditioner.normalizer.parameters);
	v34_snapshot_parameters(&data->data_receiver.mapping_decoder.parameters);
#define COPY(field) memcpy(bytes + offsetof(struct v34_adaptive_equalizer_observation, field), \
	&observation->field, sizeof(observation->field))
	COPY(decision);
	COPY(epoch);
	COPY(lineage);
#undef COPY
	memcpy(observation, bytes, sizeof(bytes));
}

static inline bool v34_data_line_receivers_equal(
	const struct v34_data_line_receiver *left,
	const struct v34_data_line_receiver *right)
{
	struct v34_data_line_receiver snapshots[2];

	memcpy(&snapshots[0], left, sizeof(*left));
	memcpy(&snapshots[1], right, sizeof(*right));
	v34_snapshot_data_receiver(&snapshots[0]);
	v34_snapshot_data_receiver(&snapshots[1]);
	return memcmp(&snapshots[0], &snapshots[1], sizeof(*left)) == 0;
}

static inline bool v34_pulse_states_equal(
	const struct v34_pulse_modulator *left,
	const struct v34_pulse_modulator *right)
{
	return qam_channels_equal(&left->channel, &right->channel) &&
	       memcmp(left->in_phase_history, right->in_phase_history,
		      sizeof(left->in_phase_history)) == 0 &&
	       memcmp(left->quadrature_history, right->quadrature_history,
		      sizeof(left->quadrature_history)) == 0 &&
	       memcmp(left->preemphasis_history, right->preemphasis_history,
		      sizeof(left->preemphasis_history)) == 0 &&
	       left->symbol_period_units == right->symbol_period_units &&
	       left->symbol_phase_units == right->symbol_phase_units &&
	       left->generated_samples == right->generated_samples &&
	       left->carrier_phase == right->carrier_phase &&
	       left->carrier_phase_step == right->carrier_phase_step &&
	       left->pulse_phase_divisor == right->pulse_phase_divisor &&
	       left->pulse_phase_count == right->pulse_phase_count &&
	       memcmp(&left->maximum_point_power, &right->maximum_point_power,
		      sizeof(left->maximum_point_power)) == 0 &&
	       left->symbol_rate == right->symbol_rate &&
	       left->preemphasis_filter_index == right->preemphasis_filter_index;
}

static inline bool v34_training_lines_equal(
	const struct v34_training_line *left,
	const struct v34_training_line *right)
{
	return v34_phase3_profile_is_equal(&left->profile, &right->profile) &&
	       v34_pulse_states_equal(&left->carrier, &right->carrier) &&
	       memcmp(&left->output_gain, &right->output_gain,
		      sizeof(left->output_gain)) == 0 &&
	       memcmp(&left->raw_rms, &right->raw_rms, sizeof(left->raw_rms)) == 0 &&
	       memcmp(&left->raw_peak, &right->raw_peak, sizeof(left->raw_peak)) == 0 &&
	       memcmp(&left->target_rms, &right->target_rms, sizeof(left->target_rms)) == 0 &&
	       left->generated_samples == right->generated_samples &&
	       left->nominal_peak_amplitude == right->nominal_peak_amplitude;
}

static inline bool v34_data_streams_equal(
	const struct v34_data_stream *left, const struct v34_data_stream *right)
{
	return v34_data_points_equal(&left->points, &right->points) &&
	       v34_training_lines_equal(&left->line, &right->line) &&
	       left->delayed_primary_bits == right->delayed_primary_bits &&
	       left->primary_delay_bits == right->primary_delay_bits &&
	       left->b1_active == right->b1_active;
}

#endif
