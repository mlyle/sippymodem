// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE4_ANALOG_STATION_H
#define V90_PHASE4_ANALOG_STATION_H

#include "pcm_downstream_rx.h"
#include "v90_jd.h"
#include "v90_phase4_cp_line_tx.h"
#include "v90_phase4_ri_search.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_phase4_analog_station_result {
	V90_PHASE4_ANALOG_STATION_OK,
	V90_PHASE4_ANALOG_STATION_COMPLETE,
	V90_PHASE4_ANALOG_STATION_FAILED,
	V90_PHASE4_ANALOG_STATION_INVALID_ARGUMENT,
	V90_PHASE4_ANALOG_STATION_INVALID_STATE,
	V90_PHASE4_ANALOG_STATION_INVALID_PARAMETERS,
	V90_PHASE4_ANALOG_STATION_OUT_OF_RANGE,
	V90_PHASE4_ANALOG_STATION_CLIPPING_RISK,
	V90_PHASE4_ANALOG_STATION_SEQUENCE_MISMATCH,
	V90_PHASE4_ANALOG_STATION_NUMERIC_FAILURE,
};

/* Ready raw-network downstream receiver at the first character following
 * Ri-bar. Authoritative G.711 characters use the transmit constellation
 * plane. A future analogue slicer must initialize a separate codec-plane
 * receiver at this same boundary. TRN2d starts a fresh coder epoch, which
 * continues unchanged through MP, MP-prime, and Ed. */
struct v90_phase4_analog_receive_handoff {
	struct pcm_downstream_rx receiver;
};

/* Own the analogue modem's initial Phase 4 exchange. CPt is repeated on the
 * continuous Phase 3 QAM line until the receive direction detects Ri-bar.
 * Ri acquisition starts only after CPt has been exposed for the measured
 * round-trip delay. Only the CPt record then in progress is completed.
 * Receive and transmit calls may complete independently, but calls on one
 * instance are serialized. */
struct v90_phase4_analog_station {
	struct v90_phase4_cp_line_tx transmit;
	struct v90_phase4_ri_search receive;
	struct v90_cp_parameters training_parameters;
	uint32_t ri_wait_remaining_samples;
	uint8_t maximum_spectral_lookahead;
	bool finish_cpt_requested;
	bool failed;
};

enum v90_phase4_analog_station_result v90_phase4_analog_station_init(
	struct v90_phase4_analog_station *station,
	const struct v34_training_line *phase3_transmit,
	const struct v90_jd_parameters *jd, enum g711_law law,
	uint8_t training_ucode, uint32_t round_trip_delay_samples,
	const struct v90_cp_parameters *training_parameters);
bool v90_phase4_analog_station_is_valid(
	const struct v90_phase4_analog_station *station);
bool v90_phase4_analog_station_has_failed(
	const struct v90_phase4_analog_station *station);

/* Produce one 8 kHz analogue sample. The final accepted CPt symbol is allowed
 * to finish the line's intersymbol interval. COMPLETE consumes no time and
 * leaves the destination unchanged at an exact next-point boundary. */
enum v90_phase4_analog_station_result v90_phase4_analog_station_transmit(
	struct v90_phase4_analog_station *station, int16_t *sample);

/* Consume one raw G.711 character. Characters inside the initial round-trip
 * delay do not contribute Ri evidence. The character that completes Ri-bar is
 * consumed normally; a later call returns COMPLETE without consuming input. */
enum v90_phase4_analog_station_result v90_phase4_analog_station_receive(
	struct v90_phase4_analog_station *station, uint8_t network_octet);

bool v90_phase4_analog_station_transmit_complete(
	const struct v90_phase4_analog_station *station);
bool v90_phase4_analog_station_receive_complete(
	const struct v90_phase4_analog_station *station);

/* Preserve both the live QAM line and CP scrambler/differential histories so
 * the next owner may start CP directly. An optional SCR transition is outside
 * this initial-exchange owner and must account for both histories explicitly.
 */
enum v90_phase4_analog_station_result
v90_phase4_analog_station_transmit_handoff(
	const struct v90_phase4_analog_station *station,
	struct v90_phase4_cp_line_tx *transmit);

bool v90_phase4_analog_receive_handoff_is_valid(
	const struct v90_phase4_analog_receive_handoff *handoff);
enum v90_phase4_analog_station_result
v90_phase4_analog_station_receive_handoff(
	const struct v90_phase4_analog_station *station,
	struct v90_phase4_analog_receive_handoff *handoff);

const char *v90_phase4_analog_station_result_name(
	enum v90_phase4_analog_station_result result);

#endif /* V90_PHASE4_ANALOG_STATION_H */
