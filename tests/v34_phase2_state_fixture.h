// SPDX-License-Identifier: BSD-3-Clause
#ifndef V34_PHASE2_STATE_FIXTURE_H
#define V34_PHASE2_STATE_FIXTURE_H

#include <v34_phase2_reciprocal_probe.h>
#include <stddef.h>
#include <string.h>

/* Snapshot copies only: preserve every field, including invalid values used
 * to check rejection, without comparing padding introduced by INFO0 decode. */
static inline void v34_snapshot_info0(struct v34_info0 *info0)
{
	unsigned char bytes[sizeof(*info0)] = { 0 };
#define COPY(field) memcpy(bytes + offsetof(struct v34_info0, field), \
	&info0->field, sizeof(info0->field))
	COPY(symbol_rate_2743_supported);
	COPY(symbol_rate_2800_supported);
	COPY(symbol_rate_3429_supported);
	COPY(low_carrier_3000_supported);
	COPY(high_carrier_3000_supported);
	COPY(low_carrier_3200_supported);
	COPY(high_carrier_3200_supported);
	COPY(symbol_rate_3429_transmit_permitted);
	COPY(transmit_power_reduction_supported);
	COPY(maximum_symbol_rate_difference_steps);
	COPY(cme_modem);
	COPY(constellation_1664_supported);
	COPY(transmit_clock_source);
	COPY(acknowledges_peer_info0);
#undef COPY
	memcpy(info0, bytes, sizeof(bytes));
}

/* All other probe state is initialized by its owner. Keep its exact bytes,
 * including counters, captured samples, and floating-point representations. */
#define PROBE_EQUAL(name, type, exchange) \
static inline bool name(const struct type *left, const struct type *right) \
{ \
	struct type snapshots[2]; \
	memcpy(&snapshots[0], left, sizeof(*left)); \
	memcpy(&snapshots[1], right, sizeof(*right)); \
	for (size_t index = 0; index < 2; ++index) { \
		v34_snapshot_info0(&snapshots[index].exchange.local_info0); \
		v34_snapshot_info0(&snapshots[index].exchange.peer_info0); \
	} \
	return memcmp(&snapshots[0], &snapshots[1], sizeof(*left)) == 0; \
}

PROBE_EQUAL(v34_initial_probes_equal, v34_phase2_initial_probe,
	ranging.info0_exchange)
PROBE_EQUAL(v34_probe_turnarounds_equal, v34_phase2_probe_turnaround,
	initial_probe.ranging.info0_exchange)
PROBE_EQUAL(v34_reciprocal_probes_equal, v34_phase2_reciprocal_probe,
	turnaround.initial_probe.ranging.info0_exchange)
#undef PROBE_EQUAL

#endif
