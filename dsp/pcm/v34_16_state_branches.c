// SPDX-License-Identifier: BSD-3-Clause

#include "v34_16_state_branches.h"

#include "memory_region.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

enum {
	V34_RATE_PROFILE_WORDS = V34_INFO1_MAX_DATA_RATE_2400_BPS_UNITS,
	V34_SHAPING_PROFILE_COUNT = 2,
};

/* These scale profiles are normative for the fixed V.34 receive metric. The
 * 2743 and 2800 symbol/s modes deliberately share the 2800 profile. */
static const uint16_t RATE_SCALE_2400[V34_SHAPING_PROFILE_COUNT]
	[V34_RATE_PROFILE_WORDS] = {
	{
		0x11e3, 0x0dc7, 0x09d0, 0x06e7, 0x04e0, 0x0372, 0x0272,
		0x01bb, 0x0139, 0, 0, 0, 0, 0,
	},
	{
		0x11e3, 0x0db7, 0x0a10, 0x0719, 0x0504, 0x0391, 0x0286,
		0x01c8, 0x0143, 0, 0, 0, 0, 0,
	},
};

static const uint16_t RATE_SCALE_2800[V34_SHAPING_PROFILE_COUNT]
	[V34_RATE_PROFILE_WORDS] = {
	{
		0, 0x0f7e, 0x0b8d, 0x0868, 0x066a, 0x04b7, 0x0374,
		0x0292, 0x01ea, 0x016b, 0x010a, 0, 0, 0,
	},
	{
		0, 0x0f7d, 0x0b94, 0x08a3, 0x0677, 0x04d6, 0x0392,
		0x02aa, 0x01fb, 0x0178, 0x0118, 0, 0, 0,
	},
};

static const uint16_t RATE_SCALE_3000[V34_SHAPING_PROFILE_COUNT]
	[V34_RATE_PROFILE_WORDS] = {
	{
		0, 0x104d, 0x0b28, 0x095a, 0x06e3, 0x054c, 0x03ed,
		0x0302, 0x0249, 0x01bc, 0x0154, 0x00fc, 0, 0,
	},
	{
		0, 0x103b, 0x0c33, 0x0955, 0x0714, 0x056f, 0x041c,
		0x031f, 0x025e, 0x01c8, 0x015c, 0x0107, 0, 0,
	},
};

static const uint16_t RATE_SCALE_3200[V34_SHAPING_PROFILE_COUNT]
	[V34_RATE_PROFILE_WORDS] = {
	{
		0, 0x11e3, 0x0c3d, 0x09cb, 0x076b, 0x05ac, 0x0472,
		0x0376, 0x02a9, 0x01fb, 0x0192, 0x0139, 0x00f0, 0,
	},
	{
		0, 0x11e3, 0x0c43, 0x0a0c, 0x07c1, 0x05f6, 0x04a5,
		0x0393, 0x02c2, 0x0220, 0x01a4, 0x0143, 0x00fa, 0,
	},
};

static const uint16_t RATE_SCALE_3429[V34_SHAPING_PROFILE_COUNT]
	[V34_RATE_PROFILE_WORDS] = {
	{
		0, 0x11e3, 0x0d48, 0x0aac, 0x07ff, 0x066e, 0x0523,
		0x03ed, 0x031e, 0x0272, 0x01ee, 0x0181, 0x012f, 0x00f0,
	},
	{
		0, 0x11e3, 0x0d4b, 0x0aaa, 0x0862, 0x06a3, 0x053f,
		0x041f, 0x0339, 0x0285, 0x01fd, 0x0190, 0x0137, 0x00f6,
	},
};

static const uint16_t (*rate_scale(enum v34_symbol_rate symbol_rate))
	[V34_RATE_PROFILE_WORDS]
{
	switch (symbol_rate) {
	case V34_SYMBOL_RATE_2400:
		return RATE_SCALE_2400;
	case V34_SYMBOL_RATE_2743:
	case V34_SYMBOL_RATE_2800:
		return RATE_SCALE_2800;
	case V34_SYMBOL_RATE_3000:
		return RATE_SCALE_3000;
	case V34_SYMBOL_RATE_3200:
		return RATE_SCALE_3200;
	case V34_SYMBOL_RATE_3429:
		return RATE_SCALE_3429;
	case V34_SYMBOL_RATE_COUNT:
		break;
	}
	return NULL;
}

static bool derive_profile(
	enum v34_symbol_rate symbol_rate,
	uint8_t primary_data_rate_2400_bps_units, bool expanded_shaping,
	struct v34_16_state_branch_profile *profile)
{
	const uint16_t (*scale)[V34_RATE_PROFILE_WORDS] =
		rate_scale(symbol_rate);
	uint16_t metric;
	uint8_t width = 7;

