// SPDX-License-Identifier: BSD-3-Clause

#include "v92_phase3_digital_tx.h"

#include "packed_bits.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static bool stage_is_valid(enum v92_phase3_digital_tx_stage stage)
{
	return stage >= V92_PHASE3_DIGITAL_TX_PREFIX &&
	       stage <= V92_PHASE3_DIGITAL_TX_COMPLETE;
}

static bool jd_frame_is_valid(const uint8_t *frame)
{
	struct v92_jd jd;
	uint8_t canonical[V92_J_FRAME_BYTES];

	return v92_jd_decode(frame, V92_J_FRAME_BYTES, &jd) == V92_J_OK &&
	       v92_jd_encode(&jd, canonical, sizeof(canonical)) == V92_J_OK &&
	       memcmp(frame, canonical, sizeof(canonical)) == 0;
}

static bool jp_frame_is_valid(const uint8_t *frame)
{
	struct v92_jp jp;
	uint8_t canonical[V92_J_FRAME_BYTES];

	return v92_jp_decode(frame, V92_J_FRAME_BYTES, &jp) == V92_J_OK &&
	       v92_jp_encode(&jp, canonical, sizeof(canonical)) == V92_J_OK &&
	       memcmp(frame, canonical, sizeof(canonical)) == 0;
}

static bool counters_are_valid(
	const struct v92_phase3_digital_tx *transmitter)
{
	if (transmitter->stage == V92_PHASE3_DIGITAL_TX_PREFIX)
		return transmitter->jd_records == 0 &&
		       transmitter->jp_records == 0 &&
		       transmitter->bit_index == 0 &&
		       !transmitter->finish_jd_requested &&
		       !transmitter->finish_jp_requested;
	if (transmitter->jd_records == UINT32_MAX ||
	    transmitter->jp_records == UINT32_MAX)
		return false;
	if (transmitter->stage == V92_PHASE3_DIGITAL_TX_JD)
		return transmitter->jp_records == 0 &&
		       transmitter->bit_index < V92_J_FRAME_BITS &&
		       !transmitter->finish_jp_requested &&
		       !(transmitter->finish_jd_requested &&
			 transmitter->jd_records != 0 &&
			 transmitter->bit_index == 0);
	if (transmitter->jd_records == 0 ||
	    !transmitter->finish_jd_requested)
		return false;
	if (transmitter->stage == V92_PHASE3_DIGITAL_TX_JP)
		return transmitter->bit_index < V92_J_FRAME_BITS &&
		       !(transmitter->finish_jp_requested &&
			 transmitter->jp_records != 0 &&
			 transmitter->bit_index == 0);
	if (transmitter->jp_records == 0 ||
	    !transmitter->finish_jp_requested)
		return false;
	if (transmitter->stage == V92_PHASE3_DIGITAL_TX_JP_PRIME)
		return transmitter->bit_index < V92_PHASE3_JP_PRIME_BITS;
	return transmitter->stage == V92_PHASE3_DIGITAL_TX_COMPLETE &&
	       transmitter->bit_index == V92_PHASE3_JP_PRIME_BITS;
}

bool v92_phase3_digital_tx_is_valid(
	const struct v92_phase3_digital_tx *transmitter)
{
	if (transmitter == NULL || !stage_is_valid(transmitter->stage) ||
	    !jd_frame_is_valid(transmitter->jd_frame) ||
	    !jp_frame_is_valid(transmitter->jp_frame) ||
	    !counters_are_valid(transmitter))
		return false;
	if (transmitter->stage == V92_PHASE3_DIGITAL_TX_PREFIX)
		return pcm_phase3_digital_prefix_is_valid(
			&transmitter->sequence.prefix);
	return v90_phase3_signs_is_valid(&transmitter->sequence.signs) &&
	       transmitter->sequence.signs.phase == V90_PHASE3_SIGNS_JD &&
	       transmitter->sequence.signs.trn1d_symbols ==
		       V90_PHASE3_TRN1D_MIN_SYMBOLS;
}

