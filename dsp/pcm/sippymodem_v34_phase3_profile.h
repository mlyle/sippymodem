// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE3_PROFILE_H
#define SIPPYMODEM_V34_PHASE3_PROFILE_H

#include "sippymodem_v34_info1.h"
#include "sippymodem_v34_station.h"

#include <stdbool.h>
#include <stdint.h>

enum sippymodem_v34_phase3_profile_result {
	SIPPYMODEM_V34_PHASE3_PROFILE_OK,
	SIPPYMODEM_V34_PHASE3_PROFILE_INVALID_ARGUMENT,
	SIPPYMODEM_V34_PHASE3_PROFILE_UNUSABLE_SELECTION,
};

/* Rates remain exact: symbol rate is in symbols per second and carrier
 * frequency is in hertz. The power reduction is the peer's recommended
 * minimum, which is a compliant initial choice within its advertised range. */
struct sippymodem_v34_phase3_profile {
	uint32_t symbol_rate_numerator;
	uint32_t symbol_rate_denominator;
	uint32_t carrier_frequency_numerator;
	uint32_t carrier_frequency_denominator;
	uint32_t md_symbol_count;
	enum sippymodem_v34_symbol_rate symbol_rate;
	enum sippymodem_v34_station_role local_role;
	uint8_t transmit_power_reduction_db;
	uint8_t preemphasis_filter_index;
	uint8_t projected_data_rate_2400_bps_units;
	uint8_t md_length_35_ms_units;
	bool high_carrier;
};

bool sippymodem_v34_phase3_profile_is_valid(
	const struct sippymodem_v34_phase3_profile *profile);

/* The caller's transmit settings are selected by the peer's INFO1a. Its MD
 * duration remains the value the caller sent in its own INFO1c. */
enum sippymodem_v34_phase3_profile_result
sippymodem_v34_phase3_caller_profile(
	struct sippymodem_v34_phase3_profile *profile,
	const struct sippymodem_v34_info1c *local_info1c,
	const struct sippymodem_v34_info1a *peer_info1a);

/* The answerer selects its symbol rate in local INFO1a, while the matching
 * carrier and pre-emphasis settings came from the caller's INFO1c. */
enum sippymodem_v34_phase3_profile_result
sippymodem_v34_phase3_answerer_profile(
	struct sippymodem_v34_phase3_profile *profile,
	const struct sippymodem_v34_info1a *local_info1a,
	const struct sippymodem_v34_info1c *peer_info1c);

const char *sippymodem_v34_phase3_profile_result_name(
	enum sippymodem_v34_phase3_profile_result result);

#endif
