// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE3_ENTRY_H
#define V90_PHASE3_ENTRY_H

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

enum v90_phase3_entry_result {
	V90_PHASE3_ENTRY_OK,
	V90_PHASE3_ENTRY_INVALID_ARGUMENT,
	V90_PHASE3_ENTRY_INVALID_SELECTION,
	V90_PHASE3_ENTRY_INVALID_STATE,
	V90_PHASE3_ENTRY_UNUSABLE_SELECTION,
	V90_PHASE3_ENTRY_ROLE_MISMATCH,
	V90_PHASE3_ENTRY_MEDIA_MISMATCH,
	V90_PHASE3_ENTRY_OUT_OF_RANGE,
	V90_PHASE3_ENTRY_CLIPPING_RISK,
	V90_PHASE3_ENTRY_INTERNAL_ERROR,
};

/* Immutable inputs shared by the two role-specific zero-DIL Phase 3 owners.
 * The line profile describes the analogue modem's conventional upstream in
 * both roles. V.90 Phase 3 starts at S rather than MD, so the retained profile
 * is deliberately canonicalized to an empty MD interval. */
struct v90_phase3_entry {
	struct v34_phase3_profile analog_profile;
	enum g711_law law;
	enum v90_modem_role local_role;
	uint8_t training_ucode;
};

bool v90_phase3_entry_is_valid(
	const struct v90_phase3_entry *entry);

/* Resolve a completed full V.90 INFO exchange against the active SIP media
 * law. digital_info0 is the retained record sent by the digital modem,
 * regardless of which endpoint is local. Rejected calls leave entry
 * unchanged. */
enum v90_phase3_entry_result v90_phase3_entry_init(
	struct v90_phase3_entry *entry,
	enum v90_modem_role local_role, enum g711_law media_law,
	const struct v90_info0d *digital_info0,
	const struct pcm_info1_selection *selection);

/* Initialize exactly the station selected by entry.local_role. Rejected
 * calls leave the station unchanged. */
enum v90_phase3_entry_result v90_phase3_entry_start_analog(
	const struct v90_phase3_entry *entry,
	int16_t nominal_peak_amplitude,
	struct v90_phase3_analog_station *station);

enum v90_phase3_entry_result v90_phase3_entry_start_digital(
	const struct v90_phase3_entry *entry,
	const struct v90_phase3_analog_rx_configuration *receive_configuration,
	const struct v90_jd_parameters *jd,
	struct v90_phase3_digital_station *station);

const char *v90_phase3_entry_result_name(
	enum v90_phase3_entry_result result);

#endif /* V90_PHASE3_ENTRY_H */
