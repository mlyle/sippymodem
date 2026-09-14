// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_phase3_entry.h"

#include "v92_phase3_analog_station.h"
#include "v92_phase3_digital_station.h"

#include <math.h>
#include <stddef.h>

enum pcm_phase3_entry_result pcm_phase3_entry_start_v92_analog(
	const struct pcm_phase3_entry *entry,
	const struct v92_phase3_analog_station_configuration *configuration,
	struct v92_phase3_analog_station *station)
{
	struct v92_phase3_analog_station_configuration selected;
	enum v92_phase3_analog_station_result result;
	int16_t nominal, level;
	double limit;
	uint8_t power_reduction;

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
	if (!v92_phase3_analog_station_configuration_is_valid(configuration) ||
	    !g711_ucode_sample(configuration->law, configuration->upstream_lu_ucode,
		true, &nominal))
		return PCM_PHASE3_ENTRY_INVALID_ARGUMENT;
	/* V.92 3.8 and Table 17: Lu follows the requested analogue transmit
	 * power reduction, not downstream UINFO. Additional reduction is only
	 * permission. Choose the highest representable level within the bound;
	 * retain the nominal proposal so retrains do not compound reductions. */
	selected = *configuration;
	power_reduction = entry->selection.startup.phase2 == PCM_PHASE2_SHORT ?
		entry->short_power_reduction_db :
		entry->selection.digital.record.v92.v34.common.minimum_peer_power_reduction_db;
	limit = nominal * pow(10.0, -(double)power_reduction / 20.0);
	for (;;) {
		if (!g711_ucode_sample(selected.law, selected.upstream_lu_ucode, true, &level))
			return PCM_PHASE3_ENTRY_INTERNAL_ERROR;
		if (level <= limit)
			break;
		if (--selected.upstream_lu_ucode == 0u)
			return PCM_PHASE3_ENTRY_UNUSABLE_SELECTION;
	}
	result = v92_phase3_analog_station_init(station, &selected);
	if (result == V92_PHASE3_ANALOG_STATION_OK)
		return PCM_PHASE3_ENTRY_OK;
	return result == V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT ?
		       PCM_PHASE3_ENTRY_INVALID_ARGUMENT :
		       PCM_PHASE3_ENTRY_INTERNAL_ERROR;
}

enum pcm_phase3_entry_result pcm_phase3_entry_start_v92_digital(
	const struct pcm_phase3_entry *entry, uint32_t trn1d_symbols,
	double equalizer_adaptation,
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
		trn1d_symbols, equalizer_adaptation, su_minimum_magnitude, jd, jp);
	if (result == V92_PHASE3_DIGITAL_STATION_OK)
		return PCM_PHASE3_ENTRY_OK;
	return result == V92_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT ?
		       PCM_PHASE3_ENTRY_INVALID_ARGUMENT :
		       PCM_PHASE3_ENTRY_INTERNAL_ERROR;
}
