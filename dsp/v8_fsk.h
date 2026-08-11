// SPDX-License-Identifier: BSD-3-Clause

#ifndef V8_FSK_H
#define V8_FSK_H

#include "fsk.h"

#include <stdbool.h>
#include <stdint.h>

enum v8_station_role {
	V8_CALLING_STATION,
	V8_ANSWERING_STATION,
};

enum v8_fsk_result {
	V8_FSK_OK,
	V8_FSK_INVALID_ARGUMENT,
};

struct v8_fsk {
	struct fsk_transmitter transmitter;
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
enum v8_fsk_result v8_fsk_receive_sample(
	struct v8_fsk *fsk, int16_t sample, uint8_t *bit,
	bool *bit_ready);

const char *v8_fsk_result_name(
	enum v8_fsk_result result);

#endif
