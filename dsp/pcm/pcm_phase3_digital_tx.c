// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_phase3_digital_tx.h"

#include "packed_bits.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { PCM_PHASE3_PRIME_BITS = 12 };

_Static_assert((int)V90_PHASE3_JD_PRIME_BITS == PCM_PHASE3_PRIME_BITS,
	"V.90 prime suffix must use the common Phase 3 length");
_Static_assert((int)V92_PHASE3_JP_PRIME_BITS == PCM_PHASE3_PRIME_BITS,
	"V.92 prime suffix must use the common Phase 3 length");

static bool stage_is_valid(enum pcm_phase3_digital_tx_stage stage)
{
	return stage >= PCM_PHASE3_DIGITAL_TX_PREFIX &&
	       stage <= PCM_PHASE3_DIGITAL_TX_COMPLETE;
}

static bool bytes_are_zero(const uint8_t *bytes, size_t length)
{
	for (size_t index = 0; index < length; ++index) {
		if (bytes[index] != 0)
			return false;
	}
	return true;
}

static bool v90_jd_frame_is_valid(const uint8_t *frame)
{
	struct v90_jd_parameters parameters;
	uint8_t canonical[PCM_J_FRAME_BYTES];

	return v90_jd_decode(
		       frame, PCM_J_FRAME_BYTES, &parameters) == V90_JD_OK &&
	       v90_jd_encode(
		       &parameters, canonical, sizeof(canonical)) == V90_JD_OK &&
	       memcmp(frame, canonical, sizeof(canonical)) == 0;
}

static bool v92_jd_frame_is_valid(const uint8_t *frame)
{
	struct v92_jd jd;
	uint8_t canonical[PCM_J_FRAME_BYTES];

	return v92_jd_decode(frame, PCM_J_FRAME_BYTES, &jd) == V92_J_OK &&
	       v92_jd_encode(&jd, canonical, sizeof(canonical)) == V92_J_OK &&
	       memcmp(frame, canonical, sizeof(canonical)) == 0;
}

static bool v92_jp_frame_is_valid(const uint8_t *frame)
{
	struct v92_jp jp;
	uint8_t canonical[PCM_J_FRAME_BYTES];

	return v92_jp_decode(frame, PCM_J_FRAME_BYTES, &jp) == V92_J_OK &&
	       v92_jp_encode(&jp, canonical, sizeof(canonical)) == V92_J_OK &&
	       memcmp(frame, canonical, sizeof(canonical)) == 0;
}

static bool frames_are_valid(
	const struct pcm_phase3_digital_tx *transmitter)
{
	if (transmitter->protocol == PCM_PROTOCOL_V90)
		return v90_jd_frame_is_valid(transmitter->jd_frame) &&
		       bytes_are_zero(
			       transmitter->jp_frame,
			       sizeof(transmitter->jp_frame));
	return v92_jd_frame_is_valid(transmitter->jd_frame) &&
	       v92_jp_frame_is_valid(transmitter->jp_frame);
}

