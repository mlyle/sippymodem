// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_JA_H
#define V90_JA_H

#include "v34_framing.h"
#include "v90_dil.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_ja_result {
	V90_JA_OK,
	V90_JA_DETECTED,
	V90_JA_REJECTED,
	V90_JA_UNSUPPORTED,
	V90_JA_COMPLETE,
	V90_JA_INVALID_ARGUMENT,
};

struct v34_training_sequence;
struct qam_point;

/* Ja carries one complete DIL descriptor repeatedly, continuing the analogue
 * modem's live GPA scrambler and differential phase after TRN. */
struct v90_ja_tx {
	struct v34_scrambler scrambler;
	uint8_t descriptor[V90_DIL_MAX_DESCRIPTOR_BYTES];
	uint32_t descriptor_fingerprint;
	uint16_t descriptor_bits;
	uint16_t bit_index;
	uint8_t previous_clockwise_quarter_turns;
};

/* The receive primitive starts at a known TRN-to-J boundary. Acquisition of
 * that boundary from noisy line decisions belongs to the line receiver. */
struct v90_ja_rx {
	struct v34_scrambler descrambler;
	uint8_t descriptor[V90_DIL_ZERO_DESCRIPTOR_BYTES];
	uint16_t bit_index;
	uint8_t previous_clockwise_quarter_turns;
	bool complete;
};

enum v90_ja_result v90_ja_tx_init_from_trn(
	struct v90_ja_tx *tx, const struct v34_training_sequence *trn);
enum v90_ja_result v90_ja_tx_init_dil_from_trn(
	struct v90_ja_tx *tx, const struct v34_training_sequence *trn,
	const struct v90_dil_parameters *parameters);
bool v90_ja_tx_is_valid(const struct v90_ja_tx *tx);
bool v90_ja_tx_at_descriptor_boundary(const struct v90_ja_tx *tx);

/* Bounded invariant used while an enclosing Phase 3 owner retains the
 * deeply validated descriptor created at initialization. The fingerprint
 * makes accidental descriptor mutation observable without decoding it. */
bool v90_ja_tx_owned_state_is_valid(const struct v90_ja_tx *tx);

/* Emit one unit-energy four-point symbol. The owning controller enforces the
 * minimum preceding TRN duration and may stop Ja at any symbol. */
enum v90_ja_result v90_ja_tx_next(
	struct v90_ja_tx *tx, struct qam_point *point,
	uint8_t *clockwise_quarter_turns);
enum v90_ja_result v90_ja_tx_next_owned(
	struct v90_ja_tx *tx, struct qam_point *point,
	uint8_t *clockwise_quarter_turns);

enum v90_ja_result v90_ja_rx_init_from_trn(
	struct v90_ja_rx *rx, const struct v34_training_sequence *trn,
	uint8_t observed_previous_clockwise_quarter_turns);
bool v90_ja_rx_is_valid(const struct v90_ja_rx *rx);
bool v90_ja_rx_is_complete(const struct v90_ja_rx *rx);

/* Consume one hard four-point phase decision. A bad or unsupported complete
 * descriptor is reported and discarded; the continuing scrambler state is
 * retained so a later repetition can still be accepted. */
enum v90_ja_result v90_ja_rx_receive(
	struct v90_ja_rx *rx, uint8_t clockwise_quarter_turns);

#endif /* V90_JA_H */
