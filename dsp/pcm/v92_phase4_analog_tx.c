// SPDX-License-Identifier: BSD-3-Clause

#include "v92_phase4_analog_tx.h"

#include "memory_region.h"

#include <stddef.h>
#include <string.h>

static bool stage_is_valid(enum v92_phase4_analog_tx_stage stage)
{
	return stage == V92_PHASE4_ANALOG_TX_TRN2U ||
	       stage == V92_PHASE4_ANALOG_TX_SUVU ||
	       stage == V92_PHASE4_ANALOG_TX_CPU ||
	       stage == V92_PHASE4_ANALOG_TX_E2U_READY;
}

static bool templates_are_valid(const struct v92_phase4_analog_tx *tx)
{
	return v92_suvu_parameters_are_valid(&tx->suvu) &&
	       !tx->suvu.acknowledges_cpd &&
	       v92_cpu_parameters_are_valid(&tx->cpu) &&
	       !tx->cpu.acknowledges_cpd;
}

bool v92_phase4_analog_tx_is_valid(
	const struct v92_phase4_analog_tx *tx)
{
	if (tx == NULL || !stage_is_valid(tx->stage) ||
	    !templates_are_valid(tx) ||
	    tx->cpu_sent != (tx->completed_cpu_records != 0u) ||
	    (tx->received_suvd == false && tx->cpu_pending) ||
	    (tx->received_cpd == false && tx->acknowledgment_sent) ||
	    (tx->stage == V92_PHASE4_ANALOG_TX_CPU && tx->cpu_pending))
		return false;
	if (tx->stage == V92_PHASE4_ANALOG_TX_TRN2U) {
		return v92_pcm_phase4_trn2u_is_valid(&tx->signal.trn2u) &&
		       !tx->cpu_sent && !tx->acknowledgment_sent &&
		       !tx->current_record_acknowledges_cpd;
	}
	if (!v92_phase4_record_tx_is_valid(&tx->signal.record))
		return false;
	if (tx->stage == V92_PHASE4_ANALOG_TX_E2U_READY) {
		return v92_phase4_record_tx_is_complete(&tx->signal.record) &&
		       tx->cpu_sent && tx->acknowledgment_sent &&
		       tx->peer_confirmation_received;
	}
	return true;
}

bool v92_phase4_analog_tx_is_e2u_ready(
	const struct v92_phase4_analog_tx *tx)
{
	return v92_phase4_analog_tx_is_valid(tx) &&
	       tx->stage == V92_PHASE4_ANALOG_TX_E2U_READY;
}

bool v92_phase4_analog_tx_silence_requested(
	const struct v92_phase4_analog_tx *tx)
{
	return v92_phase4_analog_tx_is_valid(tx) &&
	       tx->peer_requests_silence;
}

enum v92_phase4_analog_tx_result v92_phase4_analog_tx_init(
	struct v92_phase4_analog_tx *tx,
	enum v92_constellation_size constellation,
	uint8_t previous_encoded_sign, int16_t lu,
	const struct v92_suvu_parameters *suvu,
	const struct v92_cpu_parameters *cpu)
{
	struct v92_phase4_analog_tx initialized = { 0 };

	if (tx == NULL || suvu == NULL || cpu == NULL)
		return V92_PHASE4_ANALOG_TX_INVALID_ARGUMENT;
	if (!v92_suvu_parameters_are_valid(suvu) ||
	    suvu->acknowledges_cpd ||
	    !v92_cpu_parameters_are_valid(cpu) ||
	    cpu->acknowledges_cpd)
		return V92_PHASE4_ANALOG_TX_INVALID_PARAMETERS;
	if (v92_pcm_phase4_trn2u_init(
		    &initialized.signal.trn2u, constellation,
		    previous_encoded_sign, lu) != V92_PCM_PHASE4_TRN2U_OK)
		return V92_PHASE4_ANALOG_TX_INVALID_PARAMETERS;
	initialized.suvu = *suvu;
	initialized.cpu = *cpu;
	initialized.stage = V92_PHASE4_ANALOG_TX_TRN2U;
	if (!v92_phase4_analog_tx_is_valid(&initialized))
		return V92_PHASE4_ANALOG_TX_INTERNAL_ERROR;
	*tx = initialized;
	return V92_PHASE4_ANALOG_TX_OK;
}

