// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_phase3_entry.h"

#include "v90_phase3_analog_station.h"
#include "v90_phase3_digital_station.h"

#include <stddef.h>

static enum g711_law selected_law(
	const struct v90_info0d *digital_info0)
{
	return digital_info0->pcm_law == V90_PCM_A_LAW ?
		       G711_LAW_PCMA : G711_LAW_PCMU;
}

bool pcm_phase3_entry_is_valid(
	const struct pcm_phase3_entry *entry)
{
	if (entry == NULL ||
	    !pcm_modem_role_is_valid(entry->local_role) ||
	    !g711_law_is_valid(entry->law) ||
	    !pcm_info1_selection_is_valid(&entry->selection) ||
	    entry->selection.startup.phase2 != PCM_PHASE2_FULL)
		return false;
	if (entry->selection.upstream == PCM_UPSTREAM_PCM)
		return entry->selection.startup.protocol == PCM_PROTOCOL_V92;
	return v34_phase3_profile_is_valid(
		       &entry->conventional_profile) &&
	       entry->conventional_profile.local_role ==
		       V34_ANSWERING_STATION &&
	       entry->conventional_profile.md_length_35_ms_units == 0 &&
	       entry->conventional_profile.md_symbol_count == 0;
}

enum pcm_phase3_entry_result pcm_phase3_entry_init(
	struct pcm_phase3_entry *entry,
	enum v90_modem_role local_role, enum g711_law media_law,
	const struct v90_info0d *digital_info0,
	const struct pcm_info1_selection *selection)
{
	struct pcm_phase3_entry initialized = {
		.law = media_law,
		.local_role = local_role,
	};
	enum v34_phase3_profile_result profile_result;

	if (entry == NULL || !pcm_modem_role_is_valid(local_role) ||
	    !g711_law_is_valid(media_law) || !v90_info0d_is_valid(digital_info0) ||
	    !pcm_info1_selection_is_valid(selection))
		return PCM_PHASE3_ENTRY_INVALID_ARGUMENT;
	if (selection->startup.phase2 != PCM_PHASE2_FULL)
		return PCM_PHASE3_ENTRY_INVALID_SELECTION;
	if (selected_law(digital_info0) != media_law)
		return PCM_PHASE3_ENTRY_MEDIA_MISMATCH;
	initialized.selection = *selection;
	if (selection->upstream == PCM_UPSTREAM_V34) {
		profile_result = pcm_phase3_conventional_profile(
			selection, &initialized.conventional_profile);
		if (profile_result == V34_PHASE3_PROFILE_UNUSABLE_SELECTION)
			return PCM_PHASE3_ENTRY_UNUSABLE_SELECTION;
		if (profile_result != V34_PHASE3_PROFILE_OK)
			return PCM_PHASE3_ENTRY_INVALID_SELECTION;

		/* INFO1a retains the common MD-length field, but the selected PCM
		 * conventional procedure begins its upstream at S. */
		initialized.conventional_profile.md_length_35_ms_units = 0;
		initialized.conventional_profile.md_symbol_count = 0;
	}
	if (!pcm_phase3_entry_is_valid(&initialized))
		return PCM_PHASE3_ENTRY_INTERNAL_ERROR;
	*entry = initialized;
	return PCM_PHASE3_ENTRY_OK;
}

enum pcm_phase3_entry_result pcm_phase3_entry_start_v90_analog(
	const struct pcm_phase3_entry *entry,
	int16_t nominal_peak_amplitude,
	struct v90_phase3_analog_station *station)
{
	enum v90_phase3_analog_station_result result;

