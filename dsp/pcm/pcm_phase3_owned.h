// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_PHASE3_OWNED_H
#define PCM_PHASE3_OWNED_H

#include "v90_phase3_digital_station.h"

/* Private serialized continuations for frames accepted by initialization.
 * The owned Jd/JP records stay immutable while cursors advance. Public entry
 * points and parameter updates still check canonical frames and CRCs. */

bool pcm_phase3_digital_tx_owned_state_is_valid(
	const struct pcm_phase3_digital_tx *transmitter);
enum pcm_phase3_digital_tx_result pcm_phase3_digital_tx_next_owned(
	struct pcm_phase3_digital_tx *transmitter,
	struct v90_pcm_symbol *symbol);
bool v90_phase3_digital_station_owned_state_is_valid(
	const struct v90_phase3_digital_station *station);
bool v90_phase3_digital_station_has_failed_owned(
	const struct v90_phase3_digital_station *station);
bool v90_phase3_digital_station_transmit_complete_owned(
	const struct v90_phase3_digital_station *station);
bool v90_phase3_digital_station_receive_complete_owned(
	const struct v90_phase3_digital_station *station);
enum v90_phase3_digital_station_result v90_phase3_digital_station_transmit_owned(
	struct v90_phase3_digital_station *station, uint8_t *network_octet);
enum v90_phase3_digital_station_result v90_phase3_digital_station_receive_owned(
	struct v90_phase3_digital_station *station, int16_t sample);
enum v90_phase3_digital_station_result
v90_phase3_digital_station_advance_completed_receive_owned(
	struct v90_phase3_digital_station *station, int16_t sample);

#endif /* PCM_PHASE3_OWNED_H */
