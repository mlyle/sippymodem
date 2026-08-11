// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE4_J_PRIME_SEQUENCE_H
#define V34_PHASE4_J_PRIME_SEQUENCE_H

#include "qam.h"
#include "v34_framing.h"
#include "v34_station.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_PHASE4_J_PRIME_BITS = 16,
	V34_PHASE4_J_PRIME_SYMBOLS = 8,
};

enum v34_phase4_j_prime_sequence_result {
	V34_PHASE4_J_PRIME_SEQUENCE_OK,
	V34_PHASE4_J_PRIME_SEQUENCE_COMPLETE,
	V34_PHASE4_J_PRIME_SEQUENCE_INVALID_ARGUMENT,
};

struct v34_phase3_j_sequence;

struct v34_phase4_j_prime_sequence {
	struct v34_scrambler scrambler;
	enum v34_station_role role;
	uint8_t previous_clockwise_quarter_turns;
	uint8_t emitted_symbols;
};

/* Start the calling station's one-pattern J-prime sequence at a J pattern
 * boundary. The startup controller first waits for the required received
 * S/S-bar event. */
enum v34_phase4_j_prime_sequence_result
v34_phase4_j_prime_sequence_init_from_j(
	struct v34_phase4_j_prime_sequence *sequence,
	const struct v34_phase3_j_sequence *j_sequence);

bool v34_phase4_j_prime_sequence_is_valid(
	const struct v34_phase4_j_prime_sequence *sequence);
bool v34_phase4_j_prime_sequence_is_complete(
	const struct v34_phase4_j_prime_sequence *sequence);

/* Emit one unit-energy symbol. Completion leaves the following Phase 4 TRN
 * selection and its freshly initialized scrambler to the startup controller. */
enum v34_phase4_j_prime_sequence_result
v34_phase4_j_prime_sequence_next(
	struct v34_phase4_j_prime_sequence *sequence,
	struct sippymodem_qam_point *point,
	uint8_t *clockwise_quarter_turns);

const char *v34_phase4_j_prime_sequence_result_name(
	enum v34_phase4_j_prime_sequence_result result);

#endif
