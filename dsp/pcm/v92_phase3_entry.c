// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_phase3_entry.h"

#include "v92_phase3_analog_station.h"
#include "v92_phase3_digital_station.h"

#include <stddef.h>

enum pcm_phase3_entry_result pcm_phase3_entry_start_v92_analog(
	const struct pcm_phase3_entry *entry,
	const struct v92_phase3_analog_station_configuration *configuration,
	struct v92_phase3_analog_station *station)
{
	enum v92_phase3_analog_station_result result;

	if (!pcm_phase3_entry_is_valid(entry) || configuration == NULL ||
	    station == NULL)
		return PCM_PHASE3_ENTRY_INVALID_ARGUMENT;
	if (entry->local_role != V90_ANALOG_MODEM)
		return PCM_PHASE3_ENTRY_ROLE_MISMATCH;
	if (entry->selection.startup.protocol != PCM_PROTOCOL_V92 ||
	    entry->selection.upstream != PCM_UPSTREAM_PCM)
		return PCM_PHASE3_ENTRY_INVALID_SELECTION;
	if (configuration->law != entry->law ||
	    !v92_pcm_info1a_equal(
		    &configuration->local_info1a,
		    &entry->selection.analog.pcm))
		return PCM_PHASE3_ENTRY_INVALID_SELECTION;
	result = v92_phase3_analog_station_init(station, configuration);
	if (result == V92_PHASE3_ANALOG_STATION_OK)
		return PCM_PHASE3_ENTRY_OK;
	return result == V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT ?
		       PCM_PHASE3_ENTRY_INVALID_ARGUMENT :
		       PCM_PHASE3_ENTRY_INTERNAL_ERROR;
}

enum pcm_phase3_entry_result pcm_phase3_entry_start_v92_digital(
	const struct pcm_phase3_entry *entry, double equalizer_adaptation,
	uint16_t su_minimum_magnitude, const struct v92_jd *jd,
	const struct v92_jp *jp,
	struct v92_phase3_digital_station *station)
{
	enum v92_phase3_digital_station_result result;

	if (!pcm_phase3_entry_is_valid(entry) || station == NULL ||
	    !v92_jd_is_valid(jd) || !v92_jp_is_valid(jp))
		return PCM_PHASE3_ENTRY_INVALID_ARGUMENT;
	if (entry->local_role != V90_DIGITAL_MODEM)
		return PCM_PHASE3_ENTRY_ROLE_MISMATCH;
	if (entry->selection.startup.protocol != PCM_PROTOCOL_V92 ||
	    entry->selection.upstream != PCM_UPSTREAM_PCM)
		return PCM_PHASE3_ENTRY_INVALID_SELECTION;
	result = v92_phase3_digital_station_init(
		station, entry->law,
		entry->selection.analog.pcm.training_ucode,
		equalizer_adaptation, su_minimum_magnitude, jd, jp);
	if (result == V92_PHASE3_DIGITAL_STATION_OK)
		return PCM_PHASE3_ENTRY_OK;
	return result == V92_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT ?
		       PCM_PHASE3_ENTRY_INVALID_ARGUMENT :
		       PCM_PHASE3_ENTRY_INTERNAL_ERROR;
}
