// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_PHASE3_ENTRY_H
#define PCM_PHASE3_ENTRY_H

#include "g711.h"
#include "pcm_phase3_profile.h"
#include "v90_info0.h"
#include "pcm_phase2_info0_exchange.h"

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

/* Immutable inputs shared by the two role-specific zero-DIL Phase 3 owners.
 * The line profile describes the analogue modem's conventional upstream in
 * both roles. V.90 Phase 3 starts at S rather than MD, so the retained profile
 * is deliberately canonicalized to an empty MD interval. */
struct pcm_phase3_entry {
	struct v34_phase3_profile analog_profile;
	enum g711_law law;
	enum v90_modem_role local_role;
	uint8_t training_ucode;
};

bool pcm_phase3_entry_is_valid(
	const struct pcm_phase3_entry *entry);

/* Resolve a completed full V.90 INFO exchange against the active SIP media
 * law. digital_info0 is the retained record sent by the digital modem,
 * regardless of which endpoint is local. Rejected calls leave entry
 * unchanged. */
enum pcm_phase3_entry_result pcm_phase3_entry_init(
	struct pcm_phase3_entry *entry,
	enum v90_modem_role local_role, enum g711_law media_law,
	const struct v90_info0d *digital_info0,
	const struct pcm_info1_selection *selection);

/* Initialize exactly the station selected by entry.local_role. Rejected
 * calls leave the station unchanged. */
enum pcm_phase3_entry_result pcm_phase3_entry_start_analog(
	const struct pcm_phase3_entry *entry,
	int16_t nominal_peak_amplitude,
	struct v90_phase3_analog_station *station);

enum pcm_phase3_entry_result pcm_phase3_entry_start_digital(
	const struct pcm_phase3_entry *entry,
	const struct v90_phase3_analog_rx_configuration *receive_configuration,
	const struct v90_jd_parameters *jd,
	struct v90_phase3_digital_station *station);

const char *pcm_phase3_entry_result_name(
	enum pcm_phase3_entry_result result);

#endif /* PCM_PHASE3_ENTRY_H */
