// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_carrier.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>

static const double V34_DATA_ROLLOFF = 0.1;
static const double V34_DATA_POWER_EPSILON = 1e-12;

static struct sippymodem_qam_channel data_channel(
	const struct v34_phase3_profile *profile,
	uint32_t sample_rate)
{
	return (struct sippymodem_qam_channel){
		.sample_rate = sample_rate,
		.symbol_rate_numerator = profile->symbol_rate_numerator,
		.symbol_rate_denominator = profile->symbol_rate_denominator,
		.carrier_frequency_numerator =
			profile->carrier_frequency_numerator,
		.carrier_frequency_denominator =
			profile->carrier_frequency_denominator,
		.rolloff = V34_DATA_ROLLOFF,
		.pulse_span_symbols = V34_DATA_PULSE_SPAN_SYMBOLS,
	};
}

static bool occupied_band_fits(
	const struct v34_phase3_profile *profile,
	uint32_t sample_rate)
{
	long double carrier =
		(long double)profile->carrier_frequency_numerator /
		profile->carrier_frequency_denominator;
	long double symbol_rate = (long double)profile->symbol_rate_numerator /
		profile->symbol_rate_denominator;
	long double upper_edge =
		carrier + 0.5L * (1.0L + V34_DATA_ROLLOFF) * symbol_rate;

	return upper_edge < 0.5L * sample_rate;
}

static bool channel_matches_configuration(
	const struct sippymodem_qam_modulator *modulator,
	const struct v34_phase3_profile *profile,
	const struct v34_power_normalizer *normalizer)
{
	const struct sippymodem_qam_channel *channel = &modulator->channel;

	return channel->symbol_rate_numerator ==
		       profile->symbol_rate_numerator &&
	       channel->symbol_rate_denominator ==
		       profile->symbol_rate_denominator &&
	       channel->carrier_frequency_numerator ==
		       profile->carrier_frequency_numerator &&
	       channel->carrier_frequency_denominator ==
		       profile->carrier_frequency_denominator &&
	       channel->rolloff == V34_DATA_ROLLOFF &&
	       channel->pulse_span_symbols ==
		       V34_DATA_PULSE_SPAN_SYMBOLS &&
	       modulator->amplitude == INT16_MAX &&
	       modulator->maximum_point_power ==
		       normalizer->maximum_normalized_point_power;
}

static bool point_fits(
	const struct sippymodem_qam_point *point, double maximum_power)
{
	double power;

	if (!isfinite(point->in_phase) || !isfinite(point->quadrature))
		return false;
	power = point->in_phase * point->in_phase +
		point->quadrature * point->quadrature;
	return isfinite(power) &&
	       (power <= maximum_power ||
		power / maximum_power <= 1.0 + V34_DATA_POWER_EPSILON);
}

bool v34_data_carrier_is_valid(
	const struct v34_data_carrier *carrier)
{
	if (carrier == NULL ||
	    !v34_phase3_profile_is_valid(&carrier->profile) ||
	    !v34_frame_geometry_is_valid(&carrier->geometry) ||
	    !v34_power_normalizer_is_valid(&carrier->normalizer) ||
	    !sippymodem_qam_modulator_is_valid(&carrier->modulator) ||
	    carrier->geometry.symbol_rate != carrier->profile.symbol_rate ||
	    carrier->geometry.primary_data_rate_2400_bps_units >
		    carrier->profile.projected_data_rate_2400_bps_units ||
	    carrier->geometry.high_mapping_frame_bits !=
		    carrier->normalizer.parameters.mapping_frame_bits ||
	    !occupied_band_fits(
		    &carrier->profile, carrier->modulator.channel.sample_rate) ||
	    !channel_matches_configuration(
		    &carrier->modulator, &carrier->profile,
		    &carrier->normalizer) ||
	    carrier->next_symbol > V34_SYMBOLS_PER_MAPPING_FRAME)
		return false;
	for (size_t index = 0;
	     index < V34_SYMBOLS_PER_MAPPING_FRAME; ++index) {
		if (!point_fits(
			    &carrier->normalized_symbol[index],
			    carrier->normalizer.maximum_normalized_point_power))
			return false;
	}
	return true;
}

bool v34_data_carrier_needs_mapping_frame(
	const struct v34_data_carrier *carrier)
{
	return v34_data_carrier_is_valid(carrier) &&
	       carrier->next_symbol == V34_SYMBOLS_PER_MAPPING_FRAME;
}

