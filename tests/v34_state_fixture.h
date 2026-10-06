// SPDX-License-Identifier: BSD-3-Clause
#ifndef V34_STATE_FIXTURE_H
#define V34_STATE_FIXTURE_H

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

#endif
