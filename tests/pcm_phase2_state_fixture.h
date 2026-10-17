// SPDX-License-Identifier: BSD-3-Clause
#ifndef PCM_PHASE2_STATE_FIXTURE_H
#define PCM_PHASE2_STATE_FIXTURE_H

#include <pcm_phase2.h>
#include <pcm_phase3_entry.h>
#include "v34_phase2_state_fixture.h"

/* Copy representations rather than validate records: rollback checks also
 * compare deliberately invalid fields and reserved clock-source values. */
#define COPY(to, from, field) memcpy(&(to)->field, &(from)->field, sizeof((to)->field))

static inline void pcm_snapshot_info0(union pcm_info0_record *record,
	enum pcm_protocol protocol, enum v90_modem_role role)
{
	union pcm_info0_record copy = { 0 };
	struct v34_info0 *common;

	if (!pcm_protocol_is_valid(protocol) || !pcm_modem_role_is_valid(role))
		return;
	common = pcm_info0_mutable_common(&copy, protocol, role);
	memcpy(common, pcm_info0_common(record, protocol, role), sizeof(*common));
	v34_snapshot_info0(common);
	if (role == V90_DIGITAL_MODEM) {
		struct v90_info0d *to = protocol == PCM_PROTOCOL_V90 ?
			&copy.digital : &copy.v92_digital.v90;
		const struct v90_info0d *from = protocol == PCM_PROTOCOL_V90 ?
			&record->digital : &record->v92_digital.v90;

		COPY(to, from, nominal_transmit_power);
		COPY(to, from, maximum_transmit_power);
		COPY(to, from, measure_power_at_codec_output);
		COPY(to, from, pcm_law);
		COPY(to, from, upstream_symbol_rate_3429_supported);
	}
	if (protocol == PCM_PROTOCOL_V92) {
		if (role == V90_ANALOG_MODEM) {
			COPY(&copy.v92_analog, &record->v92_analog, v92_capable);
			COPY(&copy.v92_analog, &record->v92_analog, short_phase2_requested);
		} else {
			COPY(&copy.v92_digital, &record->v92_digital, v92_capable);
			COPY(&copy.v92_digital, &record->v92_digital, short_phase2_requested);
		}
	}
	memcpy(record, &copy, sizeof(copy));
}

static inline void pcm_snapshot_info0_exchange(
	struct pcm_phase2_info0_exchange *exchange)
{
	pcm_snapshot_info0(&exchange->peer_info0, exchange->protocol,
		exchange->local_role == V90_ANALOG_MODEM ? V90_DIGITAL_MODEM : V90_ANALOG_MODEM);
}

static inline void v34_copy_info1_common(struct v34_info1_common *to,
	const struct v34_info1_common *from)
{
	COPY(to, from, minimum_peer_power_reduction_db);
	COPY(to, from, additional_peer_power_reduction_db);
	COPY(to, from, local_md_length_35_ms_units);
	COPY(to, from, frequency_offset_1_50_hz_units);
}

static inline void v34_copy_info1_probe(struct v34_info1_probe_result *to,
	const struct v34_info1_probe_result *from)
{
	COPY(to, from, high_carrier);
	COPY(to, from, preemphasis_filter_index);
	COPY(to, from, projected_data_rate_2400_bps_units);
}

static inline void pcm_snapshot_info1d(struct pcm_info1d *digital)
{
	union pcm_info1d_record copy = { 0 };
	struct v34_info1c *to;
	const struct v34_info1c *from;

	if (!pcm_protocol_is_valid(digital->protocol))
		return;
	to = digital->protocol == PCM_PROTOCOL_V90 ? &copy.v90 : &copy.v92.v34;
	from = digital->protocol == PCM_PROTOCOL_V90 ?
		&digital->record.v90 : &digital->record.v92.v34;
	v34_copy_info1_common(&to->common, &from->common);
	for (size_t rate = 0; rate < V34_SYMBOL_RATE_COUNT; ++rate)
		v34_copy_info1_probe(&to->answer_transmit[rate], &from->answer_transmit[rate]);
	if (digital->protocol == PCM_PROTOCOL_V92)
		COPY(&copy.v92, &digital->record.v92, pcm_upstream_supported);
	memcpy(&digital->record, &copy, sizeof(copy));
}

static inline void pcm_snapshot_info1a(union pcm_info1a_record *record,
	enum pcm_info1a_kind kind)
{
	union pcm_info1a_record copy = { 0 };

	switch (kind) {
	case PCM_INFO1A_CONVENTIONAL:
		COPY(&copy.conventional, &record->conventional, local_md_length_35_ms_units);
		COPY(&copy.conventional, &record->conventional, training_ucode);
		COPY(&copy.conventional, &record->conventional, upstream_symbol_rate);
		COPY(&copy.conventional, &record->conventional, frequency_offset_1_50_hz_units);
		break;
	case PCM_INFO1A_PCM:
		COPY(&copy.pcm, &record->pcm, filter_sections);
		COPY(&copy.pcm, &record->pcm, maximum_total_coefficients);
		COPY(&copy.pcm, &record->pcm, maximum_section_coefficients);
		COPY(&copy.pcm, &record->pcm, local_md_length_276_symbol_units);
		COPY(&copy.pcm, &record->pcm, training_ucode);
		break;
	case PCM_INFO1A_V34_FALLBACK:
		v34_copy_info1_common(&copy.v34_fallback.common, &record->v34_fallback.common);
		v34_copy_info1_probe(&copy.v34_fallback.call_transmit, &record->v34_fallback.call_transmit);
		COPY(&copy.v34_fallback, &record->v34_fallback, answer_to_call_symbol_rate);
		COPY(&copy.v34_fallback, &record->v34_fallback, call_to_answer_symbol_rate);
		break;
	default:
		return;
	}
	memcpy(record, &copy, sizeof(copy));
}
#undef COPY

