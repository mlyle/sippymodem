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

bool v92_phase4_analog_tx_is_valid(
	const struct v92_phase4_analog_tx *tx)
{
	if (tx == NULL || !stage_is_valid(tx->stage) ||
	    !v92_upstream_control_is_valid(&tx->control))
		return false;
	if (tx->stage == V92_PHASE4_ANALOG_TX_TRN2U) {
		return v92_pcm_phase4_trn2u_is_valid(&tx->signal.trn2u) &&
		       tx->control.record == V92_UPSTREAM_CONTROL_NONE;
	}
	if (!v92_phase4_record_tx_is_valid(&tx->signal.record) ||
	    tx->control.record_complete != v92_phase4_record_tx_is_complete(&tx->signal.record))
		return false;
	if (tx->stage == V92_PHASE4_ANALOG_TX_E2U_READY) {
		return v92_phase4_record_tx_is_complete(&tx->signal.record) &&
		       v92_upstream_control_can_finish(&tx->control);
	}
	return tx->stage == V92_PHASE4_ANALOG_TX_CPU ?
		tx->control.record == V92_UPSTREAM_CONTROL_CPU :
		tx->control.record == V92_UPSTREAM_CONTROL_SUVU;
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
	       tx->control.peer_requests_silence;
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
	if (!v92_upstream_control_init(&initialized.control, suvu, cpu))
		return V92_PHASE4_ANALOG_TX_INVALID_PARAMETERS;
	if (v92_pcm_phase4_trn2u_init(
		    &initialized.signal.trn2u, constellation,
		    previous_encoded_sign, lu) != V92_PCM_PHASE4_TRN2U_OK)
		return V92_PHASE4_ANALOG_TX_INVALID_PARAMETERS;
	initialized.stage = V92_PHASE4_ANALOG_TX_TRN2U;
	if (!v92_phase4_analog_tx_is_valid(&initialized))
		return V92_PHASE4_ANALOG_TX_INTERNAL_ERROR;
	*tx = initialized;
	return V92_PHASE4_ANALOG_TX_OK;
}

enum v92_phase4_analog_tx_result v92_phase4_analog_tx_init_after_ru(
	struct v92_phase4_analog_tx *tx,
	enum v92_constellation_size constellation, int16_t lu,
	const struct v92_suvu_parameters *suvu,
	const struct v92_cpu_parameters *cpu)
{
	struct v92_phase4_analog_tx initialized;
	struct v92_cpu_parameters selected;
	if (tx == NULL || cpu == NULL)
		return V92_PHASE4_ANALOG_TX_INVALID_ARGUMENT;
	selected = *cpu;
	selected.acknowledges_cpd = false;
	enum v92_phase4_analog_tx_result result = v92_phase4_analog_tx_init(
		&initialized, constellation, 0u, lu, suvu, &selected);
	if (result != V92_PHASE4_ANALOG_TX_OK)
		return result;
	initialized.renegotiation = true;
	*tx = initialized;
	return V92_PHASE4_ANALOG_TX_OK;
}

bool v92_phase4_analog_tx_can_handoff(const struct v92_phase4_analog_tx *tx)
{
	return v92_phase4_analog_tx_is_valid(tx) &&
	       v92_upstream_control_can_finish(&tx->control);
}

static void finish_if_ready(struct v92_phase4_analog_tx *tx)
{
	if (v92_phase4_analog_tx_can_handoff(tx) &&
	    v92_phase4_record_tx_is_complete(&tx->signal.record))
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
	if (!v92_upstream_control_note_suvd(&next.control, suvd))
		return V92_PHASE4_ANALOG_TX_INTERNAL_ERROR;
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
	if (!v92_upstream_control_note_cpd(&next.control, acknowledges_cpu))
		return V92_PHASE4_ANALOG_TX_INTERNAL_ERROR;
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
	if (!v92_upstream_control_note_ed(&next.control))
		return V92_PHASE4_ANALOG_TX_INTERNAL_ERROR;
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
	if (!v92_phase4_analog_tx_is_valid(tx))
		return V92_PHASE4_ANALOG_TX_INVALID_STATE;
	next = *tx;
	if (!v92_upstream_control_repeat_cpu(&next.control))
		return V92_PHASE4_ANALOG_TX_INVALID_STATE;
	if (!v92_phase4_analog_tx_is_valid(&next))
		return V92_PHASE4_ANALOG_TX_INTERNAL_ERROR;
	*tx = next;
	return V92_PHASE4_ANALOG_TX_OK;
}

static enum v92_phase4_analog_tx_result start_record(
	struct v92_phase4_analog_tx *tx, bool after_trn2u)
{
	uint8_t frame[V92_CPU_MAX_FRAME_BYTES] = { 0 };
	struct v92_phase4_modulator modulator;
	size_t frame_bits = 0u;
	enum v92_phase4_record_tx_result result;

	if (!v92_upstream_control_start(
		    &tx->control,
		    v92_phase4_modulator_bits_per_symbol(after_trn2u ?
			    tx->signal.trn2u.modulator.constellation :
			    tx->signal.record.modulator.constellation) *
			    V92_SUVU_SYMBOLS_PER_FRAME,
		    frame, sizeof(frame), &frame_bits))
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
	tx->stage = tx->control.record == V92_UPSTREAM_CONTROL_CPU ?
		V92_PHASE4_ANALOG_TX_CPU : V92_PHASE4_ANALOG_TX_SUVU;
	return V92_PHASE4_ANALOG_TX_OK;
}

static bool record_completed(struct v92_phase4_analog_tx *tx)
{
	if (!v92_upstream_control_complete(&tx->control))
		return false;
	finish_if_ready(tx);
	return true;
}

static bool trn2u_complete(const struct v92_phase4_analog_tx *tx)
{
	uint32_t count = tx->signal.trn2u.emitted_symbols;
	/* V.92 9.6.2.1.1 and 9.8.2: use the earliest permitted local end,
	 * or the next aligned boundary after receipt of SUVd. */
	uint32_t minimum = tx->renegotiation ? 2400u :
		V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS;
	return count && count % V92_PCM_PHASE4_TRN2U_ALIGNMENT == 0u &&
		(tx->control.received_suvd || count >= minimum);
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
	    !trn2u_complete(&next)) {
		if (v92_pcm_phase4_trn2u_next(
			    &next.signal.trn2u, &generated) !=
		    V92_PCM_PHASE4_TRN2U_OK)
			return V92_PHASE4_ANALOG_TX_INTERNAL_ERROR;
	} else {
		if (next.stage == V92_PHASE4_ANALOG_TX_TRN2U)
			result = start_record(&next, true);
		else if (v92_phase4_record_tx_is_complete(
				 &next.signal.record))
			result = start_record(&next, false);
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
	struct v92_phase4_record_tx *record)
{
	if (tx == NULL || record == NULL)
		return V92_PHASE4_ANALOG_TX_INVALID_ARGUMENT;
	if (memory_regions_overlap(
		    tx, sizeof(*tx), record, sizeof(*record)))
		return V92_PHASE4_ANALOG_TX_INVALID_ARGUMENT;
	if (!v92_phase4_analog_tx_can_handoff(tx))
		return V92_PHASE4_ANALOG_TX_INVALID_STATE;
	*record = tx->signal.record;
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
