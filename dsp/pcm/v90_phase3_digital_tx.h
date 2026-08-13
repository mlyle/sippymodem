// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE3_DIGITAL_TX_H
#define V90_PHASE3_DIGITAL_TX_H

#include "pcm_phase3_digital_prefix.h"
#include "v90_jd.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum v90_phase3_digital_tx_stage {
	V90_PHASE3_DIGITAL_TX_PREFIX,
	V90_PHASE3_DIGITAL_TX_JD,
	V90_PHASE3_DIGITAL_TX_JD_PRIME,
	V90_PHASE3_DIGITAL_TX_COMPLETE,
};

enum v90_phase3_digital_tx_result {
	V90_PHASE3_DIGITAL_TX_OK,
	V90_PHASE3_DIGITAL_TX_DONE,
	V90_PHASE3_DIGITAL_TX_INVALID_ARGUMENT,
	V90_PHASE3_DIGITAL_TX_INVALID_STATE,
	V90_PHASE3_DIGITAL_TX_INTERNAL_ERROR,
};

union v90_phase3_digital_tx_sequence {
	struct pcm_phase3_digital_prefix prefix;
	struct v90_phase3_signs signs;
};

/* Owns the digital modem's exact zero-DIL Phase 3 network-character stream
 * after Ja. The shared prefix owns Sd through TRN1d; this V.90 child owns Jd
 * and Jd-prime. The call controller owns the optional pre-Sd delay and
 * detects the remote S signal. */
struct v90_phase3_digital_tx {
	union v90_phase3_digital_tx_sequence sequence;
	uint8_t jd_frame[V90_JD_FRAME_BYTES];
	enum v90_phase3_digital_tx_stage stage;
	uint32_t jd_records;
	uint8_t bit_index;
	bool finish_jd_requested;
};

enum v90_phase3_digital_tx_result v90_phase3_digital_tx_init(
	struct v90_phase3_digital_tx *transmitter, enum g711_law law,
	uint8_t training_ucode,
	const struct v90_jd_parameters *jd_parameters);
bool v90_phase3_digital_tx_is_valid(
	const struct v90_phase3_digital_tx *transmitter);

/* Emit one 8000-symbol/s network character. Sd, Sd-bar, minimum TRN1d, Jd,
 * and Jd-prime advance automatically. Rejected calls leave state and output
 * unchanged. */
enum v90_phase3_digital_tx_result v90_phase3_digital_tx_next(
	struct v90_phase3_digital_tx *transmitter,
	struct v90_pcm_symbol *symbol);

/* Latch remote S detection. The current 72-bit Jd record is completed before
 * the twelve-bit Jd-prime is sent. A request at an existing record boundary
 * advances immediately. */
enum v90_phase3_digital_tx_result v90_phase3_digital_tx_finish_jd(
	struct v90_phase3_digital_tx *transmitter);

const char *v90_phase3_digital_tx_result_name(
	enum v90_phase3_digital_tx_result result);

#endif
