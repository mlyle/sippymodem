// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE4_OWNED_H
#define V90_PHASE4_OWNED_H

#include "v90_phase4_analog_startup.h"

/* Private PCM continuations. Constructors, record replacement, online resume,
 * and handoff validate the complete frame and mapping. During serialized
 * ownership, those records and parameters stay immutable; these continuations
 * retain cursor, framing, line, receiver, and output-alias checks per sample.
 * Public entry points always validate the complete caller-provided state. */

struct pcm_call;
bool pcm_call_phase4_is_ready_owned(const struct pcm_call *call);

bool v90_phase4_cp_tx_owned_state_is_valid(
	const struct v90_phase4_cp_tx *tx);
enum v90_phase4_cp_tx_result v90_phase4_cp_tx_next_owned(
	struct v90_phase4_cp_tx *tx,
	struct qam_point *point, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns, bool *record_complete);
enum v90_phase4_cp_tx_result v90_phase4_cp_tx_scr_next_owned(
	struct v90_phase4_cp_tx *tx,
	struct qam_point *point, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns);
bool v90_phase4_cp_line_tx_owned_state_is_valid(
	const struct v90_phase4_cp_line_tx *tx);
enum v90_phase4_cp_line_tx_result v90_phase4_cp_line_tx_sample_owned(
	struct v90_phase4_cp_line_tx *tx, int16_t *sample);
enum v90_phase4_cp_line_tx_result v90_phase4_cp_line_tx_scr_sample_owned(
	struct v90_phase4_cp_line_tx *tx, bool stop_at_boundary,
	int16_t *sample);
bool v90_phase4_cp_line_tx_record_is_complete_owned(
	const struct v90_phase4_cp_line_tx *tx);
bool v90_phase4_cp_line_tx_is_handoff_ready_owned(
	const struct v90_phase4_cp_line_tx *tx);
bool v90_phase4_analog_station_owned_state_is_valid(
	const struct v90_phase4_analog_station *station);
bool v90_phase4_analog_station_has_failed_owned(
	const struct v90_phase4_analog_station *station);
bool v90_phase4_analog_station_transmit_complete_owned(
	const struct v90_phase4_analog_station *station);
bool v90_phase4_analog_station_receive_complete_owned(
	const struct v90_phase4_analog_station *station);
enum v90_phase4_analog_station_result v90_phase4_analog_station_transmit_owned(
	struct v90_phase4_analog_station *station, int16_t *sample);
enum v90_phase4_analog_station_result v90_phase4_analog_station_receive_owned(
	struct v90_phase4_analog_station *station, uint8_t network_octet);
bool v90_phase4_analog_startup_owned_state_is_valid(
	const struct v90_phase4_analog_startup *startup);
bool v90_phase4_analog_startup_has_failed_owned(
	const struct v90_phase4_analog_startup *startup);
bool v90_phase4_analog_startup_is_complete_owned(
	const struct v90_phase4_analog_startup *startup);
enum v90_phase4_startup_result v90_phase4_analog_startup_octet_owned(
	struct v90_phase4_analog_startup *startup,
	uint8_t received_octet, uint8_t *transmit_octet);

#endif /* V90_PHASE4_OWNED_H */
