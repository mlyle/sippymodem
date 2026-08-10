// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V32_STARTUP_H
#define SIPPYMODEM_V32_STARTUP_H

#include "sippymodem_v32_rate_signal.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	/* One dibit is carried per 2400-baud training symbol. */
	SIPPYMODEM_V32_STARTUP_TRAINING_DIBITS = 1280,
	SIPPYMODEM_V32_STARTUP_RATE_GAP_DIBITS = 32,
	SIPPYMODEM_V32_STARTUP_CONFIRMATION_DIBITS = 128,
};

enum sippymodem_v32_endpoint_role {
	SIPPYMODEM_V32_CALLER,
	SIPPYMODEM_V32_ANSWERER,
};

enum sippymodem_v32_startup_phase {
	SIPPYMODEM_V32_CALLER_WAIT_OFFER,
	SIPPYMODEM_V32_CALLER_SEND_RESPONSE,
	SIPPYMODEM_V32_CALLER_SEND_END,
	SIPPYMODEM_V32_ANSWERER_TRAIN,
	SIPPYMODEM_V32_ANSWERER_SEND_OFFER,
	SIPPYMODEM_V32_ANSWERER_RETRAIN,
	SIPPYMODEM_V32_ANSWERER_SEND_SELECTION,
	SIPPYMODEM_V32_ANSWERER_SEND_END,
	SIPPYMODEM_V32_CONFIRM,
	SIPPYMODEM_V32_DATA,
};

enum sippymodem_v32_startup_output {
	SIPPYMODEM_V32_STARTUP_DIBIT,
	SIPPYMODEM_V32_STARTUP_DATA,
};

enum sippymodem_v32_startup_result {
	SIPPYMODEM_V32_STARTUP_OK,
	SIPPYMODEM_V32_STARTUP_INVALID_ARGUMENT,
	SIPPYMODEM_V32_STARTUP_NO_COMMON_RATE,
};

struct sippymodem_v32_startup {
	struct sippymodem_v32_rate_signal_detector detector;
	uint16_t rate_word;
	uint16_t end_word;
	uint16_t phase_dibits;
	uint16_t receive_mark_dibits;
	uint16_t rate_gap_dibits;
	enum sippymodem_v32_endpoint_role role;
	enum sippymodem_v32_startup_phase phase;
	uint8_t word_dibit;
	bool end_received;
};

enum sippymodem_v32_startup_result sippymodem_v32_startup_init(
	struct sippymodem_v32_startup *startup,
	enum sippymodem_v32_endpoint_role role);
bool sippymodem_v32_startup_is_valid(
	const struct sippymodem_v32_startup *startup);
bool sippymodem_v32_startup_is_complete(
	const struct sippymodem_v32_startup *startup);

enum sippymodem_v32_startup_result sippymodem_v32_startup_next_transmit(
	struct sippymodem_v32_startup *startup,
	enum sippymodem_v32_startup_output *output, uint8_t *dibit);
enum sippymodem_v32_startup_result sippymodem_v32_startup_observe(
	struct sippymodem_v32_startup *startup, uint8_t dibit,
	bool *connected);

const char *sippymodem_v32_startup_phase_name(
	enum sippymodem_v32_startup_phase phase);
const char *sippymodem_v32_startup_result_name(
	enum sippymodem_v32_startup_result result);

#endif
