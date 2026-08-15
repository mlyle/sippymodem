// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_PHASE3_ENTRY_H
#define PCM_PHASE3_ENTRY_H

#include "g711.h"
#include "pcm_phase2_info0_exchange.h"
#include "pcm_phase3_profile.h"
#include "v90_info0.h"

#include <stdbool.h>
#include <stdint.h>

struct v90_jd_parameters;
struct v90_phase3_analog_rx_configuration;
struct v90_phase3_analog_station;
struct v90_phase3_digital_station;

enum pcm_phase3_entry_result {
	PCM_PHASE3_ENTRY_OK,
	PCM_PHASE3_ENTRY_INVALID_ARGUMENT,
	PCM_PHASE3_ENTRY_INVALID_SELECTION,
	PCM_PHASE3_ENTRY_INVALID_STATE,
	PCM_PHASE3_ENTRY_UNUSABLE_SELECTION,
	PCM_PHASE3_ENTRY_ROLE_MISMATCH,
	PCM_PHASE3_ENTRY_MEDIA_MISMATCH,
	PCM_PHASE3_ENTRY_OUT_OF_RANGE,
	PCM_PHASE3_ENTRY_CLIPPING_RISK,
	PCM_PHASE3_ENTRY_INTERNAL_ERROR,
};

/* Immutable inputs selected for the one PCM Phase 3 owner. The retained INFO1
 * decision nails the protocol and upstream branch. conventional_profile is
 * meaningful only for a conventional V.34 upstream and is canonicalized to
 * begin at S rather than an MD interval. */
struct pcm_phase3_entry {
	struct pcm_info1_selection selection;
	struct v34_phase3_profile conventional_profile;
	enum g711_law law;
	enum v90_modem_role local_role;
};

bool pcm_phase3_entry_is_valid(
	const struct pcm_phase3_entry *entry);

/* Resolve a completed full V.90 or V.92 INFO exchange against the active SIP
 * media law. digital_info0 is the retained V.90-compatible record sent by the
 * digital modem, regardless of which endpoint is local. Rejected calls leave
 * entry unchanged. */
enum pcm_phase3_entry_result pcm_phase3_entry_init(
	struct pcm_phase3_entry *entry,
	enum v90_modem_role local_role, enum g711_law media_law,
	const struct v90_info0d *digital_info0,
	const struct pcm_info1_selection *selection);

/* Initialize the existing V.90/conventional child for entry.local_role.
 * V.92 and PCM-upstream entries are valid common entries but cannot enter
 * these V.90-only children. Rejected calls leave the station unchanged. */
enum pcm_phase3_entry_result pcm_phase3_entry_start_v90_analog(
	const struct pcm_phase3_entry *entry,
	int16_t nominal_peak_amplitude,
	struct v90_phase3_analog_station *station);

enum pcm_phase3_entry_result pcm_phase3_entry_start_v90_digital(
	const struct pcm_phase3_entry *entry,
	const struct v90_phase3_analog_rx_configuration *receive_configuration,
	const struct v90_jd_parameters *jd,
	struct v90_phase3_digital_station *station);

const char *pcm_phase3_entry_result_name(
	enum pcm_phase3_entry_result result);

#endif /* PCM_PHASE3_ENTRY_H */