static bool counters_are_valid(
	const struct pcm_phase3_digital_tx *transmitter)
{
	if (transmitter->stage == PCM_PHASE3_DIGITAL_TX_PREFIX)
		return transmitter->jd_records == 0 &&
		       transmitter->jp_records == 0 &&
		       transmitter->scr_symbols == 0 &&
		       transmitter->bit_index == 0 &&
		       !transmitter->finish_jd_requested &&
		       !transmitter->finish_jp_requested &&
		       !transmitter->finish_scr_requested;
	if (transmitter->jd_records == UINT32_MAX ||
	    transmitter->jp_records == UINT32_MAX ||
	    transmitter->scr_symbols == UINT32_MAX)
		return false;
	if (transmitter->stage == PCM_PHASE3_DIGITAL_TX_JD)
		return transmitter->jp_records == 0 &&
		       transmitter->scr_symbols == 0 &&
		       transmitter->bit_index < PCM_J_FRAME_BITS &&
		       !transmitter->finish_jp_requested &&
		       !transmitter->finish_scr_requested &&
		       !(transmitter->finish_jd_requested &&
			 transmitter->jd_records != 0 &&
			 transmitter->bit_index == 0);
	if (transmitter->jd_records == 0 ||
	    !transmitter->finish_jd_requested)
		return false;
	if (transmitter->protocol == PCM_PROTOCOL_V90) {
		if (transmitter->stage == PCM_PHASE3_DIGITAL_TX_JP ||
		    transmitter->jp_records != 0 ||
		    transmitter->finish_jp_requested ||
		    transmitter->scr_symbols != 0 ||
		    transmitter->finish_scr_requested)
			return false;
	} else {
		if (transmitter->stage == PCM_PHASE3_DIGITAL_TX_JP)
			return transmitter->bit_index < PCM_J_FRAME_BITS &&
			       transmitter->scr_symbols == 0 &&
			       !transmitter->finish_scr_requested &&
			       !(transmitter->finish_jp_requested &&
				 transmitter->jp_records != 0 &&
				 transmitter->bit_index == 0);
		if (transmitter->jp_records == 0 ||
		    !transmitter->finish_jp_requested)
			return false;
	}
	if (transmitter->stage == PCM_PHASE3_DIGITAL_TX_PRIME)
		return transmitter->scr_symbols == 0 &&
		       !transmitter->finish_scr_requested &&
		       transmitter->bit_index < PCM_PHASE3_PRIME_BITS;
	if (transmitter->protocol == PCM_PROTOCOL_V90)
		return transmitter->stage == PCM_PHASE3_DIGITAL_TX_COMPLETE &&
		       transmitter->bit_index == PCM_PHASE3_PRIME_BITS;
	if (transmitter->stage == PCM_PHASE3_DIGITAL_TX_SCR)
		return transmitter->bit_index == PCM_PHASE3_PRIME_BITS &&
		       !(transmitter->finish_scr_requested &&
			 transmitter->scr_symbols != 0 &&
			 transmitter->scr_symbols % 6u == 0);
	return transmitter->stage == PCM_PHASE3_DIGITAL_TX_COMPLETE &&
	       transmitter->bit_index == PCM_PHASE3_PRIME_BITS &&
	       transmitter->finish_scr_requested &&
	       transmitter->scr_symbols != 0 &&
	       transmitter->scr_symbols % 6u == 0;
}

bool pcm_phase3_digital_tx_is_valid(
	const struct pcm_phase3_digital_tx *transmitter)
{
	if (transmitter == NULL ||
	    !pcm_protocol_is_valid(transmitter->protocol) ||
	    !stage_is_valid(transmitter->stage) ||
	    !frames_are_valid(transmitter) ||
	    !counters_are_valid(transmitter))
		return false;
	if (transmitter->stage == PCM_PHASE3_DIGITAL_TX_PREFIX)
		return pcm_phase3_digital_prefix_is_valid(
			&transmitter->sequence.prefix);
	return v90_phase3_signs_is_valid(&transmitter->sequence.signs) &&
	       transmitter->sequence.signs.phase == V90_PHASE3_SIGNS_JD &&
	       transmitter->sequence.signs.trn1d_symbols ==
		       V90_PHASE3_TRN1D_MIN_SYMBOLS;
}

enum pcm_phase3_digital_tx_result pcm_phase3_digital_tx_init_v90(
	struct pcm_phase3_digital_tx *transmitter, enum g711_law law,
	uint8_t training_ucode,
	const struct v90_jd_parameters *jd_parameters)
{
	struct pcm_phase3_digital_tx initialized = {
		.protocol = PCM_PROTOCOL_V90,
		.stage = PCM_PHASE3_DIGITAL_TX_PREFIX,
	};

	if (transmitter == NULL || jd_parameters == NULL ||
	    v90_jd_encode(
		    jd_parameters, initialized.jd_frame,
		    sizeof(initialized.jd_frame)) != V90_JD_OK ||
	    pcm_phase3_digital_prefix_init(
		    &initialized.sequence.prefix, law, training_ucode) !=
		    PCM_PHASE3_DIGITAL_PREFIX_OK ||
	    !pcm_phase3_digital_tx_is_valid(&initialized))
		return PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT;
	*transmitter = initialized;
	return PCM_PHASE3_DIGITAL_TX_OK;
}

