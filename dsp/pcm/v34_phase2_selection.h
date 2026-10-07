// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE2_SELECTION_H
#define V34_PHASE2_SELECTION_H

#include "v34_info0.h"
#include "v34_phase2_probe_receiver.h"
#include "v34_phase3_profile.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_phase2_selection_result {
	V34_PHASE2_SELECTION_OK,
	V34_PHASE2_SELECTION_INVALID_ARGUMENT,
	V34_PHASE2_SELECTION_INCONSISTENT_INFO1C,
	V34_PHASE2_SELECTION_INCONSISTENT_INFO1A,
	V34_PHASE2_SELECTION_NO_COMMON_PROFILE,
};

/* Completed Phase 2 records and receiver state for entering Phase 3 without
 * replaying INFO0, probing, or INFO1. */
struct v34_phase2_handoff {
	struct v34_info0 local_info0;
	struct v34_info0 peer_info0;
	struct v34_info1c caller_info1c;
	struct v34_info1a answerer_info1a;
	uint16_t round_trip_delay_samples;
	int16_t receive_gain;
	enum v34_station_role local_role;
};

bool v34_phase2_handoff_is_valid(
	const struct v34_phase2_handoff *handoff);

/* Validate the retained line state and derive both role-owned Phase 3
 * profiles. Rejected calls leave both destinations unchanged. */
enum v34_phase2_selection_result v34_phase2_handoff_profiles(
	const struct v34_phase2_handoff *handoff,
	struct v34_phase3_profile *caller_transmit,
	struct v34_phase3_profile *answerer_transmit);

/* Select the peer transmitter's pre-emphasis from the received L2 spectrum.
 * The four source bands and their per-filter slopes are the V.34 probe
 * profiles; 2743 and 2800 symbol/s share one pulse-shaping geometry. */
enum v34_phase2_selection_result v34_phase2_select_preemphasis(
	const struct v34_phase2_probe_receiver_measurement *measurement,
	enum v34_symbol_rate symbol_rate, uint8_t *filter_index);

/* Build V.90-format digital INFO1d probe groups from the measured analogue
 * modem's L2 spectrum. Nonzero configured groups enable one symbol rate;
 * their placeholder projection and filter values are not retained. MD and
 * frequency-offset fields remain with their configured owners; the spectrum
 * alone does not establish a frequency offset. Peer INFO0 capabilities bound
 * carriers, symbol rates, constellation size and power allowance. Unsupported
 * groups remain zero; the measured carrier is preferred when permitted. */
enum v34_phase2_selection_result v34_phase2_select_v90_info1d(
	const struct v34_phase2_probe_receiver_measurement *measurement,
	const struct v34_info1c *configured,
	const struct v34_info0 *peer,
	struct v34_info1c *selected);

/* INFO1c describes the answerer's possible transmit profiles. A usable entry
 * must be supported by both endpoints and by the answerer's advertised
 * transmitter configuration. Power-reduction requests are likewise bounded
 * by the answerer's INFO0. */
bool v34_phase2_info1c_is_consistent(
	const struct v34_info0 *caller_info0,
	const struct v34_info0 *answerer_info0,
	const struct v34_info1c *info1c);

/* INFO1a selects one direction from INFO1c and supplies the caller's selected
 * transmit profile. Both rates must respect each endpoint's INFO0 asymmetry
 * limit. */
bool v34_phase2_info1a_is_consistent(
	const struct v34_info0 *caller_info0,
	const struct v34_info0 *answerer_info0,
	const struct v34_info1c *info1c,
	const struct v34_info1a *info1a);

/* Preserve a consistent preferred INFO1a selection. If the peer has withdrawn
 * that answer-transmit entry, choose the highest no-faster symmetric profile
 * supported by both endpoints. The selected caller projection is clamped to
 * the implemented limit for the fallback symbol rate. */
enum v34_phase2_selection_result v34_phase2_select_answerer_info1a(
	const struct v34_info0 *caller_info0,
	const struct v34_info0 *answerer_info0,
	const struct v34_info1c *info1c,
	const struct v34_info1a *preferred,
	struct v34_info1a *selected);

/* Validate the complete Phase 2 selection and derive both transmitter-owned
 * Phase 3 profiles atomically. The outputs must be distinct. */
enum v34_phase2_selection_result v34_phase2_select_profiles(
	const struct v34_info0 *caller_info0,
	const struct v34_info0 *answerer_info0,
	const struct v34_info1c *info1c,
	const struct v34_info1a *info1a,
	struct v34_phase3_profile *caller_transmit,
	struct v34_phase3_profile *answerer_transmit);

#endif /* V34_PHASE2_SELECTION_H */
