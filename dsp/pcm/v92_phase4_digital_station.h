// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PHASE4_DIGITAL_STATION_H
#define V92_PHASE4_DIGITAL_STATION_H

#include "pcm_downstream_tx.h"
#include "v92_phase4_cpd_tx.h"
#include "v92_phase4_digital_rx.h"
#include "v92_pcm_mapping.h"
#include "v92_upstream_plan.h"
#include "v92_cleardown.h"

#include <stdbool.h>
#include <stdint.h>

enum v92_phase4_digital_station_result {
	V92_PHASE4_DIGITAL_STATION_OK,
	V92_PHASE4_DIGITAL_STATION_EVENT,
	V92_PHASE4_DIGITAL_STATION_CLEARDOWN_COMPLETE,
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
 * SUVu selects one complete initial CPd followed by SUVd; a later valid CPu
 * makes that initial CPd or the next SUVd acknowledge it. Each change waits for a
 * complete current record. After 100 ms + RTD without acknowledgement, a
 * complete unacknowledging CPu/SUVu starts continuous CPd repetition. The
 * final record and Ed drain in the handoff's transmitter. Zero-rate CPu/CPus
 * instead requests an acknowledged, bounded cleardown without Ed. One instance
 * is serialized; receive and transmit share procedure state. */
struct v92_phase4_digital_station {
	struct v92_phase4_digital_rx receive;
	struct pcm_downstream_tx transmit;
	struct v92_phase4_cpd_tx initial_cpd;
	struct v92_upstream_plan upstream_plan;
	struct v92_cpu_parameters cpu;
	struct v92_suvu_parameters suvu;
	struct v92_cleardown cleardown;
	uint64_t cpd_start_frame;
	uint64_t status_start_frame;
	uint64_t acknowledgement_end_frame;
	uint16_t round_trip_delay_samples;
	enum v92_phase4_digital_station_tx_stage transmit_stage;
	bool request_silence;
	bool suvu_received;
	bool cpu_received;
	bool transmit_acknowledges_cpu;
	bool peer_acknowledged_cpd;
	bool repeat_cpd;
	bool failed;
};

/* Exact duplex boundary after both selected controls are acknowledged. The
 * warm upstream line continues into E2u while the downstream coder completes
 * its current CPd/SUVd and Ed. */
struct v92_phase4_digital_handoff {
	struct v92_upstream_control_line_rx receive;
	struct v92_phase4_ed_tx transmit;
	struct v92_cpu_parameters cpu;
	/* The complete CPd actually transmitted, not a regenerated profile. */
	struct v92_pcm_mapping mapping;
	struct v92_upstream_plan upstream_plan;
};

/* Continue from the exact Phase 3 receive handoff and the already-running
 * downstream TRN2d/SUVd transmitter. If control is not configured yet, defer
 * SUVd until the complete channel measurement. Otherwise the supplied SUVd
 * must match the inherited selection. Retain complete CPd in the wire owner;
 * acknowledging CPu changes no filters or constellation tables. */
enum v92_phase4_digital_station_result v92_phase4_digital_station_init(
	struct v92_phase4_digital_station *station,
	const struct v92_pcm_phase3_cpt_line_handoff *receive_handoff,
	enum v92_constellation_size constellation,
	const struct pcm_downstream_tx *downstream,
	const struct v92_suvd_parameters *suvd,
	const struct v92_cpd_parameters *cpd,
	uint16_t round_trip_delay_samples);

bool v92_phase4_digital_station_is_valid(
	const struct v92_phase4_digital_station *station);
bool v92_phase4_digital_station_has_failed(
	const struct v92_phase4_digital_station *station);

/* Cancel the current Phase 4 exchange with zero-rate CPd. Finish any
 * in-flight record, acknowledge a peer zero-rate CP, and retain the line
 * until Amendment 1, 9.11 permits release. No Ed or data handoff follows. */
bool v92_phase4_digital_station_start_cleardown(
	struct v92_phase4_digital_station *station);

/* Replace an unsent initial CPd and its matching local receive plan after
 * channel measurement. Once CPd transmission starts, selection is frozen
 * through all acknowledgements/repeats. Rejected calls change nothing. */
enum v92_phase4_digital_station_result v92_phase4_digital_station_select_upstream(
	struct v92_phase4_digital_station *station,
	const struct v92_cpd_parameters *cpd, const struct v92_upstream_plan *plan);

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

/* Begin E2u reception once a complete CPu has been accepted, CPd has been
 * acknowledged, and an acknowledging downstream record has been sourced.
 * The handoff drains any current CPd/SUVd before Ed without losing E2u. */
bool v92_phase4_digital_station_is_ed_ready(
	const struct v92_phase4_digital_station *station);

bool v92_phase4_digital_handoff_is_valid(
	const struct v92_phase4_digital_handoff *handoff);
enum v92_phase4_digital_station_result v92_phase4_digital_station_handoff(
	const struct v92_phase4_digital_station *station,
	struct v92_phase4_digital_handoff *handoff);

const char *v92_phase4_digital_station_result_name(
	enum v92_phase4_digital_station_result result);

#endif /* V92_PHASE4_DIGITAL_STATION_H */
