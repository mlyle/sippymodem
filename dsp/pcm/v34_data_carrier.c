// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_carrier.h"

#include <limits.h>
#include <math.h>

static const double V34_DATA_POINT_POWER_EPSILON = 1e-12;

static bool power_fits(double power, double maximum)
{
	return isfinite(power) && isfinite(maximum) && maximum > 0.0 &&
	       (power <= maximum ||
		power / maximum <= 1.0 + V34_DATA_POINT_POWER_EPSILON);
}

static bool channel_matches_configuration(
	const struct v34_data_carrier *carrier)
{
	const struct v34_pulse_modulator *modulator = &carrier->modulator;

	return v34_line_channel_matches(
		       &carrier->profile, &modulator->channel) &&
	       power_fits(
		       carrier->normalizer.maximum_normalized_point_power,
		       modulator->maximum_point_power);
}

static bool data_configuration_is_valid(
	const struct v34_phase3_profile *profile,
	const struct v34_frame_geometry *geometry,
	const struct v34_power_normalizer *normalizer)
{
	return v34_phase3_profile_is_valid(profile) &&
	       v34_frame_geometry_is_valid(geometry) &&
	       v34_power_normalizer_is_valid(normalizer) &&
	       geometry->symbol_rate == profile->symbol_rate &&
	       geometry->high_mapping_frame_bits ==
		       normalizer->parameters.mapping_frame_bits;
}

bool v34_data_carrier_is_valid(
	const struct v34_data_carrier *carrier)
{
	if (carrier == NULL ||
	    !data_configuration_is_valid(
		    &carrier->profile, &carrier->geometry,
		    &carrier->normalizer) ||
	    !v34_pulse_modulator_is_valid(&carrier->modulator) ||
	    !channel_matches_configuration(carrier))
		return false;
	return true;
}

bool v34_data_carrier_needs_point(
	const struct v34_data_carrier *carrier)
{
	return v34_data_carrier_is_valid(carrier) &&
	       v34_pulse_modulator_needs_point(&carrier->modulator);
}

enum v34_data_carrier_result v34_data_carrier_init(
	struct v34_data_carrier *carrier,
	const struct v34_phase3_profile *profile,
	const struct v34_frame_geometry *geometry,
	const struct v34_power_normalizer *normalizer,
	uint32_t sample_rate)
{
	struct v34_data_carrier initialized = { 0 };
	struct qam_channel channel;

	if (carrier == NULL ||
	    !data_configuration_is_valid(profile, geometry, normalizer) ||
	    sample_rate == 0)
		return V34_DATA_CARRIER_INVALID_ARGUMENT;
	initialized.profile = *profile;
	initialized.geometry = *geometry;
	initialized.normalizer = *normalizer;
	if (v34_line_channel_init(profile, sample_rate, &channel) !=
	    V34_LINE_CHANNEL_OK)
		return V34_DATA_CARRIER_INVALID_ARGUMENT;
	if (v34_pulse_modulator_init(
		    &initialized.modulator, profile, &channel,
		    normalizer->maximum_normalized_point_power) !=
		    V34_PULSE_MODULATOR_OK ||
	    !v34_data_carrier_is_valid(&initialized))
		return V34_DATA_CARRIER_INVALID_ARGUMENT;
	*carrier = initialized;
	return V34_DATA_CARRIER_OK;
}

enum v34_data_carrier_result v34_data_carrier_sample(
	struct v34_data_carrier *carrier,
	const struct qam_point *point, double *sample,
	bool *symbol_accepted)
{
	struct v34_data_carrier next;
	double generated;
	bool accepting;
	bool accepted;

	if (!v34_data_carrier_is_valid(carrier) || sample == NULL ||
	    symbol_accepted == NULL)
		return V34_DATA_CARRIER_INVALID_ARGUMENT;
	accepting = v34_pulse_modulator_needs_point(&carrier->modulator);
	if ((point != NULL) != accepting ||
	    (point != NULL && !qam_point_fits_power(
			      point, carrier->normalizer
					     .maximum_normalized_point_power)))
		return V34_DATA_CARRIER_INVALID_ARGUMENT;
	next = *carrier;
	if (v34_pulse_modulator_sample(
		    &next.modulator, point,
		    &generated, &accepted) !=
	    V34_PULSE_MODULATOR_OK)
		return point != NULL ? V34_DATA_CARRIER_INVALID_ARGUMENT :
				       V34_DATA_CARRIER_NUMERIC_FAILURE;
	if (accepted != accepting || !isfinite(generated) ||
	    !v34_data_carrier_is_valid(&next))
		return V34_DATA_CARRIER_NUMERIC_FAILURE;
	*carrier = next;
	*sample = generated;
	*symbol_accepted = accepted;
	return V34_DATA_CARRIER_OK;
}