static inline void pcm_snapshot_info1_exchange(
	struct pcm_phase2_info1_exchange *exchange)
{
	/* Unstored records remain part of the initialized owner's rollback check. */
	if (exchange->digital_record_stored)
		pcm_snapshot_info1d(&exchange->digital);
	if (exchange->analog_record_stored)
		pcm_snapshot_info1a(&exchange->analog.record, exchange->analog.kind);
}

static inline void pcm_snapshot_phase2(struct pcm_phase2 *phase2)
{
	pcm_snapshot_info0(&phase2->local_info0, phase2->info0_protocol, phase2->local_role);
	pcm_snapshot_info0(&phase2->peer_info0, phase2->info0_protocol,
		phase2->local_role == V90_ANALOG_MODEM ? V90_DIGITAL_MODEM : V90_ANALOG_MODEM);
	switch (phase2->stage) {
	case PCM_PHASE2_RANGING:
		pcm_snapshot_info0_exchange(&phase2->state.ranging.info0_exchange);
		break;
	case PCM_PHASE2_INITIAL_PROBE:
		pcm_snapshot_info0_exchange(&phase2->state.initial_probe.ranging.info0_exchange);
		break;
	case PCM_PHASE2_PROBE_TURNAROUND:
		pcm_snapshot_info0_exchange(&phase2->state.turnaround.initial_probe.ranging.info0_exchange);
		break;
	case PCM_PHASE2_RECIPROCAL_PROBE:
		pcm_snapshot_info0_exchange(&phase2->state.reciprocal_probe.turnaround.initial_probe.ranging.info0_exchange);
		break;
	case PCM_PHASE2_INFO1:
		pcm_snapshot_info1_exchange(&phase2->state.info1);
		if (phase2->state.info1.control.short_exchange &&
		    phase2->local_role == V90_DIGITAL_MODEM)
			pcm_snapshot_info0_exchange(&phase2->state.short_recovery.info0_exchange);
		break;
	}
}

#define STATE_EQUAL(name, type, prepare) \
static inline bool name(const struct type *left, const struct type *right) \
{ \
	struct type snapshots[2]; \
	memcpy(&snapshots[0], left, sizeof(*left)); \
	memcpy(&snapshots[1], right, sizeof(*right)); \
	for (size_t index = 0; index < 2; ++index) { \
		struct type *copy = &snapshots[index]; \
		prepare; \
	} \
	return memcmp(&snapshots[0], &snapshots[1], sizeof(*left)) == 0; \
}
STATE_EQUAL(pcm_phase2_owners_equal, pcm_phase2, pcm_snapshot_phase2(copy))
STATE_EQUAL(pcm_info1_exchanges_equal, pcm_phase2_info1_exchange,
	pcm_snapshot_info1_exchange(copy))
STATE_EQUAL(v90_initial_probes_equal, v90_phase2_initial_probe,
	pcm_snapshot_info0_exchange(&copy->ranging.info0_exchange))
STATE_EQUAL(v90_probe_turnarounds_equal, v90_phase2_probe_turnaround,
	pcm_snapshot_info0_exchange(&copy->initial_probe.ranging.info0_exchange))
STATE_EQUAL(v90_reciprocal_probes_equal, v90_phase2_reciprocal_probe,
	pcm_snapshot_info0_exchange(&copy->turnaround.initial_probe.ranging.info0_exchange))
#undef STATE_EQUAL

static inline bool pcm_phase3_entries_equal(
	const struct pcm_phase3_entry *left, const struct pcm_phase3_entry *right)
{
	struct pcm_phase3_entry snapshots[2];

	if (!v34_phase3_profile_is_equal(&left->conventional_profile, &right->conventional_profile))
		return false;
	memcpy(&snapshots[0], left, sizeof(*left));
	memcpy(&snapshots[1], right, sizeof(*right));
	for (size_t index = 0; index < 2; ++index) {
		struct pcm_phase3_entry *copy = &snapshots[index];
		struct pcm_info1_selection *selection = &copy->selection;

		memset(&copy->conventional_profile, 0, sizeof(copy->conventional_profile));
		if (selection->startup.phase2 == PCM_PHASE2_FULL)
			pcm_snapshot_info1d(&selection->digital);
		else if (selection->startup.phase2 == PCM_PHASE2_SHORT) {
			unsigned char carrier[sizeof(selection->digital)] = { 0 };

			memcpy(carrier, &selection->short_high_carrier, sizeof(selection->short_high_carrier));
			memcpy(&selection->digital, carrier, sizeof(carrier));
		}
		if (selection->upstream == PCM_UPSTREAM_PCM || selection->upstream == PCM_UPSTREAM_V34)
			pcm_snapshot_info1a(&selection->analog, selection->upstream == PCM_UPSTREAM_PCM ?
				PCM_INFO1A_PCM : PCM_INFO1A_CONVENTIONAL);
	}
	return memcmp(&snapshots[0], &snapshots[1], sizeof(*left)) == 0;
}

#endif
