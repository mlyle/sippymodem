// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE4_DIGITAL_STATION_H
#define V90_PHASE4_DIGITAL_STATION_H

#include "v90_jd.h"
#include "v90_pcm_mapping.h"
#include "v90_phase4_cp_line_rx.h"
#include "v90_phase4_ri.h"
#include "pcm_downstream_tx.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_phase4_digital_station_result {
	V90_PHASE4_DIGITAL_STATION_OK,
	V90_PHASE4_DIGITAL_STATION_COMPLETE,
	V90_PHASE4_DIGITAL_STATION_FAILED,
	V90_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT,
	V90_PHASE4_DIGITAL_STATION_INVALID_STATE,
	V90_PHASE4_DIGITAL_STATION_NUMERIC_FAILURE,
};

/* Ready-to-run TRN2d/MP/Ed owner with every coder history initialized to zero
 * at the exact boundary following the final Ri-bar character. */
struct v90_phase4_digital_transmit_handoff {
	struct pcm_downstream_tx transmitter;
};

/* Own the digital modem's initial Phase 4 exchange. Ri begins immediately;
 * the inherited Phase 3 carrier/equalizer receives CPt concurrently. A valid
 * initial CPt retains its upstream capability mask and requests the
 * record-aligned Ri-to-Ri-bar transition. Receive and transmit calls may
 * complete independently, but calls on one instance are serialized. */
struct v90_phase4_digital_station {
	struct v90_phase4_cp_line_rx receive;
	struct v90_phase4_ri_tx transmit;
	struct v90_pcm_mapping training_mapping;
	struct v34_precoder_recommendation precoder;
	uint16_t peer_upstream_rate_mask;
	uint8_t maximum_spectral_lookahead;
	bool cpt_received;
	bool failed;
};

/* The destination must be disjoint from the fixed receive handoff. */
enum v90_phase4_digital_station_result v90_phase4_digital_station_init(
	struct v90_phase4_digital_station *station,
	const struct v90_phase3_return_line_handoff *phase3_receive,
	const struct v90_jd_parameters *jd, enum g711_law law,
	uint8_t training_ucode);

/* Join a Phase 3 receive handoff to a Phase 4 Ri transmitter that has already
 * produced the intervening network characters. A finish request is rejected:
 * it can only be caused by CPt, whose receiver starts at this boundary.
 * The destination must be disjoint from the fixed receive handoff. */
enum v90_phase4_digital_station_result v90_phase4_digital_station_resume(
	struct v90_phase4_digital_station *station,
	const struct v90_phase3_return_line_handoff *phase3_receive,
	const struct v90_jd_parameters *jd,
	const struct v90_phase4_ri_tx *transmit);
bool v90_phase4_digital_station_is_valid(
	const struct v90_phase4_digital_station *station);
bool v90_phase4_digital_station_has_failed(
	const struct v90_phase4_digital_station *station);

/* Produce one raw 8 ksymbol/s G.711 character. The final Ri-bar character is
 * returned normally; a later call returns COMPLETE without changing output. */
enum v90_phase4_digital_station_result v90_phase4_digital_station_transmit(
	struct v90_phase4_digital_station *station, uint8_t *network_octet);

/* Consume one signed 8 kHz sample. Invalid CP records are discarded at their
 * exact record boundary and acquisition continues without losing a sample. */
enum v90_phase4_digital_station_result v90_phase4_digital_station_receive(
	struct v90_phase4_digital_station *station, int16_t sample);

bool v90_phase4_digital_station_transmit_complete(
	const struct v90_phase4_digital_station *station);
bool v90_phase4_digital_station_receive_complete(
	const struct v90_phase4_digital_station *station);

enum v90_phase4_digital_station_result
v90_phase4_digital_station_transmit_handoff(
	const struct v90_phase4_digital_station *station,
	const struct v90_mp_record *mp,
	struct v90_phase4_digital_transmit_handoff *handoff);

/* Preserve the equalizer and CP scrambler/differential histories so the next
 * owner can receive CP without replaying the accepted CPt boundary. */
enum v90_phase4_digital_station_result
v90_phase4_digital_station_receive_handoff(
	const struct v90_phase4_digital_station *station,
	struct v90_phase4_cp_line_rx *receive);

bool v90_phase4_digital_transmit_handoff_is_valid(
	const struct v90_phase4_digital_transmit_handoff *handoff);
const char *v90_phase4_digital_station_result_name(
	enum v90_phase4_digital_station_result result);

#endif /* V90_PHASE4_DIGITAL_STATION_H */
