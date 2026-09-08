// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_PHASE3_DIGITAL_TX_H
#define PCM_PHASE3_DIGITAL_TX_H

#include "pcm_phase3_digital_prefix.h"
#include "pcm_protocol.h"
#include "v90_dil_sequence.h"
#include "v90_jd.h"
#include "v92_j.h"

#include <stdbool.h>
#include <stdint.h>

enum pcm_phase3_digital_tx_stage {
	PCM_PHASE3_DIGITAL_TX_PREFIX,
	PCM_PHASE3_DIGITAL_TX_JD,
	PCM_PHASE3_DIGITAL_TX_JP,
	PCM_PHASE3_DIGITAL_TX_PRIME,
	PCM_PHASE3_DIGITAL_TX_DIL,
	PCM_PHASE3_DIGITAL_TX_SCR,
	PCM_PHASE3_DIGITAL_TX_COMPLETE,
};

enum pcm_phase3_digital_tx_result {
	PCM_PHASE3_DIGITAL_TX_OK,
	PCM_PHASE3_DIGITAL_TX_DONE,
	PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT,
	PCM_PHASE3_DIGITAL_TX_INVALID_STATE,
	PCM_PHASE3_DIGITAL_TX_INTERNAL_ERROR,
};

union pcm_phase3_digital_tx_sequence {
	struct pcm_phase3_digital_prefix prefix;
	struct v90_phase3_signs signs;
};

/* Own the common digital-modem Phase 3 network-character stream after Ja.
 * Both protocols emit Sd, Sd-bar, TRN1d, Jd, and descriptor-selected DIL.
 * V.92 adds Jp and uses SCR when no DIL is requested. The call controller
 * owns remote signal detection and decides when either continuation ends. */
struct pcm_phase3_digital_tx {
	union pcm_phase3_digital_tx_sequence sequence;
	struct v90_dil_sequence dil;
	uint8_t jd_frame[PCM_J_FRAME_BYTES];
	uint8_t jp_frame[PCM_J_FRAME_BYTES];
	enum pcm_protocol protocol;
	enum pcm_phase3_digital_tx_stage stage;
	uint32_t jd_records;
	uint32_t jp_records;
	uint32_t scr_symbols;
	uint8_t bit_index;
	bool finish_jd_requested;
	bool finish_jp_requested;
	bool finish_scr_requested;
};

enum pcm_phase3_digital_tx_result pcm_phase3_digital_tx_init_v90(
	struct pcm_phase3_digital_tx *transmitter, enum g711_law law,
	uint8_t training_ucode,
	const struct v90_jd_parameters *jd_parameters,
	const struct v90_dil_parameters *dil_parameters);
enum pcm_phase3_digital_tx_result pcm_phase3_digital_tx_init_v92(
	struct pcm_phase3_digital_tx *transmitter, enum g711_law law,
	uint8_t training_ucode, const struct v92_jd *jd,
	const struct v92_jp *jp,
	const struct v90_dil_parameters *dil_parameters);
bool pcm_phase3_digital_tx_is_valid(
	const struct pcm_phase3_digital_tx *transmitter);

/* Emit one 8000-symbol/s network character. Rejected calls leave both state
 * and output unchanged. */
enum pcm_phase3_digital_tx_result pcm_phase3_digital_tx_next(
	struct pcm_phase3_digital_tx *transmitter,
	struct v90_pcm_symbol *symbol);

/* Replace only the V.92 Jp Su phase measurement while Jd is active. The
 * constellation selections remain those supplied at initialization. This
 * late binding is required because the digital modem learns the phase from
 * the peer's Su signal after it has already started transmitting Jd. */
enum pcm_phase3_digital_tx_result pcm_phase3_digital_tx_set_v92_su_phase(
	struct pcm_phase3_digital_tx *transmitter,
	uint16_t phase_extension_q16);

/* Latch completion of Jd. At least one complete record is emitted, and a
 * partial record is completed. V.90 advances to the prime suffix; V.92
 * advances to Jp. */
enum pcm_phase3_digital_tx_result pcm_phase3_digital_tx_finish_jd(
	struct pcm_phase3_digital_tx *transmitter);

/* Latch completion of V.92 Jp. V.90 transmitters reject this operation and
 * cannot enter the Jp branch. */
enum pcm_phase3_digital_tx_result pcm_phase3_digital_tx_finish_jp(
	struct pcm_phase3_digital_tx *transmitter);

/* End nonzero DIL on its next segment boundary. A request made at a
 * boundary completes immediately. Zero-DIL selections reject it. */
enum pcm_phase3_digital_tx_result pcm_phase3_digital_tx_finish_dil(
	struct pcm_phase3_digital_tx *transmitter);

/* End V.92 zero-DIL SCR on a six-symbol boundary. A request at the start of
 * SCR still emits one complete six-symbol interval. V.90 cannot enter this
 * branch. */
enum pcm_phase3_digital_tx_result pcm_phase3_digital_tx_finish_scr(
	struct pcm_phase3_digital_tx *transmitter);

const char *pcm_phase3_digital_tx_result_name(
	enum pcm_phase3_digital_tx_result result);

#endif /* PCM_PHASE3_DIGITAL_TX_H */
