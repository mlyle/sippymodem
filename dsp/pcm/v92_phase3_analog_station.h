// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PHASE3_ANALOG_STATION_H
#define V92_PHASE3_ANALOG_STATION_H

#include "v90_phase3_sd_receiver.h"
#include "v90_phase4_ri_search.h"
#include "v92_pcm_phase3_cpt_tx.h"
#include "v92_pcm_phase3_initial_tx.h"
#include "v92_phase3_j_receiver.h"
#include "v92_su.h"

#include <stdbool.h>
#include <stdint.h>

enum v92_phase3_analog_receive_stage {
	V92_PHASE3_ANALOG_RECEIVE_SD,
	V92_PHASE3_ANALOG_RECEIVE_J,
	V92_PHASE3_ANALOG_RECEIVE_RI,
	V92_PHASE3_ANALOG_RECEIVE_COMPLETE,
};

enum v92_phase3_analog_transmit_stage {
	V92_PHASE3_ANALOG_TRANSMIT_INITIAL,
	V92_PHASE3_ANALOG_TRANSMIT_SU,
	V92_PHASE3_ANALOG_TRANSMIT_CPT,
	V92_PHASE3_ANALOG_TRANSMIT_COMPLETE,
};

enum v92_phase3_analog_station_result {
	V92_PHASE3_ANALOG_STATION_OK,
	V92_PHASE3_ANALOG_STATION_COMPLETE,
	V92_PHASE3_ANALOG_STATION_NEED_MD_SYMBOL,
	V92_PHASE3_ANALOG_STATION_FAILED,
	V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT,
	V92_PHASE3_ANALOG_STATION_INVALID_STATE,
	V92_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH,
	V92_PHASE3_ANALOG_STATION_NUMERIC_FAILURE,
};

struct v92_phase3_analog_station_configuration {
	struct v92_pcm_info1a local_info1a;
	struct v92_cpt_parameters cpt;
	uint32_t upstream_rate_mask;
	uint32_t initial_trn1u_symbols;
	enum g711_law law;
	/* Nominal upstream level at call entry, independent of downstream UINFO. */
	uint8_t upstream_lu_ucode;
};

union v92_phase3_analog_receive {
	struct v90_phase3_sd_receiver sd;
	struct v92_phase3_j_receiver j;
	struct v90_phase4_ri_search ri;
};

union v92_phase3_analog_transmit {
	struct v92_pcm_phase3_initial_tx initial;
	struct v92_su su;
	struct v92_pcm_phase3_cpt_tx cpt;
};

struct v92_phase3_analog_transmit_handoff {
	int16_t lu;
	enum v92_constellation_size training_constellation;
	uint8_t previous_encoded_sign;
};

/* Own both directions of the analogue modem's zero-DIL V.92 Phase 3
 * procedure. Raw downstream network characters drive Sd, Jd/Jp, and Ri;
 * signed-linear upstream samples carry the opening, Su, second TRN1u, CPt,
 * and E1u. Calls on one instance are serialized. */
struct v92_phase3_analog_station {
	union v92_phase3_analog_receive receive;
	union v92_phase3_analog_transmit transmit;
	struct fractional_delay transmit_delay;
	struct v92_cpt_parameters cpt_parameters;
	uint32_t upstream_rate_mask;
	enum v92_constellation_size phase4_training_constellation;
	enum v92_constellation_size rate_renegotiation_constellation;
	enum v92_phase3_analog_receive_stage receive_stage;
	enum v92_phase3_analog_transmit_stage transmit_stage;
	enum g711_law law;
	int16_t upstream_lu;
	uint8_t upstream_lu_ucode;
	uint8_t downstream_training_ucode;
	bool failed;
};

bool v92_phase3_analog_station_configuration_is_valid(
	const struct v92_phase3_analog_station_configuration *configuration);
enum v92_phase3_analog_station_result v92_phase3_analog_station_init(
	struct v92_phase3_analog_station *station,
	const struct v92_phase3_analog_station_configuration *configuration);
bool v92_phase3_analog_station_is_valid(
	const struct v92_phase3_analog_station *station);
bool v92_phase3_analog_station_has_failed(
	const struct v92_phase3_analog_station *station);

/* Emit one signed 8 kHz sample. md_symbol is required only during a
 * negotiated manufacturer-defined interval and rejected otherwise.
 * COMPLETE and NEED_MD_SYMBOL consume nothing and preserve sample. */
enum v92_phase3_analog_station_result v92_phase3_analog_station_transmit(
	struct v92_phase3_analog_station *station,
	const struct v90_pcm_symbol *md_symbol, int16_t *sample);

/* Consume one raw G.711 network character. The receiver acquires Ri without
 * assuming that the call owner observed its first character. */
enum v92_phase3_analog_station_result v92_phase3_analog_station_receive(
	struct v92_phase3_analog_station *station, uint8_t network_octet);

bool v92_phase3_analog_station_transmit_complete(
	const struct v92_phase3_analog_station *station);
bool v92_phase3_analog_station_receive_complete(
	const struct v92_phase3_analog_station *station);

/* Export the CPt-selected downstream training map after Ri-bar. The upstream
 * CPt/E1u transmitter may still be finishing independently. */
enum v92_phase3_analog_station_result
v92_phase3_analog_station_downstream_handoff(
	const struct v92_phase3_analog_station *station,
	struct v92_cpt_parameters *parameters);

/* Export the exact level, Jp-selected constellation, and differential-sign
 * continuity for initial Phase 4 TRN2u. Rejected calls leave handoff
 * unchanged. */
enum v92_phase3_analog_station_result
v92_phase3_analog_station_transmit_handoff(
	const struct v92_phase3_analog_station *station,
	struct v92_phase3_analog_transmit_handoff *handoff);

const char *v92_phase3_analog_station_result_name(
	enum v92_phase3_analog_station_result result);

#endif /* V92_PHASE3_ANALOG_STATION_H */
