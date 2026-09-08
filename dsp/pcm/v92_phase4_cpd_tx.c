// SPDX-License-Identifier: BSD-3-Clause

#include "v92_phase4_cpd_tx.h"

#include "memory_region.h"
#include "v92_pcm_mapping.h"

enum {
	FINGERPRINT_OFFSET = UINT32_C(2166136261),
	FINGERPRINT_PRIME = UINT32_C(16777619),
};

static uint32_t fingerprint(const uint8_t *bytes, size_t count)
{
	uint32_t value = FINGERPRINT_OFFSET;

	for (size_t index = 0u; index < count; ++index) {
		value ^= bytes[index];
		value *= FINGERPRINT_PRIME;
	}
	return value;
}

bool v92_phase4_cpd_tx_is_valid(
	const struct v92_phase4_cpd_tx *transmitter)
{
	size_t sequence_bytes;

	if (transmitter == NULL || !g711_law_is_valid(transmitter->law) ||
	    transmitter->frame_bits < V92_CPD_DOWNSTREAM_FRAME_MIN_BITS ||
	    transmitter->frame_bits > V92_CPD_DOWNSTREAM_FRAME_MAX_BITS ||
	    transmitter->sequence_bits == 0u ||
	    transmitter->sequence_bits > V92_CPD_MAX_FRAME_BITS ||
	    transmitter->sequence_bits % transmitter->frame_bits != 0u ||
	    (transmitter->complete ?
		     transmitter->bit_index != transmitter->sequence_bits :
		     transmitter->bit_index >= transmitter->sequence_bits ||
			     transmitter->bit_index %
				     transmitter->frame_bits != 0u))
		return false;
	sequence_bytes = (transmitter->sequence_bits + 7u) / 8u;
	return fingerprint(transmitter->sequence, sequence_bytes) ==
	       transmitter->sequence_fingerprint;
}

bool v92_phase4_cpd_tx_init(
	struct v92_phase4_cpd_tx *transmitter,
	const struct pcm_downstream_tx *downstream,
	const struct v92_cpd_parameters *parameters)
{
	struct v92_phase4_cpd_tx initialized = { 0 };
	struct v92_pcm_mapping mapping;
	size_t sequence_bytes;

	if (transmitter == NULL || downstream == NULL || parameters == NULL ||
	    memory_regions_overlap(
		    transmitter, sizeof(*transmitter), downstream,
		    sizeof(*downstream)) ||
	    memory_regions_overlap(
		    transmitter, sizeof(*transmitter), parameters,
		    sizeof(*parameters)))
		return false;
	if (!pcm_downstream_tx_is_valid(downstream) ||
	    downstream->protocol != PCM_PROTOCOL_V92 ||
	    downstream->encoder.mapping.kind != V90_CP_TRAINING)
		return false;
	/* Initial negotiation must establish a complete usable mapping. Compact
	 * records can acknowledge an existing mapping, but cannot create one. */
	if (v92_pcm_mapping_init(&mapping, downstream->encoder.mapping.law,
				 parameters) != V92_PCM_MAPPING_OK)
		return false;
	if (v92_cpd_encode(
		    parameters, downstream->encoder.mapping.frame_bits,
		    initialized.sequence, sizeof(initialized.sequence),
		    &initialized.sequence_bits) != V92_CPD_OK)
		return false;
	initialized.frame_bits = downstream->encoder.mapping.frame_bits;
	initialized.law = downstream->encoder.mapping.law;
	sequence_bytes = (initialized.sequence_bits + 7u) / 8u;
	initialized.sequence_fingerprint =
		fingerprint(initialized.sequence, sequence_bytes);
	if (!v92_phase4_cpd_tx_is_valid(&initialized))
		return false;
	*transmitter = initialized;
	return true;
}

bool v92_phase4_cpd_tx_next(
	struct v92_phase4_cpd_tx *transmitter,
	struct pcm_downstream_tx *downstream,
	struct pcm_downstream_symbol *symbol)
{
	struct v92_phase4_cpd_tx next;
	struct pcm_downstream_tx next_downstream;
	struct pcm_downstream_source_record record;
	struct pcm_downstream_symbol generated;

	if (transmitter == NULL || downstream == NULL || symbol == NULL ||
	    memory_regions_overlap(
		    transmitter, sizeof(*transmitter), downstream,
		    sizeof(*downstream)) ||
	    memory_regions_overlap(
		    transmitter, sizeof(*transmitter), symbol,
		    sizeof(*symbol)) ||
	    memory_regions_overlap(
		    downstream, sizeof(*downstream), symbol, sizeof(*symbol)) ||
	    !v92_phase4_cpd_tx_is_valid(transmitter) ||
	    transmitter->complete ||
	    !pcm_downstream_tx_is_valid(downstream) ||
	    downstream->encoder.mapping.law != transmitter->law ||
	    downstream->encoder.mapping.frame_bits != transmitter->frame_bits)
		return false;
	next = *transmitter;
	next_downstream = *downstream;
	record = (struct pcm_downstream_source_record){
		.sequence = next.sequence,
		.sequence_capacity = sizeof(next.sequence),
		.sequence_bits = next.sequence_bits,
		.bit_index = next.bit_index,
	};
	if (pcm_downstream_tx_next_source_record(
		    &next_downstream, &record, &generated) !=
	    PCM_DOWNSTREAM_TX_OK)
		return false;
	next.bit_index = record.bit_index;
	next.complete = record.complete;
	if (!v92_phase4_cpd_tx_is_valid(&next) ||
	    !pcm_downstream_tx_is_valid(&next_downstream))
		return false;
	*transmitter = next;
	*downstream = next_downstream;
	*symbol = generated;
	return true;
}
