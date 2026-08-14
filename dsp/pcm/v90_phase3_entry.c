// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase3_entry.h"

#include "v90_info1.h"
#include "v90_phase3_analog_station.h"
#include "v90_phase3_digital_station.h"

#include <stddef.h>

static bool role_is_valid(enum v90_modem_role role)
{
	return role == V90_ANALOG_MODEM || role == V90_DIGITAL_MODEM;
}

static enum g711_law selected_law(
	const struct v90_info0d *digital_info0)
{
	return digital_info0->pcm_law == V90_PCM_A_LAW ?
		       G711_LAW_PCMA : G711_LAW_PCMU;
}

bool v90_phase3_entry_is_valid(
	const struct v90_phase3_entry *entry)
{
	return entry != NULL && role_is_valid(entry->local_role) &&
	       g711_law_is_valid(entry->law) &&
	       entry->training_ucode >= V90_INFO1A_MIN_TRAINING_UCODE &&
	       entry->training_ucode <= V90_INFO1A_MAX_TRAINING_UCODE &&
	       v34_phase3_profile_is_valid(&entry->analog_profile) &&
	       entry->analog_profile.local_role == V34_ANSWERING_STATION &&
	       entry->analog_profile.md_length_35_ms_units == 0 &&
	       entry->analog_profile.md_symbol_count == 0;
}

enum v90_phase3_entry_result v90_phase3_entry_init(
	struct v90_phase3_entry *entry,
	enum v90_modem_role local_role, enum g711_law media_law,
	const struct v90_info0d *digital_info0,
	const struct pcm_info1_selection *selection)
{
	struct v90_phase3_entry initialized = {
		.law = media_law,
		.local_role = local_role,
	};
	enum v34_phase3_profile_result profile_result;

	if (entry == NULL || !role_is_valid(local_role) ||
	    !g711_law_is_valid(media_law) || !v90_info0d_is_valid(digital_info0) ||
	    !pcm_info1_selection_is_valid(selection))
		return V90_PHASE3_ENTRY_INVALID_ARGUMENT;
	if (selection->startup.protocol != PCM_PROTOCOL_V90 ||
	    selection->startup.phase2 != PCM_PHASE2_FULL ||
	    selection->upstream != PCM_UPSTREAM_V34)
		return V90_PHASE3_ENTRY_INVALID_SELECTION;
	if (selected_law(digital_info0) != media_law)
		return V90_PHASE3_ENTRY_MEDIA_MISMATCH;
	profile_result = pcm_phase3_conventional_profile(
		selection, &initialized.analog_profile);
	if (profile_result == V34_PHASE3_PROFILE_UNUSABLE_SELECTION)
		return V90_PHASE3_ENTRY_UNUSABLE_SELECTION;
	if (profile_result != V34_PHASE3_PROFILE_OK)
		return V90_PHASE3_ENTRY_INVALID_SELECTION;

	/* INFO1a retains the common MD-length field, but the selected V.90
	 * procedure begins its conventional upstream at S. */
	initialized.analog_profile.md_length_35_ms_units = 0;
	initialized.analog_profile.md_symbol_count = 0;
	initialized.training_ucode =
		selection->analog.conventional.training_ucode;
	if (!v90_phase3_entry_is_valid(&initialized))
		return V90_PHASE3_ENTRY_INTERNAL_ERROR;
	*entry = initialized;
	return V90_PHASE3_ENTRY_OK;
}

enum v90_phase3_entry_result v90_phase3_entry_start_analog(
	const struct v90_phase3_entry *entry,
	int16_t nominal_peak_amplitude,
	struct v90_phase3_analog_station *station)
{
	enum v90_phase3_analog_station_result result;

