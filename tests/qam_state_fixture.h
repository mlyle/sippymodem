// SPDX-License-Identifier: BSD-3-Clause

#ifndef QAM_STATE_FIXTURE_H
#define QAM_STATE_FIXTURE_H

#include <qam.h>
#include <string.h>

/* Preserve exact floating-point bits while ignoring struct padding. Arrays
 * are initialized in full, so compare all declared elements, including the
 * inactive slots retained by the modem. */
static inline bool qam_channels_equal(
	const struct qam_channel *left, const struct qam_channel *right)
{
	return left->sample_rate == right->sample_rate &&
	       left->symbol_rate_numerator == right->symbol_rate_numerator &&
	       left->symbol_rate_denominator == right->symbol_rate_denominator &&
	       left->carrier_frequency_numerator ==
		       right->carrier_frequency_numerator &&
	       left->carrier_frequency_denominator ==
		       right->carrier_frequency_denominator &&
	       left->pulse_span_symbols == right->pulse_span_symbols &&
	       memcmp(&left->rolloff, &right->rolloff,
		      sizeof(left->rolloff)) == 0;
}

static inline bool qam_points_equal(
	const struct qam_point *left, const struct qam_point *right)
{
	return memcmp(&left->in_phase, &right->in_phase,
		      sizeof(left->in_phase)) == 0 &&
	       memcmp(&left->quadrature, &right->quadrature,
		      sizeof(left->quadrature)) == 0;
}

static inline bool qam_modulators_equal(
	const struct qam_modulator *left, const struct qam_modulator *right)
{
	for (size_t i = 0; i < QAM_MAX_ACTIVE_SYMBOLS; ++i)
		if (left->active[i].age_units != right->active[i].age_units ||
		    !qam_points_equal(&left->active[i].point, &right->active[i].point))
			return false;
	return qam_channels_equal(&left->channel, &right->channel) &&
	       left->symbol_period_units == right->symbol_period_units &&
	       left->symbol_phase_units == right->symbol_phase_units &&
	       left->carrier_phase == right->carrier_phase &&
	       left->carrier_phase_step == right->carrier_phase_step &&
	       left->active_count == right->active_count &&
	       left->amplitude == right->amplitude &&
	       left->needs_symbol == right->needs_symbol &&
	       memcmp(&left->pulse_normalization, &right->pulse_normalization,
		      sizeof(left->pulse_normalization)) == 0 &&
	       memcmp(&left->maximum_point_power, &right->maximum_point_power,
		      sizeof(left->maximum_point_power)) == 0;
}

static inline bool qam_demodulators_equal(
	const struct qam_demodulator *left, const struct qam_demodulator *right)
{
	for (size_t i = 0; i < QAM_MAX_TIMING_CANDIDATES; ++i)
		if (left->timing[i].phase_units != right->timing[i].phase_units ||
		    left->timing[i].symbol_count != right->timing[i].symbol_count ||
		    memcmp(&left->timing[i].symbol_power_sum,
			   &right->timing[i].symbol_power_sum,
			   sizeof(left->timing[i].symbol_power_sum)) != 0)
			return false;
	for (size_t i = 0; i < QAM_INTERPOLATION_HISTORY; ++i)
		if (!qam_points_equal(
			    &left->matched_history[i], &right->matched_history[i]))
			return false;
	return qam_channels_equal(&left->channel, &right->channel) &&
	       left->symbol_period_units == right->symbol_period_units &&
	       left->timing_step_units == right->timing_step_units &&
	       left->recovery_step_units == right->recovery_step_units &&
	       left->locked_phase_units == right->locked_phase_units &&
	       left->recovery_phase_units == right->recovery_phase_units &&
	       left->timing_forward_error_units == right->timing_forward_error_units &&
	       left->timing_backward_error_units == right->timing_backward_error_units &&
	       left->carrier_phase == right->carrier_phase &&
	       left->carrier_phase_step == right->carrier_phase_step &&
	       left->timing_sample_count == right->timing_sample_count &&
	       left->timing_sample_target == right->timing_sample_target &&
	       left->acquisition_window_symbols == right->acquisition_window_symbols &&
	       left->tracking_window_symbols == right->tracking_window_symbols &&
	       left->recovery_phase_samples == right->recovery_phase_samples &&
	       left->matched_filter_count == right->matched_filter_count &&
	       left->history_index == right->history_index &&
	       left->history_count == right->history_count &&
	       left->timing_candidate_count == right->timing_candidate_count &&
	       left->timing_reference_index == right->timing_reference_index &&
	       left->timing_rate_reference_index == right->timing_rate_reference_index &&
	       left->timing_pending_reference_index ==
		       right->timing_pending_reference_index &&
	       left->matched_history_count == right->matched_history_count &&
	       left->timing_error_windows == right->timing_error_windows &&
	       left->timing_forward_windows == right->timing_forward_windows &&
	       left->timing_backward_windows == right->timing_backward_windows &&
	       left->timing_reference_windows == right->timing_reference_windows &&
	       left->recovery_phase_direction == right->recovery_phase_direction &&
	       left->timing_locked == right->timing_locked &&
	       left->acquisition_started == right->acquisition_started &&
	       left->recovery_pending == right->recovery_pending &&
	       memcmp(&left->line_power, &right->line_power,
		      sizeof(left->line_power)) == 0 &&
	       memcmp(&left->symbol_power, &right->symbol_power,
		      sizeof(left->symbol_power)) == 0 &&
	       memcmp(&left->timing_power_hysteresis, &right->timing_power_hysteresis,
		      sizeof(left->timing_power_hysteresis)) == 0 &&
	       memcmp(&left->recovery_fraction, &right->recovery_fraction,
		      sizeof(left->recovery_fraction)) == 0 &&
	       memcmp(&left->matched_coefficients, &right->matched_coefficients,
		      sizeof(left->matched_coefficients)) == 0 &&
	       memcmp(&left->in_phase_history, &right->in_phase_history,
		      sizeof(left->in_phase_history)) == 0 &&
	       memcmp(&left->quadrature_history, &right->quadrature_history,
		      sizeof(left->quadrature_history)) == 0;
}

#endif
