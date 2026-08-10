// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V8_FSK_H
#define SIPPYMODEM_V8_FSK_H

#include "sippymodem_fsk.h"

#include <stdbool.h>
#include <stdint.h>

enum sippymodem_v8_station_role {
	SIPPYMODEM_V8_CALLING_STATION,
	SIPPYMODEM_V8_ANSWERING_STATION,
};

enum sippymodem_v8_fsk_result {
	SIPPYMODEM_V8_FSK_OK,
	SIPPYMODEM_V8_FSK_INVALID_ARGUMENT,
};

struct sippymodem_v8_fsk {
	struct sippymodem_fsk_transmitter transmitter;
	struct sippymodem_fsk_receiver receiver;
	enum sippymodem_v8_station_role role;
};

enum sippymodem_v8_fsk_result sippymodem_v8_fsk_init(
	struct sippymodem_v8_fsk *fsk,
	enum sippymodem_v8_station_role role);
enum sippymodem_v8_fsk_result sippymodem_v8_fsk_reset(
	struct sippymodem_v8_fsk *fsk);

bool sippymodem_v8_fsk_can_accept_bit(
	const struct sippymodem_v8_fsk *fsk);
enum sippymodem_v8_fsk_result sippymodem_v8_fsk_transmit_sample(
	struct sippymodem_v8_fsk *fsk, uint8_t bit, int16_t *sample,
	bool *bit_consumed);
enum sippymodem_v8_fsk_result sippymodem_v8_fsk_receive_sample(
	struct sippymodem_v8_fsk *fsk, int16_t sample, uint8_t *bit,
	bool *bit_ready);

const char *sippymodem_v8_fsk_result_name(
	enum sippymodem_v8_fsk_result result);

#endif