	if (scale == NULL || primary_data_rate_2400_bps_units == 0 ||
	    primary_data_rate_2400_bps_units > V34_RATE_PROFILE_WORDS)
		return false;
	metric = (uint16_t)(scale[expanded_shaping ? 1u : 0u]
		[primary_data_rate_2400_bps_units - 1u] >> 1u);
	if (metric == 0)
		return false;
	while ((UINT32_C(1) << width) < metric)
		width++;
	*profile = (struct v34_16_state_branch_profile){
		.symbol_rate = symbol_rate,
		.metric = metric,
		.primary_data_rate_2400_bps_units =
			primary_data_rate_2400_bps_units,
		.distance_shift = (uint8_t)(width + 1u),
		.expanded_shaping = expanded_shaping,
	};
	return true;
}

bool v34_16_state_branch_profile_is_valid(
	const struct v34_16_state_branch_profile *profile)
{
	struct v34_16_state_branch_profile derived;

	return profile != NULL && derive_profile(
		profile->symbol_rate,
		profile->primary_data_rate_2400_bps_units,
		profile->expanded_shaping, &derived) &&
	       profile->symbol_rate == derived.symbol_rate &&
	       profile->metric == derived.metric &&
	       profile->primary_data_rate_2400_bps_units ==
		       derived.primary_data_rate_2400_bps_units &&
	       profile->distance_shift == derived.distance_shift &&
	       profile->expanded_shaping == derived.expanded_shaping;
}

enum v34_16_state_branches_result v34_16_state_branch_profile_init(
	struct v34_16_state_branch_profile *profile,
	const struct v34_frame_geometry *geometry, bool expanded_shaping)
{
	struct v34_16_state_branch_profile derived;

	if (profile == NULL || geometry == NULL || memory_regions_overlap(
		    profile, sizeof(*profile), geometry, sizeof(*geometry)) ||
	    !v34_frame_geometry_is_valid(geometry))
		return V34_16_STATE_BRANCHES_INVALID_ARGUMENT;
	if (!derive_profile(
		    geometry->symbol_rate,
		    geometry->primary_data_rate_2400_bps_units,
		    expanded_shaping, &derived))
		return V34_16_STATE_BRANCHES_UNSUPPORTED_SELECTION;
	*profile = derived;
	return V34_16_STATE_BRANCHES_OK;
}

static bool fixed_coordinate(
	double normalized, uint16_t metric, int16_t *fixed)
{
	double scaled = normalized * metric;
	long rounded;

	if (!isfinite(scaled) || scaled < (double)INT16_MIN - 0.5 ||
	    scaled > (double)INT16_MAX + 0.5)
		return false;
	rounded = lround(scaled);
	if (rounded < INT16_MIN || rounded > INT16_MAX)
		return false;
	*fixed = (int16_t)rounded;
	return true;
}

enum v34_16_state_branches_result v34_16_state_branch_quantize(
	const struct v34_16_state_branch_profile *profile,
	const struct qam_point *normalized,
	struct v34_analytic_sample *fixed)
{
	struct v34_analytic_sample generated;

	if (profile == NULL || normalized == NULL || fixed == NULL ||
	    memory_regions_overlap(
		    fixed, sizeof(*fixed), profile, sizeof(*profile)) ||
	    memory_regions_overlap(
		    fixed, sizeof(*fixed), normalized, sizeof(*normalized)) ||
	    !v34_16_state_branch_profile_is_valid(profile) ||
	    !isfinite(normalized->in_phase) ||
	    !isfinite(normalized->quadrature))
		return V34_16_STATE_BRANCHES_INVALID_ARGUMENT;
	if (!fixed_coordinate(
		    normalized->in_phase, profile->metric,
		    &generated.in_phase) ||
	    !fixed_coordinate(
		    normalized->quadrature, profile->metric,
		    &generated.quadrature))
		return V34_16_STATE_BRANCHES_OUT_OF_RANGE;
	*fixed = generated;
	return V34_16_STATE_BRANCHES_OK;
}

struct axis_pair {
	int16_t fixed[2];
	int16_t grid[2];
};

static struct axis_pair odd_axis_pair(int16_t value, uint16_t metric)
{
	int quotient = value / (int)metric;
	int center_grid;
	int alternate_grid;
	int16_t center_fixed;

	if (value > 0)
		quotient++;
	center_grid = (quotient & ~3) + 1;
	center_fixed = (int16_t)(uint16_t)(center_grid * (int)metric);
	alternate_grid = center_grid + (value > center_fixed ? 2 : -2);
	return (struct axis_pair){
		.fixed = {
			center_fixed,
			(int16_t)(uint16_t)(alternate_grid * (int)metric),
		},
		.grid = { (int16_t)center_grid, (int16_t)alternate_grid },
	};
}

