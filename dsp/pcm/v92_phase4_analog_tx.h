// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PHASE4_ANALOG_TX_H
#define V92_PHASE4_ANALOG_TX_H

#include "v92_cpu.h"
#include "v92_pcm_phase4_trn2u.h"
#include "v92_phase4_record_tx.h"
#include "v92_suvd.h"
#include "v92_suvu.h"

#include <stdbool.h>
#include <stdint.h>

enum v92_phase4_analog_tx_stage {
	V92_PHASE4_ANALOG_TX_TRN2U,
	V92_PHASE4_ANALOG_TX_SUVU,
	V92_PHASE4_ANALOG_TX_CPU,
	V92_PHASE4_ANALOG_TX_E2U_READY,
};

enum v92_phase4_analog_tx_result {
	V92_PHASE4_ANALOG_TX_OK,
	V92_PHASE4_ANALOG_TX_E2U_READY_RESULT,
	V92_PHASE4_ANALOG_TX_INVALID_ARGUMENT,
	V92_PHASE4_ANALOG_TX_INVALID_PARAMETERS,
	V92_PHASE4_ANALOG_TX_INVALID_STATE,
	V92_PHASE4_ANALOG_TX_INTERNAL_ERROR,
};

/* Own the analogue modem's initial Phase 4 transmit procedure through the
 * last CPu/SUVu record. The caller supplies decoded downstream events and
 * owns the round-trip timer; E2u and the data-mode handoff follow separately.
 * One instance is serialized. */
struct v92_phase4_analog_tx {
	union {
		struct v92_pcm_phase4_trn2u trn2u;
		struct v92_phase4_record_tx record;
	} signal;
	struct v92_suvu_parameters suvu;
	struct v92_cpu_parameters cpu;
	enum v92_phase4_analog_tx_stage stage;
	bool received_suvd;
	bool received_cpd;
	/* Either a peer acknowledgment bit or receipt of Ed. */
	bool peer_confirmation_received;
	bool peer_requests_silence;
	bool cpu_pending;
	bool cpu_sent;
	bool acknowledgment_sent;
	bool current_record_acknowledges_cpd;
};

enum v92_phase4_analog_tx_result v92_phase4_analog_tx_init(
	struct v92_phase4_analog_tx *tx,
	enum v92_constellation_size constellation,
	uint8_t previous_encoded_sign, int16_t lu,
	const struct v92_suvu_parameters *suvu,
	const struct v92_cpu_parameters *cpu);
bool v92_phase4_analog_tx_is_valid(
	const struct v92_phase4_analog_tx *tx);
bool v92_phase4_analog_tx_is_e2u_ready(
	const struct v92_phase4_analog_tx *tx);
bool v92_phase4_analog_tx_silence_requested(
	const struct v92_phase4_analog_tx *tx);

/* Accept complete, CRC-checked downstream records. A first SUVd schedules one
 * CPu. CPd parameters themselves belong to the later receive/data handoff. */
enum v92_phase4_analog_tx_result v92_phase4_analog_tx_note_suvd(
	struct v92_phase4_analog_tx *tx,
	const struct v92_suvd_parameters *suvd);
enum v92_phase4_analog_tx_result v92_phase4_analog_tx_note_cpd(
	struct v92_phase4_analog_tx *tx, bool acknowledges_cpu);
enum v92_phase4_analog_tx_result v92_phase4_analog_tx_note_ed(
	struct v92_phase4_analog_tx *tx);

/* The procedure timer may request another single CPu after 100 ms plus the
 * measured round-trip delay. Timing remains outside this symbol owner. */
enum v92_phase4_analog_tx_result v92_phase4_analog_tx_repeat_cpu(
	struct v92_phase4_analog_tx *tx);

/* Emit one 8000-symbol/s signed level. E2U_READY_RESULT consumes no symbol and
 * leaves sample unchanged. Rejected calls are transactional. */
enum v92_phase4_analog_tx_result v92_phase4_analog_tx_next(
	struct v92_phase4_analog_tx *tx, int16_t *sample);

/* Export the exact coding history after the final complete status record. */
enum v92_phase4_analog_tx_result v92_phase4_analog_tx_handoff(
	const struct v92_phase4_analog_tx *tx,
	struct v92_phase4_modulator *modulator);

const char *v92_phase4_analog_tx_result_name(
	enum v92_phase4_analog_tx_result result);

#endif /* V92_PHASE4_ANALOG_TX_H */
