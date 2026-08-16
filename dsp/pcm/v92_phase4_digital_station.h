// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PHASE4_DIGITAL_STATION_H
#define V92_PHASE4_DIGITAL_STATION_H

#include "pcm_downstream_tx.h"
#include "v92_phase4_digital_rx.h"

#include <stdbool.h>
#include <stdint.h>

enum v92_phase4_digital_station_result {
	V92_PHASE4_DIGITAL_STATION_OK,
	V92_PHASE4_DIGITAL_STATION_EVENT,
	V92_PHASE4_DIGITAL_STATION_FAILED,
	V92_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT,
	V92_PHASE4_DIGITAL_STATION_INVALID_STATE,
	V92_PHASE4_DIGITAL_STATION_INVALID_PARAMETERS,
	V92_PHASE4_DIGITAL_STATION_INTERNAL_ERROR,
};

enum v92_phase4_digital_station_tx_stage {
	V92_PHASE4_DIGITAL_STATION_INITIAL_SUVD,
	V92_PHASE4_DIGITAL_STATION_READY_FOR_CPD,
	V92_PHASE4_DIGITAL_STATION_CPD,
	V92_PHASE4_DIGITAL_STATION_STATUS_SUVD,
};

/* Own the digital modem's V.92 Phase 4 control exchange after E1u. The
 * upstream receiver remains continuous with the Phase 3 equalizer while the
 * inherited downstream transmitter remains continuous with TRN2d. A valid
 * SUVu selects one compact CPd followed by SUVd; a later valid CPu makes the
 * next CPd or SUVd acknowledge it. Each downstream change waits for a
 * complete current record. Timeout-driven CPd repetition, Ed, and the data-
 * mode handoff are deliberately outside this initial owner. One instance is
 * serialized; receive and transmit share procedure state. */
struct v92_phase4_digital_station {
	struct v92_phase4_digital_rx receive;
	struct pcm_downstream_tx transmit;
	struct v92_cpu_parameters cpu;
	struct v92_cpd_base_parameters cpd;
	struct v92_suvu_parameters suvu;
	uint64_t cpd_start_frame;
	enum v92_phase4_digital_station_tx_stage transmit_stage;
	bool request_silence;
	bool suvu_received;
	bool cpu_received;
	bool transmit_acknowledges_cpu;
	bool peer_acknowledged_cpd;
	bool failed;
};

/* Continue from the exact Phase 3 receive handoff and the already-running
 * downstream TRN2d/SUVd transmitter. The compact CPd parameter type keeps
 * optional tables outside this bounded control owner. */
enum v92_phase4_digital_station_result v92_phase4_digital_station_init(
	struct v92_phase4_digital_station *station,
	const struct v92_pcm_phase3_cpt_line_handoff *receive_handoff,
	enum v92_constellation_size constellation,
	const struct pcm_downstream_tx *downstream,
	const struct v92_cpd_base_parameters *cpd);

bool v92_phase4_digital_station_is_valid(
	const struct v92_phase4_digital_station *station);
bool v92_phase4_digital_station_has_failed(
	const struct v92_phase4_digital_station *station);

/* Consume one signed 8 kHz upstream sample. EVENT reports a complete decoded
 * record. Rejected wire records advance only the receive line and do not
 * alter downstream procedure state. Invalid calls consume no sample. */
enum v92_phase4_digital_station_result v92_phase4_digital_station_receive(
	struct v92_phase4_digital_station *station, int16_t sample);

/* Produce one downstream network character. A queued procedure transition is
 * applied before generation only at an exact source-record boundary. */
enum v92_phase4_digital_station_result v92_phase4_digital_station_transmit(
	struct v92_phase4_digital_station *station, uint8_t *network_octet);

/* Copy the accepted CPu, including an acknowledgement advanced by a
 * semantically identical repeat, for the later E2u/data-path owner. */
bool v92_phase4_digital_station_cpu_parameters(
	const struct v92_phase4_digital_station *station,
	struct v92_cpu_parameters *parameters);

/* Report whether a valid SUVu or CPu has acknowledged the selected CPd. */
bool v92_phase4_digital_station_peer_acknowledged_cpd(
	const struct v92_phase4_digital_station *station);

const char *v92_phase4_digital_station_result_name(
	enum v92_phase4_digital_station_result result);

#endif /* V92_PHASE4_DIGITAL_STATION_H */
