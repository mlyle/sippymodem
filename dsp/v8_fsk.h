// SPDX-License-Identifier: BSD-3-Clause

#ifndef V8_FSK_H
#define V8_FSK_H

#include "fsk.h"
#include "v8_line_conditioner.h"

#include <stdbool.h>
#include <stdint.h>

enum v8_fsk_result {
	V8_FSK_OK,
	V8_FSK_INVALID_ARGUMENT,
};

enum {
	V8_FSK_TRANSMIT_FILTER_TAPS = 51,
};

struct v8_fsk_transmitter {
	int16_t filter_history[V8_FSK_TRANSMIT_FILTER_TAPS];
	uint16_t carrier_phase;
	uint16_t bit_progress;
	uint16_t mark_phase_step;
	uint16_t space_phase_step;
	int16_t gain;
	uint8_t held_bit;
	bool holding_bit;
};

struct v8_fsk {
	struct v8_fsk_transmitter transmitter;
	struct v8_line_conditioner line;
	struct fsk_receiver receiver;
	enum v8_station_role role;
};

enum v8_fsk_result v8_fsk_init(
	struct v8_fsk *fsk,
	enum v8_station_role role);
enum v8_fsk_result v8_fsk_reset(
	struct v8_fsk *fsk);

bool v8_fsk_can_accept_bit(
	const struct v8_fsk *fsk);
enum v8_fsk_result v8_fsk_transmit_sample(
	struct v8_fsk *fsk, uint8_t bit, int16_t *sample,
	bool *bit_consumed);
/* ANSam and the following answer-station control message share this exact
 * line-filter history. */
enum v8_fsk_result v8_fsk_filter_transmit_sample(
	struct v8_fsk *fsk, int16_t unfiltered, int16_t *sample);
enum v8_fsk_result v8_fsk_receive_sample(
	struct v8_fsk *fsk, int16_t sample, uint8_t *bit,
	bool *bit_ready);

const char *v8_fsk_result_name(
	enum v8_fsk_result result);

#endif
