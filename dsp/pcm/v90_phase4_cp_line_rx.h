// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE4_CP_LINE_RX_H
#define V90_PHASE4_CP_LINE_RX_H

#include "v34_data_line_receiver.h"
#include "v90_phase3_return_line_rx.h"
#include "v90_phase4_cp_rx.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_phase4_cp_line_rx_result {
	/* A sample was consumed without producing an equalized symbol. */
	V90_PHASE4_CP_LINE_RX_OK,
	V90_PHASE4_CP_LINE_RX_SYMBOL,
	V90_PHASE4_CP_LINE_RX_RECORD_READY,
	V90_PHASE4_CP_LINE_RX_FAILED,
	V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT,
	V90_PHASE4_CP_LINE_RX_INVALID_STATE,
	V90_PHASE4_CP_LINE_RX_NUMERIC_FAILURE,
};

/* Continue the digital modem's fixed timing and trained equalizer after
 * S-bar. The inherited delay line owns the first CPt points, so the next
 * observation produces the first undelivered CP decision without replay.
 * The S/S-bar phase ambiguity seeds the first differential CP decision. */
struct v90_phase4_cp_line_rx {
	struct v34_line_receiver line;
	struct v34_phase3_s_receiver boundary;
	struct v34_analytic_sample
		boundary_tail[V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS];
	struct v90_phase4_cp_rx symbols;
	struct v34_phase3_profile transmitting_profile;
	struct v90_cp_parameters pending_parameters;
	uint16_t decoder_power_q15;
	uint8_t boundary_tail_count;
	enum v34_adaptive_equalizer_adaptation adaptation;
	bool record_pending;
	bool pending_record_is_valid;
	bool boundary_complete;
	bool failed;
};

/* Import a disjoint fixed handoff. Recover any in-flight CP prefix from its
 * bounded decision history without consuming another physical sample. */
enum v90_phase4_cp_line_rx_result v90_phase4_cp_line_rx_init(
	struct v90_phase4_cp_line_rx *rx,
	const struct v90_phase3_return_line_handoff *handoff,
	enum v34_constellation_size constellation);

/* Preserve the fixed physical receive state after the online S/S-bar request
 * marker and admit the immediately following CP symbols.
 * control_symbols supplies the CP descrambler and differential state retained
 * from the preceding CP-prime. Only the bounded record parser is reset. */
enum v90_phase4_cp_line_rx_result v90_phase4_cp_line_rx_resume_online(
	struct v90_phase4_cp_line_rx *rx,
	const struct v34_data_line_receiver *online,
	const struct v90_phase4_cp_rx *control_symbols,
	enum v34_constellation_size constellation);
bool v90_phase4_cp_line_rx_is_valid(
	const struct v90_phase4_cp_line_rx *rx);
bool v90_phase4_cp_line_rx_has_failed(
	const struct v90_phase4_cp_line_rx *rx);
bool v90_phase4_cp_line_rx_has_pending_record(
	const struct v90_phase4_cp_line_rx *rx);
bool v90_phase4_cp_line_rx_pending_record_is_valid(
	const struct v90_phase4_cp_line_rx *rx);

/* Copy the validated parameters at the paused record boundary. The output
 * changes only on success. */
enum v90_phase4_cp_line_rx_result
v90_phase4_cp_line_rx_pending_parameters(
	const struct v90_phase4_cp_line_rx *rx,
	struct v90_cp_parameters *parameters);

/* A record event pauses input so a controller cannot consume the first symbol
 * of a different section. Continue after accepting or rejecting the record. */
enum v90_phase4_cp_line_rx_result v90_phase4_cp_line_rx_continue(
	struct v90_phase4_cp_line_rx *rx);

/* Consume one signed 8 kHz sample. event changes only for RECORD_READY.
 * Invalid, paused, and failed calls consume nothing. A terminal DSP failure
 * consumes the sample, latches failure, and leaves event unchanged. */
enum v90_phase4_cp_line_rx_result v90_phase4_cp_line_rx_sample(
	struct v90_phase4_cp_line_rx *rx, int16_t sample,
	struct v90_cp_receive_event *event);

const char *v90_phase4_cp_line_rx_result_name(
	enum v90_phase4_cp_line_rx_result result);

#endif /* V90_PHASE4_CP_LINE_RX_H */
