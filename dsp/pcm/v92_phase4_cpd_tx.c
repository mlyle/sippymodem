// SPDX-License-Identifier: BSD-3-Clause

#include "v92_phase4_cpd_tx.h"

#include "byte_fingerprint.h"
#include "memory_region.h"
#include "v92_pcm_mapping.h"

bool v92_phase4_cpd_tx_is_valid(
	const struct v92_phase4_cpd_tx *transmitter)
{
	size_t sequence_bytes;

	if (transmitter == NULL || !g711_law_is_valid(transmitter->law) ||
	    transmitter->frame_bits < V92_CPD_DOWNSTREAM_FRAME_MIN_BITS ||
	    transmitter->frame_bits > V92_CPD_DOWNSTREAM_DATA_MAX_BITS ||
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
	return byte_fingerprint(transmitter->sequence, sequence_bytes) ==
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
	if (!pcm_downstream_tx_is_valid(downstream) || downstream->protocol != PCM_PROTOCOL_V92)
		return false;
	/* A selected CPd must describe a complete usable upstream mapping, whether
	 * carried by downstream training or the old data mapping during FPE.
	 * Compact records may acknowledge a mapping, but cannot create one. */
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
		byte_fingerprint(initialized.sequence, sequence_bytes);
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

bool v92_phase4_ed_tx_is_valid(const struct v92_phase4_ed_tx *tail)
{
	if (tail == NULL || !pcm_downstream_tx_is_valid(&tail->transmit) ||
	    tail->transmit.protocol != PCM_PROTOCOL_V92)
		return false;
	if (!tail->cpd_pending)
		return tail->transmit.ed_requested;
	return !tail->transmit.ed_requested &&
	       pcm_downstream_tx_is_v92_control_boundary(&tail->transmit) &&
	       tail->transmit.v92_control_kind == PCM_DOWNSTREAM_V92_CONTROL_SUVD &&
	       tail->transmit.control.suvd.acknowledges_cpu &&
	       v92_phase4_cpd_tx_is_valid(&tail->cpd) && !tail->cpd.complete &&
	       tail->cpd.law == tail->transmit.encoder.mapping.law &&
	       tail->cpd.frame_bits == tail->transmit.encoder.mapping.frame_bits;
}

bool v92_phase4_ed_tx_init(
	struct v92_phase4_ed_tx *tail,
	const struct pcm_downstream_tx *transmit,
	const struct v92_phase4_cpd_tx *pending_cpd)
{
	struct v92_phase4_ed_tx initialized = { 0 };
	struct v92_cpd_parameters parameters;

	if (tail == NULL || transmit == NULL ||
	    memory_regions_overlap(tail, sizeof(*tail), transmit, sizeof(*transmit)) ||
	    (pending_cpd != NULL && memory_regions_overlap(
		    tail, sizeof(*tail), pending_cpd, sizeof(*pending_cpd))))
		return false;
	initialized.transmit = *transmit;
	if (pending_cpd != NULL) {
		initialized.cpd = *pending_cpd;
		initialized.cpd_pending = true;
		if (!v92_phase4_ed_tx_is_valid(&initialized) ||
		    v92_cpd_decode(pending_cpd->sequence, sizeof(pending_cpd->sequence),
			    pending_cpd->sequence_bits, pending_cpd->frame_bits,
			    &parameters) != V92_CPD_OK || !parameters.acknowledges_cpu)
			return false;
	} else if (pcm_downstream_tx_request_ed(&initialized.transmit) !=
			   PCM_DOWNSTREAM_TX_OK) {
		return false;
	}
	if (!v92_phase4_ed_tx_is_valid(&initialized))
		return false;
	*tail = initialized;
	return true;
}

enum pcm_downstream_tx_result v92_phase4_ed_tx_next(
	struct v92_phase4_ed_tx *tail, struct pcm_downstream_symbol *symbol)
{
	struct v92_phase4_ed_tx next;
	struct pcm_downstream_symbol generated;

	if (tail == NULL || symbol == NULL ||
	    memory_regions_overlap(tail, sizeof(*tail), symbol, sizeof(*symbol)))
		return PCM_DOWNSTREAM_TX_INVALID_ARGUMENT;
	if (!v92_phase4_ed_tx_is_valid(tail))
		return PCM_DOWNSTREAM_TX_INVALID_STATE;
	if (!tail->cpd_pending)
		return pcm_downstream_tx_next(&tail->transmit, symbol);
	next = *tail;
	if (!v92_phase4_cpd_tx_next(&next.cpd, &next.transmit, &generated))
		return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
	if (next.cpd.complete) {
		if (pcm_downstream_tx_request_ed(&next.transmit) != PCM_DOWNSTREAM_TX_OK)
			return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
		next.cpd_pending = false;
	}
	*tail = next;
	*symbol = generated;
	return PCM_DOWNSTREAM_TX_OK;
}
