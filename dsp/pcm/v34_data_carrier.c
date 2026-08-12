// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_carrier.h"

#include <limits.h>
#include <math.h>

static const double V34_DATA_ROLLOFF = 0.1;
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

enum v34_data_carrier_result v34_data_carrier_channel(
	const struct v34_phase3_profile *profile,
	uint32_t sample_rate, struct qam_channel *channel)
{
	struct qam_channel configured;

	if (!v34_phase3_profile_is_valid(profile) || sample_rate == 0 ||
	    channel == NULL)
		return V34_DATA_CARRIER_INVALID_ARGUMENT;
	if (!occupied_band_fits(profile, sample_rate))
		return V34_DATA_CARRIER_OUT_OF_RANGE;
	configured = (struct qam_channel){
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
	*channel = configured;
	return V34_DATA_CARRIER_OK;
}

bool v34_data_carrier_channel_matches(
	const struct v34_phase3_profile *profile,
	const struct qam_channel *channel)
{
	struct qam_channel expected;

	return channel != NULL &&
	       v34_data_carrier_channel(
		       profile, channel->sample_rate, &expected) ==
		       V34_DATA_CARRIER_OK &&
	       channel->sample_rate == expected.sample_rate &&
	       channel->symbol_rate_numerator ==
		       expected.symbol_rate_numerator &&
	       channel->symbol_rate_denominator ==
		       expected.symbol_rate_denominator &&
	       channel->carrier_frequency_numerator ==
		       expected.carrier_frequency_numerator &&
	       channel->carrier_frequency_denominator ==
		       expected.carrier_frequency_denominator &&
	       channel->rolloff == expected.rolloff &&
	       channel->pulse_span_symbols == expected.pulse_span_symbols;
}

static bool channel_matches_configuration(
	const struct qam_modulator *modulator,
	const struct v34_phase3_profile *profile,
	const struct v34_power_normalizer *normalizer)
{
	const struct qam_channel *channel = &modulator->channel;

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

bool v34_data_carrier_is_valid(
	const struct v34_data_carrier *carrier)
{
	if (carrier == NULL ||
	    !v34_phase3_profile_is_valid(&carrier->profile) ||
	    !v34_frame_geometry_is_valid(&carrier->geometry) ||
	    !v34_power_normalizer_is_valid(&carrier->normalizer) ||
	    !qam_modulator_is_valid(&carrier->modulator) ||
	    carrier->geometry.symbol_rate != carrier->profile.symbol_rate ||
	    carrier->geometry.primary_data_rate_2400_bps_units >
		    carrier->profile.projected_data_rate_2400_bps_units ||
	    carrier->geometry.high_mapping_frame_bits !=
		    carrier->normalizer.parameters.mapping_frame_bits ||
	    !occupied_band_fits(
		    &carrier->profile, carrier->modulator.channel.sample_rate) ||
	    !channel_matches_configuration(
		    &carrier->modulator, &carrier->profile,
		    &carrier->normalizer))
		return false;
	return true;
}

bool v34_data_carrier_needs_point(
	const struct v34_data_carrier *carrier)
{
	return v34_data_carrier_is_valid(carrier) &&
	       qam_modulator_can_accept_symbol(&carrier->modulator);
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
	if (v34_data_carrier_channel(profile, sample_rate, &channel) !=
	    V34_DATA_CARRIER_OK)
		return V34_DATA_CARRIER_INVALID_ARGUMENT;
	if (qam_modulator_init_with_point_power_limit(
		    &initialized.modulator, &channel, INT16_MAX,
		    normalizer->maximum_normalized_point_power) !=
		    QAM_OK ||
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
	static const struct qam_point ZERO = { 0 };
	struct v34_data_carrier next;
	double generated;
	bool accepting;
	bool accepted;

	if (!v34_data_carrier_is_valid(carrier) || sample == NULL ||
	    symbol_accepted == NULL)
		return V34_DATA_CARRIER_INVALID_ARGUMENT;
	accepting = qam_modulator_can_accept_symbol(&carrier->modulator);
	if ((point != NULL) != accepting)
		return V34_DATA_CARRIER_INVALID_ARGUMENT;
	next = *carrier;
	if (qam_modulator_sample_f64(
		    &next.modulator, point != NULL ? point : &ZERO,
		    &generated, &accepted) !=
	    QAM_OK)
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

const char *v34_data_carrier_result_name(
	enum v34_data_carrier_result result)
{
	switch (result) {
	case V34_DATA_CARRIER_OK:
		return "ok";
	case V34_DATA_CARRIER_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_DATA_CARRIER_OUT_OF_RANGE:
		return "out of range";
	case V34_DATA_CARRIER_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
