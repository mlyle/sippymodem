// SPDX-License-Identifier: BSD-3-Clause

#include "v34_rate_profile.h"

#include <stddef.h>

enum rate_profile_index {
	RATE_PROFILE_2400,
	RATE_PROFILE_2800,
	RATE_PROFILE_3000,
	RATE_PROFILE_3200,
	RATE_PROFILE_3429,
	RATE_PROFILE_COUNT,
};

/* Fixed-point amplitude levels for this implementation.  The first row is
 * ordinary shaping and the second expanded; 2743 and 2800 symbol/s share one
 * profile. */
static const uint16_t RATE_LEVELS
	[RATE_PROFILE_COUNT][2][V34_RATE_PROFILE_UNITS] = {
	[RATE_PROFILE_2400] = {
		{ 0x11e3, 0x0dc7, 0x09d0, 0x06e7, 0x04e0, 0x0372, 0x0272,
		  0x01bb, 0x0139, 0, 0, 0, 0, 0 },
		{ 0x11e3, 0x0db7, 0x0a10, 0x0719, 0x0504, 0x0391, 0x0286,
		  0x01c8, 0x0143, 0, 0, 0, 0, 0 },
	},
	[RATE_PROFILE_2800] = {
		{ 0, 0x0f7e, 0x0b8d, 0x0868, 0x066a, 0x04b7, 0x0374,
		  0x0292, 0x01ea, 0x016b, 0x010a, 0, 0, 0 },
		{ 0, 0x0f7d, 0x0b94, 0x08a3, 0x0677, 0x04d6, 0x0392,
		  0x02aa, 0x01fb, 0x0178, 0x0118, 0, 0, 0 },
	},
	[RATE_PROFILE_3000] = {
		{ 0, 0x104d, 0x0b28, 0x095a, 0x06e3, 0x054c, 0x03ed,
		  0x0302, 0x0249, 0x01bc, 0x0154, 0x00fc, 0, 0 },
		{ 0, 0x103b, 0x0c33, 0x0955, 0x0714, 0x056f, 0x041c,
		  0x031f, 0x025e, 0x01c8, 0x015c, 0x0107, 0, 0 },
	},
	[RATE_PROFILE_3200] = {
		{ 0, 0x11e3, 0x0c3d, 0x09cb, 0x076b, 0x05ac, 0x0472,
		  0x0376, 0x02a9, 0x01fb, 0x0192, 0x0139, 0x00f0, 0 },
		{ 0, 0x11e3, 0x0c43, 0x0a0c, 0x07c1, 0x05f6, 0x04a5,
		  0x0393, 0x02c2, 0x0220, 0x01a4, 0x0143, 0x00fa, 0 },
	},
	[RATE_PROFILE_3429] = {
		{ 0, 0x11e3, 0x0d48, 0x0aac, 0x07ff, 0x066e, 0x0523,
		  0x03ed, 0x031e, 0x0272, 0x01ee, 0x0181, 0x012f, 0x00f0 },
		{ 0, 0x11e3, 0x0d4b, 0x0aaa, 0x0862, 0x06a3, 0x053f,
		  0x041f, 0x0339, 0x0285, 0x01fd, 0x0190, 0x0137, 0x00f6 },
	},
};

static const uint8_t RATE_PROFILES[] = {
	[V34_SYMBOL_RATE_2400] = RATE_PROFILE_2400,
	[V34_SYMBOL_RATE_2743] = RATE_PROFILE_2800,
	[V34_SYMBOL_RATE_2800] = RATE_PROFILE_2800,
	[V34_SYMBOL_RATE_3000] = RATE_PROFILE_3000,
	[V34_SYMBOL_RATE_3200] = RATE_PROFILE_3200,
	[V34_SYMBOL_RATE_3429] = RATE_PROFILE_3429,
};

_Static_assert(sizeof(RATE_PROFILES) / sizeof(RATE_PROFILES[0]) ==
	       V34_SYMBOL_RATE_COUNT, "Symbol clocks need amplitude profiles");

enum v34_rate_profile_result v34_rate_profile_level(
	enum v34_symbol_rate symbol_rate,
	uint8_t primary_data_rate_2400_bps_units, bool expanded_shaping,
	uint16_t *level)
{
	uint16_t selected;

	if (level == NULL || primary_data_rate_2400_bps_units == 0 ||
	    primary_data_rate_2400_bps_units > V34_RATE_PROFILE_UNITS ||
	    (unsigned)symbol_rate >= V34_SYMBOL_RATE_COUNT)
		return V34_RATE_PROFILE_INVALID_ARGUMENT;
	selected = RATE_LEVELS[RATE_PROFILES[symbol_rate]]
		[expanded_shaping ? 1u : 0u]
		[primary_data_rate_2400_bps_units - 1u];
	if (selected == 0)
		return V34_RATE_PROFILE_UNSUPPORTED_SELECTION;
	*level = selected;
	return V34_RATE_PROFILE_OK;
}