static bool may_finish(const struct v92_phase4_analog_tx *tx)
{
	return tx->cpu_sent && tx->acknowledgment_sent &&
	       tx->peer_confirmation_received &&
	       v92_phase4_record_tx_is_complete(&tx->signal.record);
}

static void finish_if_ready(struct v92_phase4_analog_tx *tx)
{
	if (tx->stage != V92_PHASE4_ANALOG_TX_TRN2U && may_finish(tx))
		tx->stage = V92_PHASE4_ANALOG_TX_E2U_READY;
}

enum v92_phase4_analog_tx_result v92_phase4_analog_tx_note_suvd(
	struct v92_phase4_analog_tx *tx,
	const struct v92_suvd_parameters *suvd)
{
	struct v92_phase4_analog_tx next;

	if (tx == NULL || suvd == NULL)
		return V92_PHASE4_ANALOG_TX_INVALID_ARGUMENT;
	if (!v92_phase4_analog_tx_is_valid(tx) ||
	    tx->stage == V92_PHASE4_ANALOG_TX_E2U_READY)
		return V92_PHASE4_ANALOG_TX_INVALID_STATE;
	next = *tx;
	if (!next.received_suvd && !next.cpu_sent &&
	    next.stage != V92_PHASE4_ANALOG_TX_CPU)
		next.cpu_pending = true;
	next.received_suvd = true;
	next.peer_confirmation_received |= suvd->acknowledges_cpu;
	if (next.cpu_sent && next.peer_confirmation_received)
		next.cpu_pending = false;
	next.peer_requests_silence |= suvd->request_silence;
	finish_if_ready(&next);
	if (!v92_phase4_analog_tx_is_valid(&next))
		return V92_PHASE4_ANALOG_TX_INTERNAL_ERROR;
	*tx = next;
	return V92_PHASE4_ANALOG_TX_OK;
}

enum v92_phase4_analog_tx_result v92_phase4_analog_tx_note_cpd(
	struct v92_phase4_analog_tx *tx, bool acknowledges_cpu)
{
	struct v92_phase4_analog_tx next;

	if (tx == NULL)
		return V92_PHASE4_ANALOG_TX_INVALID_ARGUMENT;
	if (!v92_phase4_analog_tx_is_valid(tx) ||
	    tx->stage == V92_PHASE4_ANALOG_TX_E2U_READY)
		return V92_PHASE4_ANALOG_TX_INVALID_STATE;
	next = *tx;
	next.received_cpd = true;
	next.peer_confirmation_received |= acknowledges_cpu;
	if (next.cpu_sent && next.peer_confirmation_received)
		next.cpu_pending = false;
	finish_if_ready(&next);
	if (!v92_phase4_analog_tx_is_valid(&next))
		return V92_PHASE4_ANALOG_TX_INTERNAL_ERROR;
	*tx = next;
	return V92_PHASE4_ANALOG_TX_OK;
}

enum v92_phase4_analog_tx_result v92_phase4_analog_tx_note_ed(
	struct v92_phase4_analog_tx *tx)
{
	struct v92_phase4_analog_tx next;

	if (tx == NULL)
		return V92_PHASE4_ANALOG_TX_INVALID_ARGUMENT;
	if (!v92_phase4_analog_tx_is_valid(tx) ||
	    tx->stage == V92_PHASE4_ANALOG_TX_E2U_READY)
		return V92_PHASE4_ANALOG_TX_INVALID_STATE;
	next = *tx;
	next.peer_confirmation_received = true;
	if (next.cpu_sent)
		next.cpu_pending = false;
	finish_if_ready(&next);
	if (!v92_phase4_analog_tx_is_valid(&next))
		return V92_PHASE4_ANALOG_TX_INTERNAL_ERROR;
	*tx = next;
	return V92_PHASE4_ANALOG_TX_OK;
}

enum v92_phase4_analog_tx_result v92_phase4_analog_tx_repeat_cpu(
	struct v92_phase4_analog_tx *tx)
{
	struct v92_phase4_analog_tx next;

