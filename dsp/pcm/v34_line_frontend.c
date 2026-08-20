// SPDX-License-Identifier: BSD-3-Clause

#include "v34_line_frontend.h"

#include "memory_region.h"
#include "v34_training_line.h"

#include <stddef.h>

static const double V34_FIXED_TRAINING_AMPLITUDE = 3238.0;
enum {
	/* Match the fixed AGC's minimum qualified line level while excluding
	 * exact and G.711-coded silence from the owned receive timeline. */
	V34_FIXED_SIGNAL_LEVEL_MINIMUM = 31,
};

bool v34_line_frontend_is_valid(
	const struct v34_line_frontend *frontend)
{
	if (frontend == NULL)
		return false;
	switch (frontend->mode) {
	case V34_LINE_FRONTEND_QAM:
		return !frontend->fixed_signal_started &&
		       qam_demodulator_is_valid(&frontend->state.qam);
	case V34_LINE_FRONTEND_FIXED:
		return v34_receive_path_is_valid(&frontend->state.fixed);
	}
	return false;
}

enum v34_line_frontend_result v34_line_frontend_init_qam(
	struct v34_line_frontend *frontend,
	const struct qam_demodulator *demodulator)
{
	struct v34_line_frontend initialized;

	if (frontend == NULL || !qam_demodulator_is_valid(demodulator))
		return V34_LINE_FRONTEND_INVALID_ARGUMENT;
	initialized = (struct v34_line_frontend){
		.state.qam = *demodulator,
		.mode = V34_LINE_FRONTEND_QAM,
	};
	if (!v34_line_frontend_is_valid(&initialized))
		return V34_LINE_FRONTEND_INVALID_ARGUMENT;
	*frontend = initialized;
	return V34_LINE_FRONTEND_OK;
}

enum v34_line_frontend_result v34_line_frontend_init_fixed(
	struct v34_line_frontend *frontend,
	const struct v34_phase3_profile *transmitting_profile)
{
	struct v34_line_frontend initialized = {
		.mode = V34_LINE_FRONTEND_FIXED,
	};

	if (frontend == NULL ||
	    v34_receive_path_init(
		    &initialized.state.fixed, transmitting_profile) !=
		    V34_RECEIVE_PATH_OK ||
	    !v34_line_frontend_is_valid(&initialized))
		return V34_LINE_FRONTEND_INVALID_ARGUMENT;
	*frontend = initialized;
	return V34_LINE_FRONTEND_OK;
}

bool v34_line_frontend_timing_acquired(
	const struct v34_line_frontend *frontend)
{
	if (!v34_line_frontend_is_valid(frontend))
		return false;
	return frontend->mode == V34_LINE_FRONTEND_QAM ?
		qam_demodulator_measure(&frontend->state.qam).timing_locked :
		frontend->fixed_signal_started &&
		frontend->state.fixed.timing.mode !=
			V34_TIMING_CONTROL_INITIAL_PHASE;
}

static bool profiles_are_equal(
	const struct v34_phase3_profile *left,
	const struct v34_phase3_profile *right)
{
	return left->symbol_rate_numerator == right->symbol_rate_numerator &&
	       left->symbol_rate_denominator == right->symbol_rate_denominator &&
	       left->carrier_frequency_numerator ==
		       right->carrier_frequency_numerator &&
	       left->carrier_frequency_denominator ==
		       right->carrier_frequency_denominator &&
	       left->md_symbol_count == right->md_symbol_count &&
	       left->symbol_rate == right->symbol_rate &&
	       left->local_role == right->local_role &&
	       left->transmit_power_reduction_db ==
		       right->transmit_power_reduction_db &&
	       left->preemphasis_filter_index ==
		       right->preemphasis_filter_index &&
	       left->projected_data_rate_2400_bps_units ==
		       right->projected_data_rate_2400_bps_units &&
	       left->md_length_35_ms_units == right->md_length_35_ms_units &&
	       left->high_carrier == right->high_carrier;
}

