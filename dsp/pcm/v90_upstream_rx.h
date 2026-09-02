// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_UPSTREAM_RX_H
#define V90_UPSTREAM_RX_H

#include "v90_phase4_e_line_rx.h"
#include "v90_pcm_mapping.h"
#include "v90_upstream_data.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_upstream_rx_stage {
	V90_UPSTREAM_RX_STAGE_CP,
	V90_UPSTREAM_RX_STAGE_E,
	V90_UPSTREAM_RX_STAGE_B1,
	V90_UPSTREAM_RX_STAGE_COMPLETE,
};

enum v90_upstream_rx_result {
	V90_UPSTREAM_RX_OK,
	/* The first final CP authorizes MP prime. */
	V90_UPSTREAM_RX_REQUEST_MP_PRIME,
	/* CP prime authorizes Ed and starts continuous E reception. */
	V90_UPSTREAM_RX_REQUEST_ED,
	V90_UPSTREAM_RX_E_COMPLETE,
	V90_UPSTREAM_RX_B1_COMPLETE,
	V90_UPSTREAM_RX_COMPLETE,
	V90_UPSTREAM_RX_FAILED,
	V90_UPSTREAM_RX_SEQUENCE_MISMATCH,
	V90_UPSTREAM_RX_INVALID_ARGUMENT,
	V90_UPSTREAM_RX_INVALID_STATE,
	V90_UPSTREAM_RX_INVALID_PARAMETERS,
	V90_UPSTREAM_RX_NO_COMMON_RATE,
	V90_UPSTREAM_RX_OUT_OF_RANGE,
	V90_UPSTREAM_RX_NUMERIC_FAILURE,
};

union v90_upstream_rx_state {
	struct v90_phase4_cp_line_rx cp;
	struct v90_phase4_e_line_rx e;
	struct v34_data_line_receiver data;
};

/* Own the digital modem's continuous conventional-upstream receive path from
 * the initial CP handoff through E and B1. Record events pause internally so a
 * controller can request MP prime and Ed at their exact boundaries without
 * exposing a replayable half-transition. */
struct v90_upstream_rx {
	union v90_upstream_rx_state state;
	/* CP modulation is initialized only before the first CPt. Preserve its
	 * self-synchronizing scrambler and differential phase after CP-prime so a
	 * later rate renegotiation can reset only the frame parser. */
	struct v90_phase4_cp_rx control_symbols;
	struct v90_pcm_mapping training_mapping;
	struct v90_cp_parameters data_parameters;
	struct v90_mp_record local_mp;
	/* Data CP rate indices admitted by the Jd sent on this call. */
	uint32_t enabled_downstream_rate_mask;
	enum v90_upstream_rx_stage stage;
	bool data_cp_received;
	bool failed;
};

enum v90_upstream_rx_result v90_upstream_rx_init(
	struct v90_upstream_rx *receiver,
	const struct v90_phase4_cp_line_rx *cp,
	const struct v90_pcm_mapping *training_mapping,
	const struct v90_mp_record *local_mp,
	uint32_t enabled_downstream_rate_mask);
bool v90_upstream_rx_is_valid(const struct v90_upstream_rx *receiver);
bool v90_upstream_rx_has_failed(const struct v90_upstream_rx *receiver);
bool v90_upstream_rx_is_complete(const struct v90_upstream_rx *receiver);

/* Consume one signed 8 kHz line sample. event changes only for the two
 * control requests. All other results leave event unchanged. A failure after
 * consumption is latched because carrier and equalizer history cannot be
 * replayed. */
enum v90_upstream_rx_result v90_upstream_rx_sample(
	struct v90_upstream_rx *receiver, int16_t sample,
	struct v90_cp_parameters *event);

/* Export the continuous V.34 data receiver immediately after B1. */
enum v90_upstream_rx_result v90_upstream_rx_handoff(
	const struct v90_upstream_rx *receiver,
	struct v34_data_line_receiver *data_receiver);

/* Start conventional upstream B1/data reception after the analogue modem's
 * E sequence. MP is the record sent by this digital modem. */
enum v90_upstream_data_result v90_upstream_rx_start_after_e(
	struct v34_data_line_receiver *receiver,
	const struct v90_phase4_e_line_rx *completed_e,
	const struct v90_mp_record *mp);

const char *v90_upstream_rx_result_name(
	enum v90_upstream_rx_result result);

#endif /* V90_UPSTREAM_RX_H */