	if (tx == NULL)
		return V92_PHASE4_ANALOG_TX_INVALID_ARGUMENT;
	if (!v92_phase4_analog_tx_is_valid(tx) || !tx->cpu_sent ||
	    tx->peer_confirmation_received || tx->cpu_pending ||
	    tx->stage != V92_PHASE4_ANALOG_TX_SUVU)
		return V92_PHASE4_ANALOG_TX_INVALID_STATE;
	next = *tx;
	next.cpu_pending = true;
	if (!v92_phase4_analog_tx_is_valid(&next))
		return V92_PHASE4_ANALOG_TX_INTERNAL_ERROR;
	*tx = next;
	return V92_PHASE4_ANALOG_TX_OK;
}

static enum v92_phase4_analog_tx_result start_suvu(
	struct v92_phase4_analog_tx *tx, bool after_trn2u)
{
	struct v92_suvu_parameters parameters = tx->suvu;
	uint8_t frame[V92_SUVU_FRAME_BYTES] = { 0 };
	struct v92_phase4_modulator modulator;
	size_t frame_bits = 0u;
	enum v92_phase4_record_tx_result result;

	parameters.acknowledges_cpd = tx->received_cpd;
	if (v92_suvu_encode(
		    &parameters,
		    after_trn2u ? tx->signal.trn2u.modulator.constellation :
			    tx->signal.record.modulator.constellation,
		    frame, sizeof(frame), &frame_bits) != V92_SUVU_OK)
		return V92_PHASE4_ANALOG_TX_INTERNAL_ERROR;
	if (after_trn2u) {
		modulator = tx->signal.trn2u.modulator;
		memset(&tx->signal, 0, sizeof(tx->signal));
		result = v92_phase4_record_tx_init(
			&tx->signal.record, &modulator, frame,
			sizeof(frame), frame_bits);
	} else {
		result = v92_phase4_record_tx_start_next(
			&tx->signal.record, frame, sizeof(frame), frame_bits);
	}
	if (result != V92_PHASE4_RECORD_TX_OK)
		return V92_PHASE4_ANALOG_TX_INTERNAL_ERROR;
	tx->stage = V92_PHASE4_ANALOG_TX_SUVU;
	tx->current_record_acknowledges_cpd = parameters.acknowledges_cpd;
	return V92_PHASE4_ANALOG_TX_OK;
}

static enum v92_phase4_analog_tx_result start_cpu(
	struct v92_phase4_analog_tx *tx)
{
	struct v92_cpu_parameters parameters = tx->cpu;
	uint8_t frame[V92_CPU_MAX_FRAME_BYTES] = { 0 };
	size_t frame_bits = 0u;
	unsigned int bits = v92_phase4_modulator_bits_per_symbol(
		tx->signal.record.modulator.constellation);

	parameters.acknowledges_cpd = tx->received_cpd;
	if (v92_cpu_encode(
		    &parameters, bits, frame, sizeof(frame), &frame_bits) !=
	    V92_CPU_OK ||
	    v92_phase4_record_tx_start_next(
		    &tx->signal.record, frame, sizeof(frame), frame_bits) !=
	    V92_PHASE4_RECORD_TX_OK)
		return V92_PHASE4_ANALOG_TX_INTERNAL_ERROR;
	tx->stage = V92_PHASE4_ANALOG_TX_CPU;
	tx->cpu_pending = false;
	tx->current_record_acknowledges_cpd = parameters.acknowledges_cpd;
	return V92_PHASE4_ANALOG_TX_OK;
}

static enum v92_phase4_analog_tx_result begin_next_record(
	struct v92_phase4_analog_tx *tx)
{
	if (tx->cpu_pending)
		return start_cpu(tx);
	return start_suvu(tx, false);
}

static bool record_completed(struct v92_phase4_analog_tx *tx)
{
	if (tx->stage == V92_PHASE4_ANALOG_TX_CPU) {
		if (tx->completed_cpu_records == UINT64_MAX)
			return false;
		tx->completed_cpu_records++;
		tx->cpu_sent = true;
	}
	if (tx->current_record_acknowledges_cpd)
		tx->acknowledgment_sent = true;
	finish_if_ready(tx);
	return true;
}

