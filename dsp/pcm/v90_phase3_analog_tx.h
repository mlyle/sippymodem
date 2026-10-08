// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE3_ANALOG_TX_H
#define V90_PHASE3_ANALOG_TX_H

#include "v34_phase3_transmitter.h"
#include "v90_ja.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V90_PHASE3_JA_DESCRIPTOR_SYMBOLS =
		V90_DIL_ZERO_DESCRIPTOR_BITS / 2,
};

enum v90_phase3_analog_tx_stage {
	V90_PHASE3_ANALOG_TX_PREFIX,
	V90_PHASE3_ANALOG_TX_JA,
	V90_PHASE3_ANALOG_TX_COMPLETE,
};

enum v90_phase3_analog_tx_result {
	V90_PHASE3_ANALOG_TX_OK,
	V90_PHASE3_ANALOG_TX_DONE,
	V90_PHASE3_ANALOG_TX_INVALID_ARGUMENT,
	V90_PHASE3_ANALOG_TX_INVALID_STATE,
	V90_PHASE3_ANALOG_TX_INTERNAL_ERROR,
};

union v90_phase3_analog_tx_sequence {
	struct v34_phase3_transmitter prefix;
	struct v90_ja_tx ja;
};

/* Own the analogue modem's zero-MD point stream after its required silence:
 * S/S-bar, PP, open-ended TRN, and repeated zero-DIL Ja descriptors. */
struct v90_phase3_analog_tx {
	union v90_phase3_analog_tx_sequence sequence;
	enum v90_phase3_analog_tx_stage stage;
	uint32_t ja_symbols;
};

enum v90_phase3_analog_tx_result v90_phase3_analog_tx_init(
	struct v90_phase3_analog_tx *tx);
bool v90_phase3_analog_tx_is_valid(
	const struct v90_phase3_analog_tx *tx);
bool v90_phase3_analog_tx_owned_state_is_valid(
	const struct v90_phase3_analog_tx *tx);

/* S/S-bar and PP advance automatically. TRN and Ja remain open-ended until
 * their corresponding control call succeeds. */
enum v90_phase3_analog_tx_result v90_phase3_analog_tx_next(
	struct v90_phase3_analog_tx *tx, struct qam_point *point);
enum v90_phase3_analog_tx_result v90_phase3_analog_tx_start_ja(
	struct v90_phase3_analog_tx *tx);
enum v90_phase3_analog_tx_result v90_phase3_analog_tx_start_dil_ja(
	struct v90_phase3_analog_tx *tx,
	const struct v90_dil_parameters *parameters);

/* Ja may end between descriptor boundaries, but only after one complete
 * descriptor has made the selected DIL request available to the peer. */
enum v90_phase3_analog_tx_result v90_phase3_analog_tx_finish_ja(
	struct v90_phase3_analog_tx *tx);

#endif /* V90_PHASE3_ANALOG_TX_H */
