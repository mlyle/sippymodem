// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PHASE3_DIGITAL_STATION_H
#define V92_PHASE3_DIGITAL_STATION_H

#include "pcm_phase3_digital_tx.h"
#include "v90_phase4_ri.h"
#include "v92_pcm_phase3_cpt_line_rx.h"
#include "v92_pcm_phase3_initial_rx.h"
#include "v92_su_rx.h"

#include <stdbool.h>
#include <stdint.h>

enum v92_phase3_digital_receive_stage {
	V92_PHASE3_DIGITAL_RECEIVE_INITIAL,
	V92_PHASE3_DIGITAL_RECEIVE_SU_PHASE,
	V92_PHASE3_DIGITAL_RECEIVE_SU_RESPONSE,
	V92_PHASE3_DIGITAL_RECEIVE_WAIT_SECOND_TRN1U,
	V92_PHASE3_DIGITAL_RECEIVE_SECOND_TRN1U,
	V92_PHASE3_DIGITAL_RECEIVE_CPT,
	V92_PHASE3_DIGITAL_RECEIVE_COMPLETE,
};

enum v92_phase3_digital_transmit_stage {
	V92_PHASE3_DIGITAL_TRANSMIT_SILENCE,
	V92_PHASE3_DIGITAL_TRANSMIT_DOWNSTREAM,
	V92_PHASE3_DIGITAL_TRANSMIT_RI,
	V92_PHASE3_DIGITAL_TRANSMIT_COMPLETE,
};

enum v92_phase3_digital_station_result {
	V92_PHASE3_DIGITAL_STATION_OK,
	V92_PHASE3_DIGITAL_STATION_COMPLETE,
	V92_PHASE3_DIGITAL_STATION_FAILED,
	V92_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT,
	V92_PHASE3_DIGITAL_STATION_INVALID_STATE,
	V92_PHASE3_DIGITAL_STATION_SEQUENCE_MISMATCH,
	V92_PHASE3_DIGITAL_STATION_UNSUPPORTED,
	V92_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE,
};

union v92_phase3_digital_receive {
	struct v92_pcm_phase3_initial_rx initial;
	struct v92_su_rx su;
	struct v92_pcm_phase3_trn1u_line_rx trn1u;
	struct v92_pcm_phase3_cpt_line_rx cpt;
};

/* Own both directions of the digital modem's zero-DIL V.92 Phase 3
 * procedure. The common PCM transmitter supplies Sd through SCR, and the
 * existing Ri generator supplies the CPt/E1u boundary; this owner only
 * coordinates their protocol events. Calls on one instance are serialized. */
struct v92_phase3_digital_station {
	union v92_phase3_digital_receive receive;
	struct pcm_phase3_digital_tx downstream;
	struct v90_phase4_ri_tx ri;
	struct v92_jd jd;
	struct v92_jp jp;
	double equalizer_adaptation;
	uint32_t upstream_rate_mask;
	enum v92_phase3_digital_receive_stage receive_stage;
	enum v92_phase3_digital_transmit_stage transmit_stage;
	enum g711_law law;
	uint16_t su_minimum_magnitude;
	uint8_t training_ucode;
	bool failed;
};

bool v92_phase3_digital_station_parameters_are_valid(
	enum g711_law law, uint8_t training_ucode,
	double equalizer_adaptation, uint16_t su_minimum_magnitude,
	const struct v92_jd *jd, const struct v92_jp *jp);
enum v92_phase3_digital_station_result v92_phase3_digital_station_init(
	struct v92_phase3_digital_station *station, enum g711_law law,
	uint8_t training_ucode, double equalizer_adaptation,
	uint16_t su_minimum_magnitude, const struct v92_jd *jd,
	const struct v92_jp *jp);
bool v92_phase3_digital_station_is_valid(
	const struct v92_phase3_digital_station *station);
bool v92_phase3_digital_station_has_failed(
	const struct v92_phase3_digital_station *station);

/* Emit one raw G.711 network character. Before Ja, canonical silence is
 * emitted. COMPLETE consumes no character and preserves the destination. */
enum v92_phase3_digital_station_result v92_phase3_digital_station_transmit(
	struct v92_phase3_digital_station *station, uint8_t *network_octet);

/* Consume one signed 8 kHz sample. Su phase and both TRN1u equalizers are
 * internal continuous stages. A complete CPt record requests Ri-bar; E1u
 * exposes the exact receive handoff. */
enum v92_phase3_digital_station_result v92_phase3_digital_station_receive(
	struct v92_phase3_digital_station *station, int16_t sample);

bool v92_phase3_digital_station_transmit_complete(
	const struct v92_phase3_digital_station *station);
bool v92_phase3_digital_station_receive_complete(
	const struct v92_phase3_digital_station *station);
bool v92_phase3_digital_station_upstream_rate_mask(
	const struct v92_phase3_digital_station *station,
	uint32_t *rate_mask);

/* Export the accepted CPt downstream training map after Ri-bar. E1u may
 * still be traversing the independent upstream receiver. */
enum v92_phase3_digital_station_result
v92_phase3_digital_station_downstream_handoff(
	const struct v92_phase3_digital_station *station,
	struct v92_cpt_parameters *parameters);
enum v92_phase3_digital_station_result
v92_phase3_digital_station_receive_handoff(
	const struct v92_phase3_digital_station *station,
	struct v92_pcm_phase3_cpt_line_handoff *handoff);

const char *v92_phase3_digital_station_result_name(
	enum v92_phase3_digital_station_result result);

#endif /* V92_PHASE3_DIGITAL_STATION_H */
