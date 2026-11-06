// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_B1_GAIN_H
#define V34_B1_GAIN_H

#include "v34_data_points.h"

enum { V34_B1_GAIN_FIT_POINTS = 8, V34_B1_GAIN_POINTS = 16 };

/* Reconstruct the known-one B1 waveform from negotiated receive parameters.
 * Fit real gain from eight complex points, then require residual improvement
 * on eight disjoint points. Decoder history and B1 error counts are retained. */
struct v34_b1_gain {
	struct qam_point expected[V34_B1_GAIN_POINTS];
	double normalization_gain;
	double correlation, energy, gain;
	double held_error, held_scaled_error;
	uint8_t count;
	bool active;
};

/* Optional acquisition: unsupported templates leave an inactive zero state.
 * Only receiver-negotiated parameters are used; no transmitter is borrowed. */
void v34_b1_gain_init(struct v34_b1_gain *fit,
	const struct v34_power_normalizer *normalizer,
	const struct v34_frame_geometry *geometry,
	const struct v34_q14_coefficient coefficients[V34_PRECODER_TAP_COUNT],
	enum v34_trellis_selection selection, enum v34_station_role role);
bool v34_b1_gain_is_valid(const struct v34_b1_gain *fit);

/* Call only after accepting an observation. Invalid evidence disables this
 * optional acquisition. Collection ends after sixteen points. */
void v34_b1_gain_observe(struct v34_b1_gain *fit,
	const struct v34_power_normalizer *normalizer,
	const struct qam_point *observed);

/* Retire a complete fit, applying only a qualified small correction. Invalid
 * or overlapping arguments are unchanged. No partial normalizer is published. */
bool v34_b1_gain_apply(struct v34_b1_gain *fit,
	struct v34_power_normalizer *normalizer);

#endif
