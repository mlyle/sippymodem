// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PROFILE_H
#define V90_PROFILE_H

#include "pcm_call.h"
#include "pcm_rate.h"

#include <stdbool.h>

enum {
	V90_PROFILE_MIN_DOWNSTREAM_BIT_RATE = 28000,
	V90_PROFILE_MAX_DOWNSTREAM_BIT_RATE = 56000,
};

/* Zero fields select the generated conventional-upstream defaults. */
struct v90_upstream_policy {
	unsigned int minimum_bit_rate;
	unsigned int maximum_bit_rate;
	/* Analogue INFO1a selection; digital INFO1d retains mandatory rates. */
	unsigned int maximum_symbol_rate;
	/* A digital station may omit the optional 3429-symbol/s INFO1d probe
	 * without withdrawing the mandatory 3000- and 3200-symbol/s probes. */
	bool suppress_optional_info1d_3429;
};

enum v90_profile_result {
	V90_PROFILE_OK,
	V90_PROFILE_INVALID_ARGUMENT,
	V90_PROFILE_UNSUPPORTED_RATE,
};

/* Build a generated exact-G.711 operating profile. Rate limits refer to the
 * PCM downstream; the conventional upstream is selected during MP/CP
 * exchange. */
enum v90_profile_result v90_profile_init(
	struct pcm_call_configuration *configuration,
	enum v90_modem_role role, enum g711_law law,
	unsigned int minimum_rate, unsigned int maximum_rate,
	const struct v90_upstream_policy *upstream);

/* Select a common-row direct G.711 constellation at the mapping's current
 * K, retaining its cardinality so later retrains can raise the rate again.
 * Data mappings maximize minimum signed linear-level distance within the
 * generated upper rail and exact power budget. Training keeps its existing
 * companded-code spacing. This does not estimate channel noise or change K.
 * Transmit and codec planes must be identical, with one shared row. Power
 * is mean square in 16-bit linear units. Failure leaves the output alone. */
bool v90_profile_select_constellation(
	const struct v90_pcm_mapping *mapping, uint32_t maximum_power,
	struct pcm_constellation *constellation);

/* Generated CP/CPu data maps use the smallest common row of at least eight
 * levels supporting the selected rate and sign redundancy, then maximize signed
 * distance under the exact power budget above. Regenerate on every rate
 * selection; explicit caller-owned mappings must not use this policy.
 * Failure leaves the output unchanged. */
bool v90_profile_generate_data_constellation(enum g711_law law,
	uint8_t rate_index, uint8_t sign_redundancy, uint32_t maximum_power,
	struct pcm_constellation *constellation);

const char *v90_profile_result_name(enum v90_profile_result result);

#endif /* V90_PROFILE_H */