bool v34_line_frontend_matches_profile(
	const struct v34_line_frontend *frontend,
	const struct v34_phase3_profile *transmitting_profile)
{
	if (!v34_line_frontend_is_valid(frontend) ||
	    !v34_phase3_profile_is_valid(transmitting_profile))
		return false;
	return frontend->mode == V34_LINE_FRONTEND_QAM ?
		v34_training_line_channel_matches(
			transmitting_profile, &frontend->state.qam.channel) :
		profiles_are_equal(
			transmitting_profile,
			&frontend->state.fixed.transmitting_profile);
}

uint32_t v34_line_frontend_sample_rate(
	const struct v34_line_frontend *frontend)
{
	if (!v34_line_frontend_is_valid(frontend))
		return 0;
	return frontend->mode == V34_LINE_FRONTEND_QAM ?
		frontend->state.qam.channel.sample_rate :
		V34_RECEIVE_PATH_SAMPLE_RATE;
}

enum v34_line_frontend_result v34_line_frontend_freeze_gain(
	struct v34_line_frontend *frontend)
{
	if (!v34_line_frontend_is_valid(frontend))
		return V34_LINE_FRONTEND_INVALID_STATE;
	if (frontend->mode == V34_LINE_FRONTEND_FIXED &&
	    v34_gain_control_set_mode(
		    &frontend->state.fixed.gain, V34_GAIN_FROZEN) !=
		    V34_GAIN_CONTROL_OK)
		return V34_LINE_FRONTEND_INVALID_STATE;
	return V34_LINE_FRONTEND_OK;
}

enum v34_line_frontend_result v34_line_frontend_sample(
	struct v34_line_frontend *frontend, int16_t sample,
	struct qam_point *point, bool *ready)
{
	struct v34_line_frontend next;
	struct qam_point generated = { 0 };
	bool generated_ready = false;

	if (!v34_line_frontend_is_valid(frontend))
		return V34_LINE_FRONTEND_INVALID_STATE;
	if (point == NULL || ready == NULL ||
	    memory_regions_overlap(
		    frontend, sizeof(*frontend), point, sizeof(*point)) ||
	    memory_regions_overlap(
		    frontend, sizeof(*frontend), ready, sizeof(*ready)) ||
	    memory_regions_overlap(point, sizeof(*point), ready, sizeof(*ready)))
		return V34_LINE_FRONTEND_INVALID_ARGUMENT;
	next = *frontend;
	if (next.mode == V34_LINE_FRONTEND_QAM) {
		if (qam_demodulator_sample(
			    &next.state.qam, sample, &generated,
			    &generated_ready) != QAM_OK)
			return V34_LINE_FRONTEND_INVALID_STATE;
	} else {
		struct v34_analytic_sample fixed_symbol;
		enum v34_receive_path_result result;

		if (!next.fixed_signal_started &&
		    sample >= -V34_FIXED_SIGNAL_LEVEL_MINIMUM &&
		    sample <= V34_FIXED_SIGNAL_LEVEL_MINIMUM) {
			generated_ready = false;
		} else {
			next.fixed_signal_started = true;
			result = v34_receive_path_sample(
				&next.state.fixed, sample, &fixed_symbol,
				&generated_ready);
			if (result != V34_RECEIVE_PATH_OK &&
			    result != V34_RECEIVE_PATH_SYMBOL_READY)
				return V34_LINE_FRONTEND_INVALID_STATE;
		}
		if (generated_ready &&
		    (next.state.fixed.timing.mode ==
			     V34_TIMING_CONTROL_INITIAL_PHASE ||
		     (fixed_symbol.in_phase == 0 &&
		      fixed_symbol.quadrature == 0)))
			generated_ready = false;
		if (generated_ready) {
			generated.in_phase = fixed_symbol.in_phase /
				V34_FIXED_TRAINING_AMPLITUDE;
			generated.quadrature = fixed_symbol.quadrature /
				V34_FIXED_TRAINING_AMPLITUDE;
		}
	}
	if (!v34_line_frontend_is_valid(&next))
		return V34_LINE_FRONTEND_INVALID_STATE;
	*frontend = next;
	*ready = generated_ready;
	if (generated_ready)
		*point = generated;
	return generated_ready ? V34_LINE_FRONTEND_SYMBOL_READY :
				 V34_LINE_FRONTEND_OK;
}
