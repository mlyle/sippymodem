// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PHASE4_ANALOG_STATION_H
#define V92_PHASE4_ANALOG_STATION_H

#include "pcm_downstream_rx.h"
#include "v92_phase3_analog_station.h"
#include "v92_phase4_analog_tx.h"

#include <stdbool.h>
#include <stdint.h>

enum v92_phase4_analog_station_result {
	V92_PHASE4_ANALOG_STATION_OK,
	V92_PHASE4_ANALOG_STATION_EVENT,
	V92_PHASE4_ANALOG_STATION_E2U_READY,
	V92_PHASE4_ANALOG_STATION_FAILED,
	V92_PHASE4_ANALOG_STATION_INVALID_ARGUMENT,
	V92_PHASE4_ANALOG_STATION_INVALID_PARAMETERS,
	V92_PHASE4_ANALOG_STATION_INVALID_STATE,
	V92_PHASE4_ANALOG_STATION_INTERNAL_ERROR,
};

/* Own the analogue modem's initial Phase 4 control exchange. The inherited
 * downstream receiver preserves the Phase 3 coding histories; the upstream
 * transmitter preserves the CPt/E1u differential-sign boundary. Only the
 * compact, table-free CPd selection is retained. Ed/E2u emission and data
 * mode follow in the next owner. One instance is serialized. */
struct v92_phase4_analog_station {
	struct v92_phase4_analog_tx transmit;
	struct pcm_downstream_rx receive;
	struct v92_suvd_parameters suvd;
	struct v92_cpd_base_parameters cpd;
	bool suvd_received;
	bool cpd_received;
	bool failed;
};

/* Exact duplex boundary after the control exchange. The upstream modulator
 * continues into E2u, while the downstream receiver continues through Ed.
 * CPU and CPd retain the independently selected downstream and upstream data
 * mappings for the next owner. */
struct v92_phase4_analog_handoff {
	struct v92_phase4_modulator transmit;
	struct pcm_downstream_rx receive;
	struct v92_cpu_parameters cpu;
	struct v92_cpd_base_parameters cpd;
};

/* Resume from exact Phase 3 handoffs. Optional early controls preserve a
 * complete record decoded while the independent CPt/E1u direction finished.
 * Rejected initialization leaves station unchanged. */
enum v92_phase4_analog_station_result v92_phase4_analog_station_init(
	struct v92_phase4_analog_station *station,
	const struct v92_phase3_analog_transmit_handoff *transmit_handoff,
	const struct pcm_downstream_rx *downstream,
	const struct v92_suvu_parameters *suvu,
	const struct v92_cpu_parameters *cpu,
	const struct v92_suvd_parameters *early_suvd,
	const struct v92_cpd_base_parameters *early_cpd);

bool v92_phase4_analog_station_is_valid(
	const struct v92_phase4_analog_station *station);
bool v92_phase4_analog_station_has_failed(
	const struct v92_phase4_analog_station *station);
bool v92_phase4_analog_station_is_e2u_ready(
	const struct v92_phase4_analog_station *station);

/* Consume one authoritative downstream G.711 character. Complete rejected
 * records are ignored; a changed valid selection fails the station. */
enum v92_phase4_analog_station_result v92_phase4_analog_station_receive(
	struct v92_phase4_analog_station *station, uint8_t network_octet);

/* Emit one signed 8 kHz upstream sample. E2U_READY consumes no interval and
 * leaves sample unchanged. */
enum v92_phase4_analog_station_result v92_phase4_analog_station_transmit(
	struct v92_phase4_analog_station *station, int16_t *sample);

/* The call timer may request one complete CPu repeat after its normative
 * deadline. */
enum v92_phase4_analog_station_result
v92_phase4_analog_station_repeat_cpu(
	struct v92_phase4_analog_station *station);

/* Export both continuous line directions and their selected mappings at the
 * E2u boundary. Rejected calls leave the handoff unchanged. */
bool v92_phase4_analog_station_cpd_parameters(
	const struct v92_phase4_analog_station *station,
	struct v92_cpd_base_parameters *parameters);
bool v92_phase4_analog_handoff_is_valid(
	const struct v92_phase4_analog_handoff *handoff);
enum v92_phase4_analog_station_result v92_phase4_analog_station_handoff(
	const struct v92_phase4_analog_station *station,
	struct v92_phase4_analog_handoff *handoff);

const char *v92_phase4_analog_station_result_name(
	enum v92_phase4_analog_station_result result);

#endif /* V92_PHASE4_ANALOG_STATION_H */
