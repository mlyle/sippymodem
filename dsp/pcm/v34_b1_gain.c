// SPDX-License-Identifier: BSD-3-Clause

#include "v34_b1_gain.h"

#include "memory_region.h"

#include <math.h>
#include <string.h>

bool v34_b1_gain_is_valid(const struct v34_b1_gain *fit)
{
	if (fit == NULL || fit->count > V34_B1_GAIN_POINTS)
		return false;
	if (!fit->active)
		return true;
	if (!isfinite(fit->normalization_gain) || fit->normalization_gain <= 0 ||
	    !isfinite(fit->correlation) || !isfinite(fit->energy) || fit->energy < 0 ||
	    !isfinite(fit->held_error) || fit->held_error < 0 ||
	    !isfinite(fit->held_scaled_error) || fit->held_scaled_error < 0 ||
	    (fit->count >= V34_B1_GAIN_FIT_POINTS &&
	     (!isfinite(fit->gain) || fit->gain <= 0 || fit->energy <= 0)))
		return false;
	for (unsigned i = 0; i < V34_B1_GAIN_POINTS; ++i)
		if (!point_is_finite(&fit->expected[i]))
			return false;
	return true;
}

void v34_b1_gain_init(struct v34_b1_gain *fit,
	const struct v34_power_normalizer *normalizer,
	const struct v34_frame_geometry *geometry,
	const struct v34_q14_coefficient coefficients[V34_PRECODER_TAP_COUNT],
	enum v34_trellis_selection selection, enum v34_station_role role)
{
	struct v34_b1_gain initialized = { 0 };
	struct v34_frame_schedule schedule;
	struct v34_scrambler scrambler;
	struct v34_4d_encoder encoder;
	struct v34_data_points points;

	if (fit == NULL)
		return;
	if (v34_b1_frame_schedule_init(&schedule, geometry) != V34_FRAME_SCHEDULE_OK ||
	    v34_transmit_scrambler_init(&scrambler, role) != V34_FRAMING_OK ||
	    normalizer == NULL ||
	    v34_4d_encoder_init(&encoder, &normalizer->parameters,
		    coefficients, selection) != V34_4D_ENCODER_OK ||
	    v34_data_points_init(&points, &schedule, &scrambler, &encoder,
		    normalizer, role) != V34_DATA_POINTS_OK)
		goto unsupported;
	for (unsigned i = 0; i < V34_B1_GAIN_POINTS; ++i) {
		if (v34_data_points_needs_mapping_frame(&points)) {
			struct v34_mapping_frame frame;
			uint8_t ones[V34_MAX_MAPPING_FRAME_BITS], auxiliary = 1;

			memset(ones, 1, sizeof(ones));
			if (v34_data_points_preview_mapping_frame(&points, &frame) != V34_DATA_POINTS_OK ||
			    v34_data_points_queue_mapping_frame(&points, ones, frame.primary_bits,
				    frame.carries_auxiliary_bit ? &auxiliary : NULL) != V34_DATA_POINTS_OK)
				goto unsupported;
		}
		if (v34_data_points_next(&points, &initialized.expected[i]) != V34_DATA_POINTS_OK)
			goto unsupported;
	}
	initialized.normalization_gain = normalizer->normalization_gain;
	initialized.active = true;
	*fit = initialized;
	return;
unsupported:
	*fit = (struct v34_b1_gain){ 0 };
}

void v34_b1_gain_observe(struct v34_b1_gain *fit,
	const struct v34_power_normalizer *normalizer,
	const struct qam_point *observed)
{
	if (fit == NULL || !fit->active || fit->count >= V34_B1_GAIN_POINTS)
		return;
	if (!v34_b1_gain_is_valid(fit) || !point_is_finite(observed) || normalizer == NULL ||
	    normalizer->normalization_gain != fit->normalization_gain) {
		fit->active = false;
		return;
	}
	const struct qam_point *expected = &fit->expected[fit->count];
	if (fit->count < V34_B1_GAIN_FIT_POINTS) {
		fit->correlation += expected->in_phase * observed->in_phase +
			expected->quadrature * observed->quadrature;
		fit->energy += expected->in_phase * expected->in_phase +
			expected->quadrature * expected->quadrature;
		if (fit->count + 1u == V34_B1_GAIN_FIT_POINTS)
			fit->gain = fit->energy > 0 ? fit->correlation / fit->energy : 0;
	} else {
		double ip = observed->in_phase - expected->in_phase;
		double qp = observed->quadrature - expected->quadrature;

		fit->held_error += ip * ip + qp * qp;
		ip = observed->in_phase - fit->gain * expected->in_phase;
		qp = observed->quadrature - fit->gain * expected->quadrature;
		fit->held_scaled_error += ip * ip + qp * qp;
	}
	fit->count++;
	if (!v34_b1_gain_is_valid(fit))
		fit->active = false;
}

bool v34_b1_gain_apply(struct v34_b1_gain *fit,
	struct v34_power_normalizer *normalizer)
{
	if (fit == NULL || normalizer == NULL ||
	    memory_regions_overlap(fit, sizeof(*fit), normalizer, sizeof(*normalizer)) ||
	    !v34_b1_gain_is_valid(fit) || !fit->active ||
	    fit->count != V34_B1_GAIN_POINTS || !v34_power_normalizer_is_valid(normalizer))
		return false;
	fit->active = false;
	/* Refine a calibration, not an arbitrary amplitude/phase error. The
	 * held observations may verify this gain but cannot refit it. */
	double db = 20.0 * log10(fit->gain);
	if (normalizer->normalization_gain != fit->normalization_gain ||
	    !isfinite(db) || fabs(db) < 0.10 || fabs(db) > 0.50 ||
	    fit->held_error <= 0 ||
	    0.5 * fit->held_error < fit->held_scaled_error)
		return false;
	return v34_power_normalizer_set_gain(normalizer,
		normalizer->normalization_gain * fit->gain) == V34_SYMBOL_POWER_OK;
}
