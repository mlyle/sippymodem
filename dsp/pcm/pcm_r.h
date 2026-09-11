// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_R_H
#define PCM_R_H

#include "v90_pcm_mapping.h"
#include "v90_pcm_symbol.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	PCM_R_SYMBOLS = 384,
	PCM_R_BAR_SYMBOLS = 24,
	PCM_R_SEQUENCE_SYMBOLS =
		PCM_R_SYMBOLS + PCM_R_BAR_SYMBOLS,
};

enum pcm_r_signal {
	PCM_R_RD,
	PCM_R_RT,
	PCM_R_RF,
};

enum pcm_r_tx_result {
	PCM_R_TX_OK,
	PCM_R_TX_COMPLETE,
	PCM_R_TX_INVALID_ARGUMENT,
	PCM_R_TX_INVALID_PARAMETERS,
	PCM_R_TX_INTERNAL_ERROR,
};

/* V.90 8.6.4 Rd/Rt and V.92 8.8.4 Rf use the highest-power member of
 * each constellation. Rd/Rf require the current data mapping; Rt requires
 * the training mapping. No R signal is scrambled, differentially encoded,
 * or shaped. The caller owns protocol selection and data-frame alignment. */
struct pcm_r_tx {
	struct v90_pcm_mapping mapping;
	enum pcm_r_signal signal;
	uint16_t transmitted_symbols;
};

enum pcm_r_tx_result pcm_r_tx_init(
	struct pcm_r_tx *transmitter,
	const struct v90_pcm_mapping *mapping, enum pcm_r_signal signal);
bool pcm_r_tx_is_valid(const struct pcm_r_tx *transmitter);
bool pcm_r_tx_is_complete(const struct pcm_r_tx *transmitter);

/* Emit exactly 384T of R followed by 24T of R-bar. COMPLETE consumes no
 * character and leaves symbol unchanged. */
enum pcm_r_tx_result pcm_r_tx_next(
	struct pcm_r_tx *transmitter, struct v90_pcm_symbol *symbol);

enum {
	PCM_R_RX_MIN_SYMBOLS = 240,
	PCM_R_RX_BAR_SYMBOLS = PCM_R_BAR_SYMBOLS,
	PCM_R_RX_HYPOTHESES =
		2 * V90_CP_FRAME_INTERVALS,
};

enum pcm_r_rx_stage {
	PCM_R_RX_SEARCH,
	PCM_R_RX_SIGNAL,
	PCM_R_RX_BAR,
	PCM_R_RX_COMPLETE,
};

enum pcm_r_rx_result {
	PCM_R_RX_OK,
	PCM_R_RX_BUFFERING,
	PCM_R_RX_SIGNAL_DETECTED,
	PCM_R_RX_BAR_STARTED,
	PCM_R_RX_BAR_COMPLETE,
	PCM_R_RX_SEQUENCE_MISMATCH,
	PCM_R_RX_INVALID_ARGUMENT,
	PCM_R_RX_INVALID_PARAMETERS,
	PCM_R_RX_INVALID_STATE,
	PCM_R_RX_INTERNAL_ERROR,
};

/* Detect the selected Rd, Rt or Rf directly in authoritative G.711 characters.
 * Six constellation phases and both polarities cover every entry phase:
 * Rf's second six-symbol half is the first half with polarity inverted.
 * Exact constellation magnitudes reject unrelated data with matching signs.
 * Rd/Rf use the data mapping; Rt uses the training mapping during the
 * intervening silent period. Rf is only a V.92 fast-exchange request.
 * R signals do not advance the suspended control decoder's coding history. */
struct pcm_r_rx {
	struct v90_pcm_mapping mapping;
	uint16_t candidate_runs[PCM_R_RX_HYPOTHESES];
	uint16_t locked_hypotheses;
	enum v90_pcm_mapping_plane decision_plane;
	enum pcm_r_signal signal;
	enum pcm_r_rx_stage stage;
	uint8_t phase;
	uint8_t bar_symbols;
};

enum pcm_r_rx_result pcm_r_rx_init(
	struct pcm_r_rx *receiver,
	const struct v90_pcm_mapping *mapping,
	enum v90_pcm_mapping_plane decision_plane, enum pcm_r_signal signal);
bool pcm_r_rx_is_valid(
	const struct pcm_r_rx *receiver);
bool pcm_r_rx_signal_detected(
	const struct pcm_r_rx *receiver);
bool pcm_r_rx_is_complete(
	const struct pcm_r_rx *receiver);

/* BUFFERING consumes one character. SIGNAL_DETECTED is reported on the 240th
 * consecutive R character, BAR_STARTED on the exact frame-boundary
 * polarity reversal, and BAR_COMPLETE on the 24th R-bar character. The
 * next character remains unconsumed for TRN2d (Rd), MP/SUVd (Rt) or SUVd (Rf).
 * A post-lock SEQUENCE_MISMATCH consumes that character and restarts the
 * phase/polarity search with it as the first new candidate. */
enum pcm_r_rx_result pcm_r_rx_receive(
	struct pcm_r_rx *receiver, uint8_t network_octet);

const char *pcm_r_rx_result_name(
	enum pcm_r_rx_result result);

#endif /* PCM_R_H */