enum v92_phase3_digital_tx_result v92_phase3_digital_tx_init(
	struct v92_phase3_digital_tx *transmitter, enum g711_law law,
	uint8_t training_ucode, const struct v92_jd *jd,
	const struct v92_jp *jp)
{
	struct v92_phase3_digital_tx initialized = {
		.stage = V92_PHASE3_DIGITAL_TX_PREFIX,
	};

	if (transmitter == NULL || jd == NULL || jp == NULL ||
	    v92_jd_encode(jd, initialized.jd_frame,
			  sizeof(initialized.jd_frame)) != V92_J_OK ||
	    v92_jp_encode(jp, initialized.jp_frame,
			  sizeof(initialized.jp_frame)) != V92_J_OK ||
	    pcm_phase3_digital_prefix_init(
		    &initialized.sequence.prefix, law, training_ucode) !=
		    PCM_PHASE3_DIGITAL_PREFIX_OK ||
	    !v92_phase3_digital_tx_is_valid(&initialized))
		return V92_PHASE3_DIGITAL_TX_INVALID_ARGUMENT;
	*transmitter = initialized;
	return V92_PHASE3_DIGITAL_TX_OK;
}

static enum v92_phase3_digital_tx_result next_prefix(
	struct v92_phase3_digital_tx *transmitter,
	struct v90_pcm_symbol *symbol)
{
	struct v90_phase3_signs signs;

	if (pcm_phase3_digital_prefix_next(
		    &transmitter->sequence.prefix, symbol) !=
	    PCM_PHASE3_DIGITAL_PREFIX_OK)
		return V92_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
	if (!pcm_phase3_digital_prefix_is_complete(
		    &transmitter->sequence.prefix))
		return V92_PHASE3_DIGITAL_TX_OK;
	if (pcm_phase3_digital_prefix_handoff(
		    &transmitter->sequence.prefix, &signs) !=
		    PCM_PHASE3_DIGITAL_PREFIX_OK ||
	    v90_phase3_signs_begin_jd(&signs) != V90_PHASE3_SIGNS_OK)
		return V92_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
	transmitter->sequence = (union v92_phase3_digital_tx_sequence){
		.signs = signs,
	};
	transmitter->stage = V92_PHASE3_DIGITAL_TX_JD;
	return V92_PHASE3_DIGITAL_TX_OK;
}

static uint8_t record_bit(
	const struct v92_phase3_digital_tx *transmitter)
{
	const uint8_t *frame =
		transmitter->stage == V92_PHASE3_DIGITAL_TX_JD ?
			transmitter->jd_frame : transmitter->jp_frame;

	return packed_msb_bit(frame, transmitter->bit_index);
}

static enum v92_phase3_digital_tx_result next_record(
	struct v92_phase3_digital_tx *transmitter,
	struct v90_pcm_symbol *symbol)
{
	bool jd = transmitter->stage == V92_PHASE3_DIGITAL_TX_JD;
	uint32_t *records = jd ? &transmitter->jd_records :
				 &transmitter->jp_records;

	if (v90_phase3_signs_next_jd_bit(
		    &transmitter->sequence.signs, record_bit(transmitter),
		    symbol) != V90_PHASE3_SIGNS_OK)
		return V92_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
	transmitter->bit_index++;
	if (transmitter->bit_index != V92_J_FRAME_BITS)
		return V92_PHASE3_DIGITAL_TX_OK;
	if (*records == UINT32_MAX - 1u)
		return V92_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
	(*records)++;
	transmitter->bit_index = 0;
	if (jd && transmitter->finish_jd_requested)
		transmitter->stage = V92_PHASE3_DIGITAL_TX_JP;
	else if (!jd && transmitter->finish_jp_requested)
		transmitter->stage = V92_PHASE3_DIGITAL_TX_JP_PRIME;
	return V92_PHASE3_DIGITAL_TX_OK;
}

static enum v92_phase3_digital_tx_result next_jp_prime(
	struct v92_phase3_digital_tx *transmitter,
	struct v90_pcm_symbol *symbol)
{
	if (v90_phase3_signs_next_jd_bit(
		    &transmitter->sequence.signs, 0, symbol) !=
	    V90_PHASE3_SIGNS_OK)
		return V92_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
	transmitter->bit_index++;
	if (transmitter->bit_index == V92_PHASE3_JP_PRIME_BITS)
		transmitter->stage = V92_PHASE3_DIGITAL_TX_COMPLETE;
	return V92_PHASE3_DIGITAL_TX_OK;
}

