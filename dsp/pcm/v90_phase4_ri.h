// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE4_RI_H
#define V90_PHASE4_RI_H

#include "g711.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V90_PHASE4_RI_PERIOD = 6,
	V90_PHASE4_RI_MIN_SYMBOLS = 192,
	V90_PHASE4_RI_BAR_SYMBOLS = 24,
};

enum v90_phase4_ri_stage {
	V90_PHASE4_RI,
	V90_PHASE4_RI_BAR,
	V90_PHASE4_RI_COMPLETE,
};

enum v90_phase4_ri_result {
	V90_PHASE4_RI_OK,
	V90_PHASE4_RI_TRANSITION_DETECTED,
	V90_PHASE4_RI_DONE,
	V90_PHASE4_RI_INVALID_ARGUMENT,
	V90_PHASE4_RI_INVALID_STATE,
	V90_PHASE4_RI_INVALID_SEQUENCE,
};

/* Generate the digital modem's Phase 4 Ri signal as raw G.711 network
 * characters. A finish request is deferred until both the 192T minimum and
 * the end of the current six-character Ri period, then exactly 24T of Ri-bar
 * is emitted. */
struct v90_phase4_ri_tx {
	enum v90_phase4_ri_stage stage;
	enum g711_law law;
	uint16_t ri_symbols;
	uint8_t ri_bar_symbols;
	uint8_t phase;
	uint8_t training_ucode;
	bool finish_requested;
};

enum v90_phase4_ri_result v90_phase4_ri_tx_init(
	struct v90_phase4_ri_tx *tx, enum g711_law law,
	uint8_t training_ucode);
bool v90_phase4_ri_tx_is_valid(const struct v90_phase4_ri_tx *tx);
bool v90_phase4_ri_tx_is_complete(const struct v90_phase4_ri_tx *tx);
enum v90_phase4_ri_result v90_phase4_ri_tx_request_finish(
	struct v90_phase4_ri_tx *tx);
/* A generated character always returns OK, including the final Ri-bar
 * character. A later call returns DONE without changing the destination. */
enum v90_phase4_ri_result v90_phase4_ri_tx_next(
	struct v90_phase4_ri_tx *tx, uint8_t *network_octet);

/* Detect the corresponding Ri-to-Ri-bar transition at the raw network
 * character boundary. The first sign establishes the possible whole-signal
 * polarity reversal; all following signs and magnitudes are exact. */
struct v90_phase4_ri_rx {
	enum v90_phase4_ri_stage stage;
	enum g711_law law;
	uint16_t ri_symbols;
	uint8_t ri_bar_symbols;
	uint8_t phase;
	uint8_t training_ucode;
	bool polarity_known;
	bool inverted;
};

enum v90_phase4_ri_result v90_phase4_ri_rx_init(
	struct v90_phase4_ri_rx *rx, enum g711_law law,
	uint8_t training_ucode);
bool v90_phase4_ri_rx_is_valid(const struct v90_phase4_ri_rx *rx);
bool v90_phase4_ri_rx_is_complete(const struct v90_phase4_ri_rx *rx);
enum v90_phase4_ri_result v90_phase4_ri_rx_receive(
	struct v90_phase4_ri_rx *rx, uint8_t network_octet);

const char *v90_phase4_ri_result_name(enum v90_phase4_ri_result result);

#endif /* V90_PHASE4_RI_H */
