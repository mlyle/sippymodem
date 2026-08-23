// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE3_PROFILE_H
#define V34_PHASE3_PROFILE_H

#include "v34_info1.h"
#include "v34_station.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_phase3_profile_result {
	V34_PHASE3_PROFILE_OK,
	V34_PHASE3_PROFILE_INVALID_ARGUMENT,
	V34_PHASE3_PROFILE_UNUSABLE_SELECTION,
};

/* Rates remain exact: symbol rate is in symbols per second and carrier
 * frequency is in hertz. The power reduction is the peer's recommended
 * minimum, which is a compliant initial choice within its advertised range. */
struct v34_phase3_profile {
	uint32_t symbol_rate_numerator;
	uint32_t symbol_rate_denominator;
	uint32_t carrier_frequency_numerator;
	uint32_t carrier_frequency_denominator;
	uint32_t md_symbol_count;
	enum v34_symbol_rate symbol_rate;
	enum v34_station_role local_role;
	uint8_t transmit_power_reduction_db;
	uint8_t preemphasis_filter_index;
	uint8_t projected_data_rate_2400_bps_units;
	uint8_t md_length_35_ms_units;
	bool high_carrier;
};

bool v34_phase3_profile_is_valid(
	const struct v34_phase3_profile *profile);

bool v34_phase3_profile_is_equal(
	const struct v34_phase3_profile *left,
	const struct v34_phase3_profile *right);

/* Construct one exact transmit profile after the owning negotiation has
 * selected the station role, symbol rate, probe result, power, and MD
 * duration. Rejected calls leave profile unchanged. */
enum v34_phase3_profile_result v34_phase3_profile_init(
	struct v34_phase3_profile *profile,
	enum v34_station_role local_role,
	enum v34_symbol_rate symbol_rate,
	const struct v34_info1_probe_result *probe_result,
	uint8_t power_reduction_db, uint8_t md_length_35_ms_units);

/* The caller's transmit settings are selected by the peer's INFO1a. Its MD
 * duration remains the value the caller sent in its own INFO1c. */
enum v34_phase3_profile_result
v34_phase3_caller_profile(
	struct v34_phase3_profile *profile,
	const struct v34_info1c *local_info1c,
	const struct v34_info1a *peer_info1a);

/* The answerer selects its symbol rate in local INFO1a, while the matching
 * carrier and pre-emphasis settings came from the caller's INFO1c. */
enum v34_phase3_profile_result
v34_phase3_answerer_profile(
	struct v34_phase3_profile *profile,
	const struct v34_info1a *local_info1a,
	const struct v34_info1c *peer_info1c);

#endif