enum v92_phase4_analog_tx_result v92_phase4_analog_tx_next(
	struct v92_phase4_analog_tx *tx, int16_t *sample)
{
	struct v92_phase4_analog_tx next;
	int16_t generated;
	enum v92_phase4_analog_tx_result result;

	if (tx == NULL || sample == NULL)
		return V92_PHASE4_ANALOG_TX_INVALID_ARGUMENT;
	if (memory_regions_overlap(tx, sizeof(*tx), sample, sizeof(*sample)))
		return V92_PHASE4_ANALOG_TX_INVALID_ARGUMENT;
	if (!v92_phase4_analog_tx_is_valid(tx))
		return V92_PHASE4_ANALOG_TX_INVALID_STATE;
	if (tx->stage == V92_PHASE4_ANALOG_TX_E2U_READY)
		return V92_PHASE4_ANALOG_TX_E2U_READY_RESULT;
	next = *tx;
	if (next.stage == V92_PHASE4_ANALOG_TX_TRN2U &&
	    !v92_pcm_phase4_trn2u_can_handoff(
		    &next.signal.trn2u, next.received_suvd)) {
		if (v92_pcm_phase4_trn2u_next(
			    &next.signal.trn2u, &generated) !=
		    V92_PCM_PHASE4_TRN2U_OK)
			return V92_PHASE4_ANALOG_TX_INTERNAL_ERROR;
	} else {
		if (next.stage == V92_PHASE4_ANALOG_TX_TRN2U)
			result = start_suvu(&next, true);
		else if (v92_phase4_record_tx_is_complete(
				 &next.signal.record))
			result = begin_next_record(&next);
		else
			result = V92_PHASE4_ANALOG_TX_OK;
		if (result != V92_PHASE4_ANALOG_TX_OK)
			return result;
		if (v92_phase4_record_tx_next(
			    &next.signal.record, &generated) !=
		    V92_PHASE4_RECORD_TX_OK)
			return V92_PHASE4_ANALOG_TX_INTERNAL_ERROR;
		if (v92_phase4_record_tx_is_complete(&next.signal.record) &&
		    !record_completed(&next))
			return V92_PHASE4_ANALOG_TX_INTERNAL_ERROR;
	}
	if (!v92_phase4_analog_tx_is_valid(&next))
		return V92_PHASE4_ANALOG_TX_INTERNAL_ERROR;
	*tx = next;
	*sample = generated;
	return V92_PHASE4_ANALOG_TX_OK;
}

enum v92_phase4_analog_tx_result v92_phase4_analog_tx_handoff(
	const struct v92_phase4_analog_tx *tx,
	struct v92_phase4_modulator *modulator)
{
	if (tx == NULL || modulator == NULL)
		return V92_PHASE4_ANALOG_TX_INVALID_ARGUMENT;
	if (memory_regions_overlap(
		    tx, sizeof(*tx), modulator, sizeof(*modulator)))
		return V92_PHASE4_ANALOG_TX_INVALID_ARGUMENT;
	if (!v92_phase4_analog_tx_is_e2u_ready(tx))
		return V92_PHASE4_ANALOG_TX_INVALID_STATE;
	if (v92_phase4_record_tx_handoff(
		    &tx->signal.record, modulator) !=
	    V92_PHASE4_RECORD_TX_OK)
		return V92_PHASE4_ANALOG_TX_INTERNAL_ERROR;
	return V92_PHASE4_ANALOG_TX_OK;
}

const char *v92_phase4_analog_tx_result_name(
	enum v92_phase4_analog_tx_result result)
{
	switch (result) {
	case V92_PHASE4_ANALOG_TX_OK:
		return "ok";
	case V92_PHASE4_ANALOG_TX_E2U_READY_RESULT:
		return "E2u ready";
	case V92_PHASE4_ANALOG_TX_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PHASE4_ANALOG_TX_INVALID_PARAMETERS:
		return "invalid parameters";
	case V92_PHASE4_ANALOG_TX_INVALID_STATE:
		return "invalid state";
	case V92_PHASE4_ANALOG_TX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
