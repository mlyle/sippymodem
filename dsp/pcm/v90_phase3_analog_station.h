// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE3_ANALOG_STATION_H
#define V90_PHASE3_ANALOG_STATION_H

#include "v90_phase3_analog_line_tx.h"
#include "v90_phase3_return_line_tx.h"
#include "pcm_s_pattern_rx.h"
#include "pcm_phase3_j_receiver.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_phase3_analog_transmit_stage {
	V90_PHASE3_ANALOG_TRANSMIT_INITIAL,
	V90_PHASE3_ANALOG_TRANSMIT_RETURN,
	V90_PHASE3_ANALOG_TRANSMIT_DIL_RETURN,
};

enum v90_phase3_analog_receive_stage {
	V90_PHASE3_ANALOG_RECEIVE_SD,
	V90_PHASE3_ANALOG_RECEIVE_SIGNS,
};

enum v90_phase3_analog_station_result {
	V90_PHASE3_ANALOG_STATION_OK,
	V90_PHASE3_ANALOG_STATION_COMPLETE,
	V90_PHASE3_ANALOG_STATION_FAILED,
	V90_PHASE3_ANALOG_STATION_INVALID_ARGUMENT,
	V90_PHASE3_ANALOG_STATION_INVALID_STATE,
	V90_PHASE3_ANALOG_STATION_OUT_OF_RANGE,
	V90_PHASE3_ANALOG_STATION_CLIPPING_RISK,
	V90_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH,
	V90_PHASE3_ANALOG_STATION_NUMERIC_FAILURE,
};

union v90_phase3_analog_station_transmit {
	struct v90_phase3_analog_line_tx initial;
	struct v90_phase3_return_line_tx returning;
};

union v90_phase3_analog_station_receive {
	struct pcm_s_pattern_rx sd;
	struct pcm_phase3_j_receiver signs;
};

/* State needed to continue decoding the digital network-character stream at
 * the first Phase 4 character. */
struct v90_phase3_analog_receive_handoff {
	struct v34_scrambler descrambler;
	struct v90_jd_parameters jd;
	enum g711_law law;
	uint32_t trn1d_symbols;
	uint8_t training_ucode;
	uint8_t last_sign;
};

/* Owns both directions of the analogue modem's Phase 3 exchange. For nonzero
 * DIL it waits through one complete generated traversal, then the same
 * return-line owner emits the required second S/S-bar. The receive and
 * transmit calls may reach Phase 4 at different times; the caller must
 * serialize calls on one instance and hand each direction over independently
 * when its completion predicate becomes true. */
struct v90_phase3_analog_station {
	union v90_phase3_analog_station_transmit transmit;
	union v90_phase3_analog_station_receive receive;
	uint32_t dil_training_symbols;
	uint32_t dil_received_symbols;
	enum v90_phase3_analog_transmit_stage transmit_stage;
	enum v90_phase3_analog_receive_stage receive_stage;
	enum v90_dil_profile dil_profile;
	/* INFO1 training magnitude, retained independently of Sd's W code. */
	uint8_t training_ucode;
	bool failed;
};

enum v90_phase3_analog_station_result v90_phase3_analog_station_init(
	struct v90_phase3_analog_station *station,
	const struct v34_phase3_profile *analog_profile,
	enum g711_law law, uint8_t training_ucode,
	int16_t nominal_peak_amplitude, enum v90_dil_profile dil_profile);
bool v90_phase3_analog_station_is_valid(
	const struct v90_phase3_analog_station *station);
bool v90_phase3_analog_station_owned_state_is_valid(
	const struct v90_phase3_analog_station *station);
bool v90_phase3_analog_station_has_failed(
	const struct v90_phase3_analog_station *station);

/* Produce one 8 kHz analogue sample. COMPLETE consumes no sample and leaves
 * the destination unchanged. Receive controls take effect on the next point
 * that has not yet been accepted by the continuous transmit line. */
enum v90_phase3_analog_station_result v90_phase3_analog_station_transmit(
	struct v90_phase3_analog_station *station, int16_t *sample);

/* Consume one raw G.711 network character. COMPLETE consumes no character.
 * Keeping the octet intact preserves both PCMU zero characters. */
enum v90_phase3_analog_station_result v90_phase3_analog_station_receive(
	struct v90_phase3_analog_station *station, uint8_t network_octet);

bool v90_phase3_analog_station_transmit_complete(
	const struct v90_phase3_analog_station *station);
bool v90_phase3_analog_station_receive_complete(
	const struct v90_phase3_analog_station *station);

enum v90_phase3_analog_station_result
v90_phase3_analog_station_transmit_handoff(
	const struct v90_phase3_analog_station *station,
	struct v34_training_line *line);
bool v90_phase3_analog_receive_handoff_is_valid(
	const struct v90_phase3_analog_receive_handoff *handoff);
enum v90_phase3_analog_station_result
v90_phase3_analog_station_receive_handoff(
	const struct v90_phase3_analog_station *station,
	struct v90_phase3_analog_receive_handoff *handoff);

const char *v90_phase3_analog_station_result_name(
	enum v90_phase3_analog_station_result result);

#endif /* V90_PHASE3_ANALOG_STATION_H */
