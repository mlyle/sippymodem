// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_H
#define SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_H

#include "sippymodem_qam.h"
#include "sippymodem_v34_framing.h"
#include "sippymodem_v34_station.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_PHASE4_J_PRIME_BITS = 16,
	SIPPYMODEM_V34_PHASE4_J_PRIME_SYMBOLS = 8,
};

enum sippymodem_v34_phase4_j_prime_sequence_result {
	SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_OK,
	SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_COMPLETE,
	SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_INVALID_ARGUMENT,
};

struct sippymodem_v34_phase3_j_sequence;

struct sippymodem_v34_phase4_j_prime_sequence {
	struct sippymodem_v34_scrambler scrambler;
	enum sippymodem_v34_station_role role;
	uint8_t previous_clockwise_quarter_turns;
	uint8_t emitted_symbols;
};

/* Start the calling station's one-pattern J-prime sequence at a J pattern
 * boundary. The startup controller first waits for the required received
 * S/S-bar event. */
enum sippymodem_v34_phase4_j_prime_sequence_result
sippymodem_v34_phase4_j_prime_sequence_init_from_j(
	struct sippymodem_v34_phase4_j_prime_sequence *sequence,
	const struct sippymodem_v34_phase3_j_sequence *j_sequence);

bool sippymodem_v34_phase4_j_prime_sequence_is_valid(
	const struct sippymodem_v34_phase4_j_prime_sequence *sequence);
bool sippymodem_v34_phase4_j_prime_sequence_is_complete(
	const struct sippymodem_v34_phase4_j_prime_sequence *sequence);

/* Emit one unit-energy symbol. Completion leaves the following Phase 4 TRN
 * selection and its freshly initialized scrambler to the startup controller. */
enum sippymodem_v34_phase4_j_prime_sequence_result
sippymodem_v34_phase4_j_prime_sequence_next(
	struct sippymodem_v34_phase4_j_prime_sequence *sequence,
	struct sippymodem_qam_point *point,
	uint8_t *clockwise_quarter_turns);

const char *sippymodem_v34_phase4_j_prime_sequence_result_name(
	enum sippymodem_v34_phase4_j_prime_sequence_result result);

#endif