enum v92_phase3_digital_tx_result v92_phase3_digital_tx_next(
	struct v92_phase3_digital_tx *transmitter,
	struct v90_pcm_symbol *symbol)
{
	struct v92_phase3_digital_tx next;
	struct v90_pcm_symbol generated;
	enum v92_phase3_digital_tx_result result;

	if (!v92_phase3_digital_tx_is_valid(transmitter) || symbol == NULL)
		return V92_PHASE3_DIGITAL_TX_INVALID_ARGUMENT;
	if (transmitter->stage == V92_PHASE3_DIGITAL_TX_COMPLETE)
		return V92_PHASE3_DIGITAL_TX_DONE;
	next = *transmitter;
	switch (next.stage) {
	case V92_PHASE3_DIGITAL_TX_PREFIX:
		result = next_prefix(&next, &generated);
		break;
	case V92_PHASE3_DIGITAL_TX_JD:
	case V92_PHASE3_DIGITAL_TX_JP:
		result = next_record(&next, &generated);
		break;
	case V92_PHASE3_DIGITAL_TX_JP_PRIME:
		result = next_jp_prime(&next, &generated);
		break;
	case V92_PHASE3_DIGITAL_TX_COMPLETE:
		return V92_PHASE3_DIGITAL_TX_DONE;
	default:
		return V92_PHASE3_DIGITAL_TX_INVALID_ARGUMENT;
	}
	if (result != V92_PHASE3_DIGITAL_TX_OK)
		return result;
	if (!v92_phase3_digital_tx_is_valid(&next))
		return V92_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
	*transmitter = next;
	*symbol = generated;
	return V92_PHASE3_DIGITAL_TX_OK;
}

static enum v92_phase3_digital_tx_result request_finish(
	struct v92_phase3_digital_tx *transmitter,
	enum v92_phase3_digital_tx_stage required_stage)
{
	struct v92_phase3_digital_tx next;
	bool jd = required_stage == V92_PHASE3_DIGITAL_TX_JD;
	uint32_t records;

	if (!v92_phase3_digital_tx_is_valid(transmitter))
		return V92_PHASE3_DIGITAL_TX_INVALID_ARGUMENT;
	if (transmitter->stage != required_stage)
		return V92_PHASE3_DIGITAL_TX_INVALID_STATE;
	next = *transmitter;
	if (jd)
		next.finish_jd_requested = true;
	else
		next.finish_jp_requested = true;
	records = jd ? next.jd_records : next.jp_records;
	if (records != 0 && next.bit_index == 0)
		next.stage = jd ? V92_PHASE3_DIGITAL_TX_JP :
				  V92_PHASE3_DIGITAL_TX_JP_PRIME;
	if (!v92_phase3_digital_tx_is_valid(&next))
		return V92_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
	*transmitter = next;
	return V92_PHASE3_DIGITAL_TX_OK;
}

enum v92_phase3_digital_tx_result v92_phase3_digital_tx_finish_jd(
	struct v92_phase3_digital_tx *transmitter)
{
	return request_finish(transmitter, V92_PHASE3_DIGITAL_TX_JD);
}

enum v92_phase3_digital_tx_result v92_phase3_digital_tx_finish_jp(
	struct v92_phase3_digital_tx *transmitter)
{
	return request_finish(transmitter, V92_PHASE3_DIGITAL_TX_JP);
}

const char *v92_phase3_digital_tx_result_name(
	enum v92_phase3_digital_tx_result result)
{
	switch (result) {
	case V92_PHASE3_DIGITAL_TX_OK:
		return "ok";
	case V92_PHASE3_DIGITAL_TX_DONE:
		return "done";
	case V92_PHASE3_DIGITAL_TX_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PHASE3_DIGITAL_TX_INVALID_STATE:
		return "invalid state";
	case V92_PHASE3_DIGITAL_TX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