	if (!v90_phase3_entry_is_valid(entry) || station == NULL ||
	    nominal_peak_amplitude <= 0)
		return V90_PHASE3_ENTRY_INVALID_ARGUMENT;
	if (entry->local_role != V90_ANALOG_MODEM)
		return V90_PHASE3_ENTRY_ROLE_MISMATCH;
	result = v90_phase3_analog_station_init(
		station, &entry->analog_profile, entry->law,
		entry->training_ucode, nominal_peak_amplitude);
	switch (result) {
	case V90_PHASE3_ANALOG_STATION_OK:
		return V90_PHASE3_ENTRY_OK;
	case V90_PHASE3_ANALOG_STATION_OUT_OF_RANGE:
		return V90_PHASE3_ENTRY_OUT_OF_RANGE;
	case V90_PHASE3_ANALOG_STATION_CLIPPING_RISK:
		return V90_PHASE3_ENTRY_CLIPPING_RISK;
	case V90_PHASE3_ANALOG_STATION_COMPLETE:
	case V90_PHASE3_ANALOG_STATION_FAILED:
	case V90_PHASE3_ANALOG_STATION_INVALID_ARGUMENT:
	case V90_PHASE3_ANALOG_STATION_INVALID_STATE:
	case V90_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH:
	case V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE:
		break;
	}
	return V90_PHASE3_ENTRY_INTERNAL_ERROR;
}

enum v90_phase3_entry_result v90_phase3_entry_start_digital(
	const struct v90_phase3_entry *entry,
	const struct v90_phase3_analog_rx_configuration *receive_configuration,
	const struct v90_jd_parameters *jd,
	struct v90_phase3_digital_station *station)
{
	enum v90_phase3_digital_station_result result;

	if (!v90_phase3_entry_is_valid(entry) || station == NULL ||
	    !v90_phase3_analog_rx_configuration_is_valid(
		    receive_configuration) ||
	    !v90_jd_parameters_are_valid(jd))
		return V90_PHASE3_ENTRY_INVALID_ARGUMENT;
	if (entry->local_role != V90_DIGITAL_MODEM)
		return V90_PHASE3_ENTRY_ROLE_MISMATCH;
	result = v90_phase3_digital_station_init(
		station, &entry->analog_profile, receive_configuration,
		entry->law, entry->training_ucode, jd);
	switch (result) {
	case V90_PHASE3_DIGITAL_STATION_OK:
		return V90_PHASE3_ENTRY_OK;
	case V90_PHASE3_DIGITAL_STATION_OUT_OF_RANGE:
		return V90_PHASE3_ENTRY_OUT_OF_RANGE;
	case V90_PHASE3_DIGITAL_STATION_COMPLETE:
	case V90_PHASE3_DIGITAL_STATION_FAILED:
	case V90_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT:
	case V90_PHASE3_DIGITAL_STATION_INVALID_STATE:
	case V90_PHASE3_DIGITAL_STATION_SEQUENCE_MISMATCH:
	case V90_PHASE3_DIGITAL_STATION_UNSUPPORTED:
	case V90_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE:
		break;
	}
	return V90_PHASE3_ENTRY_INTERNAL_ERROR;
}

const char *v90_phase3_entry_result_name(
	enum v90_phase3_entry_result result)
{
	switch (result) {
	case V90_PHASE3_ENTRY_OK:
		return "ok";
	case V90_PHASE3_ENTRY_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PHASE3_ENTRY_INVALID_SELECTION:
		return "invalid selection";
	case V90_PHASE3_ENTRY_INVALID_STATE:
		return "invalid state";
	case V90_PHASE3_ENTRY_UNUSABLE_SELECTION:
		return "unusable selection";
	case V90_PHASE3_ENTRY_ROLE_MISMATCH:
		return "role mismatch";
	case V90_PHASE3_ENTRY_MEDIA_MISMATCH:
		return "media mismatch";
	case V90_PHASE3_ENTRY_OUT_OF_RANGE:
		return "out of range";
	case V90_PHASE3_ENTRY_CLIPPING_RISK:
		return "clipping risk";
	case V90_PHASE3_ENTRY_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
