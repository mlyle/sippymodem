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

enum pcm_r_tx_result {
	PCM_R_TX_OK,
	PCM_R_TX_COMPLETE,
	PCM_R_TX_INVALID_ARGUMENT,
	PCM_R_TX_INVALID_PARAMETERS,
	PCM_R_TX_INTERNAL_ERROR,
};

/* V.90 8.6.4 Rd/Rd-bar and Rt/Rt-bar use the highest-power member of each
 * current data or training constellation. They are independent of the data
 * encoder: neither signal is scrambled, differentially encoded, or shaped. */
struct pcm_r_tx {
	struct v90_pcm_mapping mapping;
	uint16_t transmitted_symbols;
};

enum pcm_r_tx_result pcm_rd_tx_init(
	struct pcm_r_tx *transmitter,
	const struct v90_pcm_mapping *current_data_mapping);
/* Rt shares the R waveform and duration, but uses CPt constellations. */
enum pcm_r_tx_result pcm_rt_tx_init(
	struct pcm_r_tx *transmitter,
	const struct v90_pcm_mapping *training_mapping);
bool pcm_r_tx_is_valid(const struct pcm_r_tx *transmitter);
bool pcm_r_tx_is_complete(const struct pcm_r_tx *transmitter);

/* Emit exactly 384T of R followed by 24T of R-bar. COMPLETE consumes no
 * character and leaves symbol unchanged. */
enum pcm_r_tx_result pcm_r_tx_next(
	struct pcm_r_tx *transmitter, struct v90_pcm_symbol *symbol);

enum {
	PCM_R_RX_MIN_SYMBOLS = 240,
	PCM_R_RX_BAR_SYMBOLS = 24,
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

/* Detect the digital station's V.90 8.6.4 Rd/Rd-bar signal directly in the
 * authoritative G.711 character stream.  Six phase hypotheses and both
 * polarities avoid assuming where an online monitor first entered the data
 * frame.  Each symbol must be the highest-power member of its current data
 * constellation; unlike a sign-only line detector this rejects unrelated
 * data with the same +++--- sign pattern. */
struct pcm_r_rx {
	struct v90_pcm_mapping mapping;
	uint16_t candidate_runs[PCM_R_RX_HYPOTHESES];
	uint16_t locked_hypotheses;
	enum v90_pcm_mapping_plane decision_plane;
	enum pcm_r_rx_stage stage;
	uint8_t phase;
	uint8_t rd_bar_symbols;
};

enum pcm_r_rx_result pcm_r_rx_init(
	struct pcm_r_rx *receiver,
	const struct v90_pcm_mapping *current_data_mapping,
	enum v90_pcm_mapping_plane decision_plane);
bool pcm_r_rx_is_valid(
	const struct pcm_r_rx *receiver);
bool pcm_r_rx_signal_detected(
	const struct pcm_r_rx *receiver);
bool pcm_r_rx_is_complete(
	const struct pcm_r_rx *receiver);

/* BUFFERING consumes one character. SIGNAL_DETECTED is reported on the 240th
 * consecutive Rd character, BAR_STARTED on the exact frame-boundary
 * polarity reversal, and BAR_COMPLETE on the 24th Rd-bar character. The
 * next character remains unconsumed for the TRN2d receiver.  A post-lock
 * SEQUENCE_MISMATCH consumes that character and restarts the phase/polarity
 * search with it as the first new candidate. */
enum pcm_r_rx_result pcm_r_rx_receive(
	struct pcm_r_rx *receiver, uint8_t network_octet);

const char *pcm_r_rx_result_name(
	enum pcm_r_rx_result result);

#endif /* PCM_R_H */