	if (!pcm_phase3_entry_is_valid(entry) || station == NULL ||
	    nominal_peak_amplitude <= 0)
		return PCM_PHASE3_ENTRY_INVALID_ARGUMENT;
	if (entry->local_role != V90_ANALOG_MODEM)
		return PCM_PHASE3_ENTRY_ROLE_MISMATCH;
	if (entry->selection.startup.protocol != PCM_PROTOCOL_V90 ||
	    entry->selection.upstream != PCM_UPSTREAM_V34)
		return PCM_PHASE3_ENTRY_INVALID_SELECTION;
	result = v90_phase3_analog_station_init(
		station, &entry->conventional_profile, entry->law,
		entry->selection.analog.conventional.training_ucode,
		nominal_peak_amplitude);
	switch (result) {
	case V90_PHASE3_ANALOG_STATION_OK:
		return PCM_PHASE3_ENTRY_OK;
	case V90_PHASE3_ANALOG_STATION_OUT_OF_RANGE:
		return PCM_PHASE3_ENTRY_OUT_OF_RANGE;
	case V90_PHASE3_ANALOG_STATION_CLIPPING_RISK:
		return PCM_PHASE3_ENTRY_CLIPPING_RISK;
	case V90_PHASE3_ANALOG_STATION_COMPLETE:
	case V90_PHASE3_ANALOG_STATION_FAILED:
	case V90_PHASE3_ANALOG_STATION_INVALID_ARGUMENT:
	case V90_PHASE3_ANALOG_STATION_INVALID_STATE:
	case V90_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH:
	case V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE:
		break;
	}
	return PCM_PHASE3_ENTRY_INTERNAL_ERROR;
}

enum pcm_phase3_entry_result pcm_phase3_entry_start_v90_digital(
	const struct pcm_phase3_entry *entry,
	const struct v90_phase3_analog_rx_configuration *receive_configuration,
	const struct v90_jd_parameters *jd,
	struct v90_phase3_digital_station *station)
{
	enum v90_phase3_digital_station_result result;

	if (!pcm_phase3_entry_is_valid(entry) || station == NULL ||
	    !v90_phase3_analog_rx_configuration_is_valid(
		    receive_configuration) ||
	    !v90_jd_parameters_are_valid(jd))
		return PCM_PHASE3_ENTRY_INVALID_ARGUMENT;
	if (entry->local_role != V90_DIGITAL_MODEM)
		return PCM_PHASE3_ENTRY_ROLE_MISMATCH;
	if (entry->selection.startup.protocol != PCM_PROTOCOL_V90 ||
	    entry->selection.upstream != PCM_UPSTREAM_V34)
		return PCM_PHASE3_ENTRY_INVALID_SELECTION;
	result = v90_phase3_digital_station_init(
		station, &entry->conventional_profile, receive_configuration,
		entry->law,
		entry->selection.analog.conventional.training_ucode, jd);
	switch (result) {
	case V90_PHASE3_DIGITAL_STATION_OK:
		return PCM_PHASE3_ENTRY_OK;
	case V90_PHASE3_DIGITAL_STATION_OUT_OF_RANGE:
		return PCM_PHASE3_ENTRY_OUT_OF_RANGE;
	case V90_PHASE3_DIGITAL_STATION_COMPLETE:
	case V90_PHASE3_DIGITAL_STATION_FAILED:
	case V90_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT:
	case V90_PHASE3_DIGITAL_STATION_INVALID_STATE:
	case V90_PHASE3_DIGITAL_STATION_SEQUENCE_MISMATCH:
	case V90_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE:
		break;
	}
	return PCM_PHASE3_ENTRY_INTERNAL_ERROR;
}

const char *pcm_phase3_entry_result_name(
	enum pcm_phase3_entry_result result)
{
	switch (result) {
	case PCM_PHASE3_ENTRY_OK:
		return "ok";
	case PCM_PHASE3_ENTRY_INVALID_ARGUMENT:
		return "invalid argument";
	case PCM_PHASE3_ENTRY_INVALID_SELECTION:
		return "invalid selection";
	case PCM_PHASE3_ENTRY_INVALID_STATE:
		return "invalid state";
	case PCM_PHASE3_ENTRY_UNUSABLE_SELECTION:
		return "unusable selection";
	case PCM_PHASE3_ENTRY_ROLE_MISMATCH:
		return "role mismatch";
	case PCM_PHASE3_ENTRY_MEDIA_MISMATCH:
		return "media mismatch";
	case PCM_PHASE3_ENTRY_OUT_OF_RANGE:
		return "out of range";
	case PCM_PHASE3_ENTRY_CLIPPING_RISK:
		return "clipping risk";
	case PCM_PHASE3_ENTRY_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
