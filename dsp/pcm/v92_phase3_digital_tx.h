// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PHASE3_DIGITAL_TX_H
#define V92_PHASE3_DIGITAL_TX_H

#include "pcm_phase3_digital_prefix.h"
#include "v92_j.h"

#include <stdbool.h>
#include <stdint.h>

enum v92_phase3_digital_tx_stage {
	V92_PHASE3_DIGITAL_TX_PREFIX,
	V92_PHASE3_DIGITAL_TX_JD,
	V92_PHASE3_DIGITAL_TX_JP,
	V92_PHASE3_DIGITAL_TX_JP_PRIME,
	V92_PHASE3_DIGITAL_TX_COMPLETE,
};

enum v92_phase3_digital_tx_result {
	V92_PHASE3_DIGITAL_TX_OK,
	V92_PHASE3_DIGITAL_TX_DONE,
	V92_PHASE3_DIGITAL_TX_INVALID_ARGUMENT,
	V92_PHASE3_DIGITAL_TX_INVALID_STATE,
	V92_PHASE3_DIGITAL_TX_INTERNAL_ERROR,
};

union v92_phase3_digital_tx_sequence {
	struct pcm_phase3_digital_prefix prefix;
	struct v90_phase3_signs signs;
};

/* Owns the digital modem's V.92 Phase 3 network-character stream after Ja.
 * The shared prefix emits Sd through the minimum TRN1d. This child then emits
 * complete Jd and Jp records followed by the twelve-bit Jp-prime. The call
 * controller owns remote signal detection and the following DIL/SCR stage. */
struct v92_phase3_digital_tx {
	union v92_phase3_digital_tx_sequence sequence;
	uint8_t jd_frame[V92_J_FRAME_BYTES];
	uint8_t jp_frame[V92_J_FRAME_BYTES];
	enum v92_phase3_digital_tx_stage stage;
	uint32_t jd_records;
	uint32_t jp_records;
	uint8_t bit_index;
	bool finish_jd_requested;
	bool finish_jp_requested;
};

enum v92_phase3_digital_tx_result v92_phase3_digital_tx_init(
	struct v92_phase3_digital_tx *transmitter, enum g711_law law,
	uint8_t training_ucode, const struct v92_jd *jd,
	const struct v92_jp *jp);
bool v92_phase3_digital_tx_is_valid(
	const struct v92_phase3_digital_tx *transmitter);

/* Emit one 8000-symbol/s network character. Rejected calls leave both state
 * and output unchanged. */
enum v92_phase3_digital_tx_result v92_phase3_digital_tx_next(
	struct v92_phase3_digital_tx *transmitter,
	struct v90_pcm_symbol *symbol);

/* Latch completion of phase determination. At least one complete Jd record is
 * emitted, and a partial record is completed, before Jp starts. */
enum v92_phase3_digital_tx_result v92_phase3_digital_tx_finish_jd(
	struct v92_phase3_digital_tx *transmitter);

/* Latch the later Su-to-Su-bar transition. At least one complete Jp record is
 * emitted, and a partial record is completed, before Jp-prime starts. */
enum v92_phase3_digital_tx_result v92_phase3_digital_tx_finish_jp(
	struct v92_phase3_digital_tx *transmitter);

const char *v92_phase3_digital_tx_result_name(
	enum v92_phase3_digital_tx_result result);

#endif /* V92_PHASE3_DIGITAL_TX_H */
