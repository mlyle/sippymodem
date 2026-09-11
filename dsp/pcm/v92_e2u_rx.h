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
	uint16_t bit_errors;
	uint8_t expected_symbols;
	uint8_t received_symbols;
	bool matched;
	bool failed;
};

enum v92_e2u_rx_result v92_e2u_rx_init(
	struct v92_e2u_rx *receiver,
	const struct v92_upstream_rx *signal, bool extend_one_symbol);
bool v92_e2u_rx_is_valid(const struct v92_e2u_rx *receiver);
bool v92_e2u_rx_is_complete(const struct v92_e2u_rx *receiver);
bool v92_e2u_rx_has_failed(const struct v92_e2u_rx *receiver);

enum v92_e2u_rx_result v92_e2u_rx_sample(
	struct v92_e2u_rx *receiver, int16_t sample);

/* Search alongside the continuous control parser. Initialize the candidate
 * at an exact record boundary first; every complete or rejected record rearms
 * it there. The caller validates emitted records against its procedure.
 * COMPLETE consumes the final E2u sample and retains its authoritative line;
 * the speculative control decoder is not committed for that interval.
 * OK consumes one sample and replaces event (NONE unless a record ended).
 * Invalid/terminal calls leave all state and outputs unchanged; processing
 * failure latches receiver->failed without committing control or event.
 * All three objects must be distinct. */
enum v92_e2u_rx_result v92_e2u_rx_search(
	struct v92_e2u_rx *receiver,
	struct v92_upstream_control_line_rx *control, int16_t sample,
	struct v92_control_event *event);

/* Export the exact post-E2u line state. Rejected and premature calls leave
 * signal unchanged. */
enum v92_e2u_rx_result v92_e2u_rx_handoff(
	const struct v92_e2u_rx *receiver,
	struct v92_upstream_rx *signal);

const char *v92_e2u_rx_result_name(enum v92_e2u_rx_result result);

#endif /* V92_E2U_RX_H */
