// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE3_DIGITAL_STATION_H
#define V90_PHASE3_DIGITAL_STATION_H

#include "v90_phase3_analog_line_rx.h"
#include "pcm_phase3_digital_tx.h"
#include "v90_phase3_return_line_rx.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_phase3_digital_receive_stage {
	V90_PHASE3_DIGITAL_RECEIVE_INITIAL,
	V90_PHASE3_DIGITAL_RECEIVE_RETURN,
	V90_PHASE3_DIGITAL_RECEIVE_DIL_RETURN,
};

enum {
	/* Leave the peer a 20-ms settling interval after Ja before Sd. */
	V90_PHASE3_DIGITAL_POST_JA_SILENCE_SYMBOLS = 160,
};

enum v90_phase3_digital_station_result {
	V90_PHASE3_DIGITAL_STATION_OK,
	V90_PHASE3_DIGITAL_STATION_COMPLETE,
	V90_PHASE3_DIGITAL_STATION_FAILED,
	V90_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT,
	V90_PHASE3_DIGITAL_STATION_INVALID_STATE,
	V90_PHASE3_DIGITAL_STATION_OUT_OF_RANGE,
	V90_PHASE3_DIGITAL_STATION_SEQUENCE_MISMATCH,
	V90_PHASE3_DIGITAL_STATION_NUMERIC_FAILURE,
};

union v90_phase3_digital_station_receive {
	struct v90_phase3_analog_line_rx initial;
	struct v90_phase3_return_line_rx returning;
};

/* State needed to continue generating the digital Phase 4 network-character
 * stream immediately after Jd-prime. */
struct v90_phase3_digital_transmit_handoff {
	struct v34_scrambler scrambler;
	struct v90_jd_parameters jd;
	enum g711_law law;
	uint32_t trn1d_symbols;
	uint8_t training_ucode;
	uint8_t last_sign;
};

/* Owns both directions of the digital modem's Phase 3 exchange. Network
 * transmission remains silent until a valid Ja descriptor is received, then
 * leaves a short settling interval before Sd. A nonzero DIL rearms the same
 * trained line receiver for the second S/S-bar. Calls on one instance must be
 * serialized; transmit and receive reach independent Phase 4 boundaries. */
struct v90_phase3_digital_station {
	union v90_phase3_digital_station_receive receive;
	struct pcm_phase3_digital_tx transmit;
	struct v90_jd_parameters jd;
	enum v90_phase3_digital_receive_stage receive_stage;
	uint16_t post_ja_silence_symbols_remaining;
	enum g711_law law;
	uint8_t training_ucode;
	bool transmit_started;
	bool failed;
};

enum v90_phase3_digital_station_result v90_phase3_digital_station_init(
	struct v90_phase3_digital_station *station,
	const struct v34_phase3_profile *analog_profile,
	int16_t initial_receive_gain, enum g711_law law, uint8_t training_ucode,
	const struct v90_jd_parameters *jd);
bool v90_phase3_digital_station_is_valid(
	const struct v90_phase3_digital_station *station);
bool v90_phase3_digital_station_has_failed(
	const struct v90_phase3_digital_station *station);

/* Produce one raw 8 ksymbol/s G.711 network character. Before Ja this is the
 * law's canonical silence character. COMPLETE produces no character. */
enum v90_phase3_digital_station_result v90_phase3_digital_station_transmit(
	struct v90_phase3_digital_station *station, uint8_t *network_octet);

/* Consume one signed 8 kHz sample. The first S terminates the current Jd
 * record. With nonzero DIL, the second S requests a segment-boundary stop;
 * its complete S-bar exposes the Phase 4 line handoff. */
enum v90_phase3_digital_station_result v90_phase3_digital_station_receive(
	struct v90_phase3_digital_station *station, int16_t sample);

/* Consume a conventional physical sample in the fixed upstream receiver after
 * the final return completes while the independent DIL transmitter reaches its
 * segment boundary. The completed return marker is frozen. The ordinary
 * receive entry point continues to reject calls after completion. */
enum v90_phase3_digital_station_result
v90_phase3_digital_station_advance_completed_receive(
	struct v90_phase3_digital_station *station, int16_t sample);

bool v90_phase3_digital_station_transmit_complete(
	const struct v90_phase3_digital_station *station);
bool v90_phase3_digital_station_receive_complete(
	const struct v90_phase3_digital_station *station);

bool v90_phase3_digital_transmit_handoff_is_valid(
	const struct v90_phase3_digital_transmit_handoff *handoff);
enum v90_phase3_digital_station_result
v90_phase3_digital_station_transmit_handoff(
	const struct v90_phase3_digital_station *station,
	struct v90_phase3_digital_transmit_handoff *handoff);
enum v90_phase3_digital_station_result
v90_phase3_digital_station_receive_handoff(
	const struct v90_phase3_digital_station *station,
	struct v90_phase3_return_line_handoff *handoff);

const char *v90_phase3_digital_station_result_name(
	enum v90_phase3_digital_station_result result);

#endif /* V90_PHASE3_DIGITAL_STATION_H */
