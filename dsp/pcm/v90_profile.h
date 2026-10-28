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

/* Zero rate limits allow 4800..33600 bit/s. A zero symbol-rate ceiling
 * chooses the highest rate needed by that range (up to 3429 symbols/s). */
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

/* Select a common-row direct G.711 training constellation at the mapping's
 * current K and cardinality. Preserve companded-code spacing and reduce the
 * upper rail until exact mean-square power fits. Data uses the six-phase
 * selectors below. Transmit/codec planes must be identical and all intervals
 * share one row. Failure leaves the output unchanged. */
bool v90_profile_select_training_constellation(
	const struct v90_pcm_mapping *mapping, uint32_t maximum_power,
	struct pcm_constellation *constellation);

/* Initial common-row capacity for CP or CPu, sized for its own sign
 * redundancy. The call later selects levels under the measured power limit.
 * Failure leaves the output unchanged. */
bool v90_profile_generate_data_template(uint8_t rate_index,
	uint8_t sign_redundancy, struct pcm_constellation *constellation);

/* Preserve each interval's cardinality, using qualified levels
 * and the exact whole-frame power bound. Output is unchanged on failure. */
bool v90_profile_select_measured_constellations(
	const struct v90_pcm_mapping *mapping, uint32_t maximum_power,
	const struct pcm_constellation available[V90_CP_FRAME_INTERVALS],
	struct pcm_constellation rows[V90_CP_MAX_CONSTELLATIONS],
	uint8_t map[V90_CP_FRAME_INTERVALS], uint8_t *count);

/* Maximize signed distance over the common-row baseline, floor/ceiling
 * sixth-root assignments, and fixed families that trade capacity out of
 * scarce phases (minimum eight levels). Equal distance retains the original
 * family; other ties prefer the lower exact power ceiling, then family and
 * pattern order. Each phase retains its DIL-qualified mask; NULL permits the
 * direct-codec level set. Capacity and power are exact
 * over all 2^K words. Regenerate at every rate; explicit peer maps stay owned
 * by their caller. Rejected calls leave rows, map and count unchanged. */
bool v90_profile_generate_measured_constellations(enum g711_law law,
	uint8_t rate_index, uint8_t sign_redundancy, uint32_t maximum_power,
	const struct pcm_constellation available[V90_CP_FRAME_INTERVALS],
	struct pcm_constellation rows[V90_CP_MAX_CONSTELLATIONS],
	uint8_t map[V90_CP_FRAME_INTERVALS], uint8_t *count);

#endif /* V90_PROFILE_H */