static uint8_t path_axis(uint8_t path, bool quadrature)
{
	uint8_t in_phase = (uint8_t)((path >> 1u) & 1u);

	return quadrature ? (uint8_t)(in_phase ^ (path & 1u)) : in_phase;
}

static uint16_t axis_distance(
	int16_t sample, int16_t candidate, uint8_t shift)
{
	int32_t difference =
		(int16_t)((uint16_t)sample - (uint16_t)candidate);
	uint64_t squared = (uint64_t)((int64_t)difference * difference);

	return (uint16_t)(squared >> shift);
}

static uint16_t path_distance(
	struct v34_analytic_sample sample, const struct axis_pair axes[2],
	uint8_t path, uint8_t shift)
{
	uint16_t in_phase = axis_distance(
		sample.in_phase, axes[0].fixed[path_axis(path, false)], shift);
	uint16_t quadrature = axis_distance(
		sample.quadrature,
		axes[1].fixed[path_axis(path, true)], shift);

	return (uint16_t)(in_phase + quadrature);
}

static struct v34_grid_point path_point(
	const struct axis_pair axes[2], uint8_t path)
{
	return (struct v34_grid_point){
		.in_phase = axes[0].grid[path_axis(path, false)],
		.quadrature = axes[1].grid[path_axis(path, true)],
	};
}

static const uint8_t NORMAL_PAIRS[16] = {
	0, 10, 5, 15, 2, 8, 7, 13,
	1, 11, 6, 12, 3, 9, 4, 14,
};

static const uint8_t INVERTED_PAIRS[16] = {
	1, 11, 6, 12, 3, 9, 4, 14,
	0, 10, 5, 15, 2, 8, 7, 13,
};

enum v34_16_state_branches_result v34_16_state_branches_build(
	const struct v34_16_state_branch_profile *profile,
	const struct v34_analytic_sample
		samples[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	uint8_t inversion_bit,
	struct v34_16_state_branch
		branches[V34_16_STATE_BRANCH_COUNT])
{
	struct axis_pair axes[V34_2D_SYMBOLS_PER_4D_INTERVAL][2];
	uint16_t distance[V34_2D_SYMBOLS_PER_4D_INTERVAL][4];
	struct v34_16_state_branch generated[V34_16_STATE_BRANCH_COUNT];
	const uint8_t *pairs;

	if (profile == NULL || samples == NULL || branches == NULL ||
	    inversion_bit > 1u || memory_regions_overlap(
		    branches, sizeof(generated), profile, sizeof(*profile)) ||
	    memory_regions_overlap(
		    branches, sizeof(generated), samples,
		    sizeof(*samples) * V34_2D_SYMBOLS_PER_4D_INTERVAL) ||
	    !v34_16_state_branch_profile_is_valid(profile))
		return V34_16_STATE_BRANCHES_INVALID_ARGUMENT;
	for (uint8_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		axes[symbol][0] = odd_axis_pair(
			samples[symbol].in_phase, profile->metric);
		axes[symbol][1] = odd_axis_pair(
			samples[symbol].quadrature, profile->metric);
		for (uint8_t path = 0; path < 4; ++path)
			distance[symbol][path] = path_distance(
				samples[symbol], axes[symbol], path,
				profile->distance_shift);
	}
	pairs = inversion_bit == 0 ? NORMAL_PAIRS : INVERTED_PAIRS;
	for (uint8_t branch = 0; branch < V34_16_STATE_BRANCH_COUNT;
	     ++branch) {
		uint8_t first = pairs[branch * 2u];
		uint8_t second = pairs[branch * 2u + 1u];
		uint16_t first_metric = (uint16_t)(
			distance[0][first >> 2u] + distance[1][first & 3u]);
		uint16_t second_metric = (uint16_t)(
			distance[0][second >> 2u] +
			distance[1][second & 3u]);
		uint8_t selected = first;

		if ((int16_t)second_metric < (int)first_metric) {
			selected = second;
			first_metric = second_metric;
		}
		generated[branch] = (struct v34_16_state_branch){
			.decision = {
				.channel_output = {
					path_point(axes[0], selected >> 2u),
					path_point(axes[1], selected & 3u),
				},
				.convolutional_output = (uint8_t)(branch >> 2u),
				.inversion_bit = inversion_bit,
			},
			.metric = first_metric,
		};
		if (!v34_survivor_decision_is_valid(
			    &generated[branch].decision))
			return V34_16_STATE_BRANCHES_OUT_OF_RANGE;
	}
	memcpy(branches, generated, sizeof(generated));
	return V34_16_STATE_BRANCHES_OK;
}
