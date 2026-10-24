// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_E2U_RX_H
#define V92_E2U_RX_H

#include "v92_upstream_rx.h"
#include "v92_upstream_control_line_rx.h"

#include <stdbool.h>
#include <stdint.h>

enum v92_e2u_rx_result {
	V92_E2U_RX_OK,
	V92_E2U_RX_COMPLETE,
	V92_E2U_RX_NO_MATCH,
	V92_E2U_RX_FAILED,
	V92_E2U_RX_INVALID_ARGUMENT,
	V92_E2U_RX_INVALID_STATE,
	V92_E2U_RX_NUMERIC_FAILURE,
};

/* Receive the one-frame E2u zero sequence at an exact upstream record
 * boundary. The inherited line remains continuous, and a successful handoff
 * owns the first B1u decision. A terminal non-match is distinct from a DSP
 * failure so a higher-level boundary search can discard a trial cleanly. */
struct v92_e2u_rx {
	struct v92_upstream_rx signal;
	double correlation;
	double observed_energy;
	double expected_energy;
	/* Raw carrier evidence follows the learned FIR input tap throughout
	 * this marker, including taps whose oldest samples leave its history. */
	double carrier_energy, carrier_peak;
	/* Largest nominal-level residual and its contribution to the three
	 * correlation sums. This bounded evidence belongs to one marker trial. */
	struct {
		double squared_error, correlation, observed_energy, expected_energy;
	} outlier;
	uint16_t bit_errors;
	uint8_t expected_symbols;
	uint8_t received_symbols;
	uint8_t carrier_tap;
	bool matched;
	bool failed;
};

/* Optional repeat evidence has one owner per Phase 4 exchange. A validated
 * nonzero CPu supplies the exact wire bits and an independent coder.
 * The second marker trial never replaces the ordinary receive-history trial. */
struct v92_e2u_search {
	struct v92_e2u_rx repeated;
	struct v92_phase4_modulator coder;
	uint8_t frame[V92_CPU_MAX_FRAME_BYTES];
	uint16_t frame_bits;
	uint16_t bit_index;
	bool candidate_active;
};

/* Zero initialization starts an exchange; copy this state at a live handoff.
 * Discard it on retrain/reentry. No pointers or process state are retained.
 * All four arguments are distinct owned objects; the native search contract
 * below applies, including eligibility and no mutation on rejected calls. */
bool v92_e2u_search_is_valid(const struct v92_e2u_search *search);
enum v92_e2u_rx_result v92_e2u_search_sample(
	struct v92_e2u_search *search, struct v92_e2u_rx *receiver,
	struct v92_upstream_control_line_rx *control, bool eligible, int16_t sample,
	struct v92_control_event *event);

enum v92_e2u_rx_result v92_e2u_rx_init(
	struct v92_e2u_rx *receiver,
	const struct v92_upstream_rx *signal, bool extend_one_symbol);
bool v92_e2u_rx_is_valid(const struct v92_e2u_rx *receiver);
bool v92_e2u_rx_is_complete(const struct v92_e2u_rx *receiver);
bool v92_e2u_rx_has_failed(const struct v92_e2u_rx *receiver);

enum v92_e2u_rx_result v92_e2u_rx_sample(
	struct v92_e2u_rx *receiver, int16_t sample);

/* Search alongside the continuous control parser. A complete record boundary
 * or a confirmed repetition boundary after lost framing arms a candidate.
 * A mid-record handoff continues parsing first. Recovered timing does not
 * admit damaged control records or change negotiated parameters.
 * eligible must remain false until the peer could have received and answered
 * our acknowledgement. Ineligible samples discard any candidate but still
 * advance control. The caller validates emitted records against its procedure.
 * COMPLETE consumes the final E2u sample and retains its authoritative line;
 * the speculative control decoder is not committed for that interval.
 * OK consumes one sample and replaces event (NONE unless a record ended).
 * Invalid/terminal calls leave all state and outputs unchanged; processing
 * failure latches receiver->failed without committing control or event.
 * All three objects must be distinct. */
enum v92_e2u_rx_result v92_e2u_rx_search(
	struct v92_e2u_rx *receiver,
	struct v92_upstream_control_line_rx *control, bool eligible, int16_t sample,
	struct v92_control_event *event);

/* Export the exact post-E2u line state. Rejected and premature calls leave
 * signal unchanged. */
enum v92_e2u_rx_result v92_e2u_rx_handoff(
	const struct v92_e2u_rx *receiver,
	struct v92_upstream_rx *signal);

const char *v92_e2u_rx_result_name(enum v92_e2u_rx_result result);

#endif /* V92_E2U_RX_H */