enum pcm_phase3_digital_tx_result pcm_phase3_digital_tx_init_v92(
	struct pcm_phase3_digital_tx *transmitter, enum g711_law law,
	uint8_t training_ucode, const struct v92_jd *jd,
	const struct v92_jp *jp)
{
	struct pcm_phase3_digital_tx initialized = {
		.protocol = PCM_PROTOCOL_V92,
		.stage = PCM_PHASE3_DIGITAL_TX_PREFIX,
	};

	if (transmitter == NULL || jd == NULL || jp == NULL ||
	    v92_jd_encode(
		    jd, initialized.jd_frame,
		    sizeof(initialized.jd_frame)) != V92_J_OK ||
	    v92_jp_encode(
		    jp, initialized.jp_frame,
		    sizeof(initialized.jp_frame)) != V92_J_OK ||
	    pcm_phase3_digital_prefix_init(
		    &initialized.sequence.prefix, law, training_ucode) !=
		    PCM_PHASE3_DIGITAL_PREFIX_OK ||
	    !pcm_phase3_digital_tx_is_valid(&initialized))
		return PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT;
	*transmitter = initialized;
	return PCM_PHASE3_DIGITAL_TX_OK;
}

static enum pcm_phase3_digital_tx_result next_prefix(
	struct pcm_phase3_digital_tx *transmitter,
	struct v90_pcm_symbol *symbol)
{
	struct v90_phase3_signs signs;

	if (pcm_phase3_digital_prefix_next(
		    &transmitter->sequence.prefix, symbol) !=
	    PCM_PHASE3_DIGITAL_PREFIX_OK)
		return PCM_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
	if (!pcm_phase3_digital_prefix_is_complete(
		    &transmitter->sequence.prefix))
		return PCM_PHASE3_DIGITAL_TX_OK;
	if (pcm_phase3_digital_prefix_handoff(
		    &transmitter->sequence.prefix, &signs) !=
		    PCM_PHASE3_DIGITAL_PREFIX_OK ||
	    v90_phase3_signs_begin_jd(&signs) != V90_PHASE3_SIGNS_OK)
		return PCM_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
	transmitter->sequence = (union pcm_phase3_digital_tx_sequence){
		.signs = signs,
	};
	transmitter->stage = PCM_PHASE3_DIGITAL_TX_JD;
	return PCM_PHASE3_DIGITAL_TX_OK;
}

static uint8_t record_bit(
	const struct pcm_phase3_digital_tx *transmitter)
{
	const uint8_t *frame =
		transmitter->stage == PCM_PHASE3_DIGITAL_TX_JD ?
			transmitter->jd_frame : transmitter->jp_frame;

	return packed_msb_bit(frame, transmitter->bit_index);
}

static enum pcm_phase3_digital_tx_result next_record(
	struct pcm_phase3_digital_tx *transmitter,
	struct v90_pcm_symbol *symbol)
{
	bool jd = transmitter->stage == PCM_PHASE3_DIGITAL_TX_JD;
	uint32_t *records = jd ? &transmitter->jd_records :
				 &transmitter->jp_records;
	bool finish_requested = jd ? transmitter->finish_jd_requested :
				      transmitter->finish_jp_requested;

	if (v90_phase3_signs_next_jd_bit(
		    &transmitter->sequence.signs, record_bit(transmitter),
		    symbol) != V90_PHASE3_SIGNS_OK)
		return PCM_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
	transmitter->bit_index++;
	if (transmitter->bit_index != PCM_J_FRAME_BITS)
		return PCM_PHASE3_DIGITAL_TX_OK;
	if (*records == UINT32_MAX - 1u)
		return PCM_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
	(*records)++;
	transmitter->bit_index = 0;
	if (!finish_requested)
		return PCM_PHASE3_DIGITAL_TX_OK;
	if (jd && transmitter->protocol == PCM_PROTOCOL_V92)
		transmitter->stage = PCM_PHASE3_DIGITAL_TX_JP;
	else
		transmitter->stage = PCM_PHASE3_DIGITAL_TX_PRIME;
	return PCM_PHASE3_DIGITAL_TX_OK;
}