enum v34_data_carrier_result v34_data_carrier_init(
	struct v34_data_carrier *carrier,
	const struct v34_phase3_profile *profile,
	const struct v34_frame_geometry *geometry,
	const struct v34_power_normalizer *normalizer,
	uint32_t sample_rate)
{
	struct v34_data_carrier initialized = { 0 };
	struct sippymodem_qam_channel channel;

	if (carrier == NULL ||
	    !v34_phase3_profile_is_valid(profile) ||
	    !v34_frame_geometry_is_valid(geometry) ||
	    !v34_power_normalizer_is_valid(normalizer) ||
	    sample_rate == 0 || geometry->symbol_rate != profile->symbol_rate ||
	    geometry->primary_data_rate_2400_bps_units >
		    profile->projected_data_rate_2400_bps_units ||
	    geometry->high_mapping_frame_bits !=
		    normalizer->parameters.mapping_frame_bits ||
	    !occupied_band_fits(profile, sample_rate))
		return V34_DATA_CARRIER_INVALID_ARGUMENT;
	initialized.profile = *profile;
	initialized.geometry = *geometry;
	initialized.normalizer = *normalizer;
	initialized.next_symbol = V34_SYMBOLS_PER_MAPPING_FRAME;
	channel = data_channel(profile, sample_rate);
	if (sippymodem_qam_modulator_init_with_point_power_limit(
		    &initialized.modulator, &channel, INT16_MAX,
		    normalizer->maximum_normalized_point_power) !=
		    SIPPYMODEM_QAM_OK ||
	    !v34_data_carrier_is_valid(&initialized))
		return V34_DATA_CARRIER_INVALID_ARGUMENT;
	*carrier = initialized;
	return V34_DATA_CARRIER_OK;
}

static enum v34_data_carrier_result normalization_result(
	enum v34_symbol_power_result result)
{
	switch (result) {
	case V34_SYMBOL_POWER_OK:
		return V34_DATA_CARRIER_OK;
	case V34_SYMBOL_POWER_OUT_OF_RANGE:
		return V34_DATA_CARRIER_OUT_OF_RANGE;
	case V34_SYMBOL_POWER_NUMERIC_FAILURE:
		return V34_DATA_CARRIER_NUMERIC_FAILURE;
	case V34_SYMBOL_POWER_INVALID_ARGUMENT:
		break;
	}
	return V34_DATA_CARRIER_INVALID_ARGUMENT;
}

enum v34_data_carrier_result
v34_data_carrier_load_mapping_frame(
	struct v34_data_carrier *carrier,
	const struct v34_transmitted_mapping_frame *frame)
{
	struct sippymodem_qam_point
		normalized[V34_SYMBOLS_PER_MAPPING_FRAME];

	if (!v34_data_carrier_is_valid(carrier) || frame == NULL)
		return V34_DATA_CARRIER_INVALID_ARGUMENT;
	if (!v34_data_carrier_needs_mapping_frame(carrier))
		return V34_DATA_CARRIER_MAPPING_FRAME_PENDING;
	for (size_t index = 0;
	     index < V34_SYMBOLS_PER_MAPPING_FRAME; ++index) {
		enum v34_data_carrier_result result =
			normalization_result(v34_normalize_power(
				&carrier->normalizer, &frame->projected_symbol[index],
				&normalized[index]));

		if (result != V34_DATA_CARRIER_OK)
			return result;
	}
	for (size_t index = 0;
	     index < V34_SYMBOLS_PER_MAPPING_FRAME; ++index)
		carrier->normalized_symbol[index] = normalized[index];
	carrier->next_symbol = 0;
	return V34_DATA_CARRIER_OK;
}

enum v34_data_carrier_result v34_data_carrier_sample(
	struct v34_data_carrier *carrier, double *sample,
	bool *symbol_accepted)
{
	static const struct sippymodem_qam_point ZERO = { 0 };
	struct v34_data_carrier next;
	const struct sippymodem_qam_point *point;
	double generated;
	bool accepted;

	if (!v34_data_carrier_is_valid(carrier) || sample == NULL ||
	    symbol_accepted == NULL)
		return V34_DATA_CARRIER_INVALID_ARGUMENT;
	if (sippymodem_qam_modulator_can_accept_symbol(&carrier->modulator) &&
	    v34_data_carrier_needs_mapping_frame(carrier))
		return V34_DATA_CARRIER_MAPPING_FRAME_REQUIRED;
	next = *carrier;
	point = next.next_symbol < V34_SYMBOLS_PER_MAPPING_FRAME ?
		&next.normalized_symbol[next.next_symbol] :
		&ZERO;
	if (sippymodem_qam_modulator_sample_f64(
		    &next.modulator, point, &generated, &accepted) !=
	    SIPPYMODEM_QAM_OK)
		return V34_DATA_CARRIER_NUMERIC_FAILURE;
	if (accepted) {
		if (next.next_symbol >=
		    V34_SYMBOLS_PER_MAPPING_FRAME)
			return V34_DATA_CARRIER_NUMERIC_FAILURE;
		next.next_symbol++;
	}
	if (!isfinite(generated) ||
	    !v34_data_carrier_is_valid(&next))
		return V34_DATA_CARRIER_NUMERIC_FAILURE;
	*carrier = next;
	*sample = generated;
	*symbol_accepted = accepted;
	return V34_DATA_CARRIER_OK;
}

const char *v34_data_carrier_result_name(
	enum v34_data_carrier_result result)
{
	switch (result) {
	case V34_DATA_CARRIER_OK:
		return "ok";
	case V34_DATA_CARRIER_MAPPING_FRAME_REQUIRED:
		return "mapping frame required";
	case V34_DATA_CARRIER_MAPPING_FRAME_PENDING:
		return "mapping frame pending";
	case V34_DATA_CARRIER_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_DATA_CARRIER_OUT_OF_RANGE:
		return "out of range";
	case V34_DATA_CARRIER_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