static enum pcm_phase3_digital_tx_result next_prime(
	struct pcm_phase3_digital_tx *transmitter,
	struct v90_pcm_symbol *symbol)
{
	if (v90_phase3_signs_next_jd_bit(
		    &transmitter->sequence.signs, 0, symbol) !=
	    V90_PHASE3_SIGNS_OK)
		return PCM_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
	transmitter->bit_index++;
	if (transmitter->bit_index == PCM_PHASE3_PRIME_BITS)
		transmitter->stage =
			transmitter->protocol == PCM_PROTOCOL_V92 ?
				PCM_PHASE3_DIGITAL_TX_SCR :
				PCM_PHASE3_DIGITAL_TX_COMPLETE;
	return PCM_PHASE3_DIGITAL_TX_OK;
}

static enum pcm_phase3_digital_tx_result next_scr(
	struct pcm_phase3_digital_tx *transmitter,
	struct v90_pcm_symbol *symbol)
{
	uint8_t sign;

	if (transmitter->scr_symbols == UINT32_MAX - 1u ||
	    v34_scramble_group(
		    &transmitter->sequence.signs.scrambler,
		    1u, 1u, &sign) != V34_FRAMING_OK ||
	    !v90_pcm_symbol_make(
		    symbol, transmitter->sequence.signs.law,
		    transmitter->sequence.signs.training_ucode, sign != 0u))
		return PCM_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
	transmitter->scr_symbols++;
	if (transmitter->finish_scr_requested &&
	    transmitter->scr_symbols % 6u == 0)
		transmitter->stage = PCM_PHASE3_DIGITAL_TX_COMPLETE;
	return PCM_PHASE3_DIGITAL_TX_OK;
}

enum pcm_phase3_digital_tx_result pcm_phase3_digital_tx_next(
	struct pcm_phase3_digital_tx *transmitter,
	struct v90_pcm_symbol *symbol)
{
	struct pcm_phase3_digital_tx next;
	struct v90_pcm_symbol generated;
	enum pcm_phase3_digital_tx_result result;

	if (!pcm_phase3_digital_tx_is_valid(transmitter) || symbol == NULL)
		return PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT;
	if (transmitter->stage == PCM_PHASE3_DIGITAL_TX_COMPLETE)
		return PCM_PHASE3_DIGITAL_TX_DONE;
	next = *transmitter;
	switch (next.stage) {
	case PCM_PHASE3_DIGITAL_TX_PREFIX:
		result = next_prefix(&next, &generated);
		break;
	case PCM_PHASE3_DIGITAL_TX_JD:
	case PCM_PHASE3_DIGITAL_TX_JP:
		result = next_record(&next, &generated);
		break;
	case PCM_PHASE3_DIGITAL_TX_PRIME:
		result = next_prime(&next, &generated);
		break;
	case PCM_PHASE3_DIGITAL_TX_SCR:
		result = next_scr(&next, &generated);
		break;
	case PCM_PHASE3_DIGITAL_TX_COMPLETE:
		return PCM_PHASE3_DIGITAL_TX_DONE;
	default:
		return PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT;
	}
	if (result != PCM_PHASE3_DIGITAL_TX_OK)
		return result;
	if (!pcm_phase3_digital_tx_is_valid(&next))
		return PCM_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
	*transmitter = next;
	*symbol = generated;
	return PCM_PHASE3_DIGITAL_TX_OK;
}

enum pcm_phase3_digital_tx_result pcm_phase3_digital_tx_set_v92_su_phase(
	struct pcm_phase3_digital_tx *transmitter,
	uint16_t phase_extension_q16)
{
	struct pcm_phase3_digital_tx next;
	struct v92_jp jp;

	if (!pcm_phase3_digital_tx_is_valid(transmitter))
		return PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT;
	if (transmitter->protocol != PCM_PROTOCOL_V92 ||
	    transmitter->stage != PCM_PHASE3_DIGITAL_TX_JD ||
	    transmitter->finish_jd_requested)
		return PCM_PHASE3_DIGITAL_TX_INVALID_STATE;
	next = *transmitter;
	if (v92_jp_decode(
		    next.jp_frame, sizeof(next.jp_frame), &jp) != V92_J_OK)
		return PCM_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
	jp.su_phase_extension_q16 = phase_extension_q16;
	if (v92_jp_encode(
		    &jp, next.jp_frame, sizeof(next.jp_frame)) != V92_J_OK ||
	    !pcm_phase3_digital_tx_is_valid(&next))
		return PCM_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
	*transmitter = next;
	return PCM_PHASE3_DIGITAL_TX_OK;
}

static enum pcm_phase3_digital_tx_result request_finish(
	struct pcm_phase3_digital_tx *transmitter,
	enum pcm_phase3_digital_tx_stage required_stage)
{
	struct pcm_phase3_digital_tx next;
	bool jd = required_stage == PCM_PHASE3_DIGITAL_TX_JD;
	uint32_t records;

	if (!pcm_phase3_digital_tx_is_valid(transmitter))
		return PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT;
	if (transmitter->stage != required_stage ||
	    (!jd && transmitter->protocol != PCM_PROTOCOL_V92))
		return PCM_PHASE3_DIGITAL_TX_INVALID_STATE;
	next = *transmitter;
	if (jd)
		next.finish_jd_requested = true;
	else
		next.finish_jp_requested = true;
	records = jd ? next.jd_records : next.jp_records;
	if (records != 0 && next.bit_index == 0) {
		if (jd && next.protocol == PCM_PROTOCOL_V92)
			next.stage = PCM_PHASE3_DIGITAL_TX_JP;
		else
			next.stage = PCM_PHASE3_DIGITAL_TX_PRIME;
	}
	if (!pcm_phase3_digital_tx_is_valid(&next))
		return PCM_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
	*transmitter = next;
	return PCM_PHASE3_DIGITAL_TX_OK;
}

enum pcm_phase3_digital_tx_result pcm_phase3_digital_tx_finish_jd(
	struct pcm_phase3_digital_tx *transmitter)
{
	return request_finish(transmitter, PCM_PHASE3_DIGITAL_TX_JD);
}

enum pcm_phase3_digital_tx_result pcm_phase3_digital_tx_finish_jp(
	struct pcm_phase3_digital_tx *transmitter)
{
	return request_finish(transmitter, PCM_PHASE3_DIGITAL_TX_JP);
}

enum pcm_phase3_digital_tx_result pcm_phase3_digital_tx_finish_scr(
	struct pcm_phase3_digital_tx *transmitter)
{
	struct pcm_phase3_digital_tx next;

	if (!pcm_phase3_digital_tx_is_valid(transmitter))
		return PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT;
	if (transmitter->protocol != PCM_PROTOCOL_V92 ||
	    (transmitter->stage != PCM_PHASE3_DIGITAL_TX_SCR &&
	     transmitter->stage != PCM_PHASE3_DIGITAL_TX_COMPLETE))
		return PCM_PHASE3_DIGITAL_TX_INVALID_STATE;
	if (transmitter->stage == PCM_PHASE3_DIGITAL_TX_COMPLETE)
		return PCM_PHASE3_DIGITAL_TX_OK;
	next = *transmitter;
	next.finish_scr_requested = true;
	if (next.scr_symbols != 0 && next.scr_symbols % 6u == 0)
		next.stage = PCM_PHASE3_DIGITAL_TX_COMPLETE;
	if (!pcm_phase3_digital_tx_is_valid(&next))
		return PCM_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
	*transmitter = next;
	return PCM_PHASE3_DIGITAL_TX_OK;
}

const char *pcm_phase3_digital_tx_result_name(
	enum pcm_phase3_digital_tx_result result)
{
	switch (result) {
	case PCM_PHASE3_DIGITAL_TX_OK:
		return "ok";
	case PCM_PHASE3_DIGITAL_TX_DONE:
		return "done";
	case PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT:
		return "invalid argument";
	case PCM_PHASE3_DIGITAL_TX_INVALID_STATE:
		return "invalid state";
	case PCM_PHASE3_DIGITAL_TX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
