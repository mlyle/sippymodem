// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE3_SD_RECEIVER_H
#define V90_PHASE3_SD_RECEIVER_H

#include "v90_pcm_symbol.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V90_PHASE3_SD_ACQUISITION_SYMBOLS = 18,
	V90_PHASE3_SD_ACQUISITION_MAX_ERRORS = 2,
	V90_PHASE3_SD_BAR_QUALIFY_SYMBOLS = 12,
	V90_PHASE3_SD_BAR_MAX_ERRORS = 2,
};

enum v90_phase3_sd_receiver_stage {
	V90_PHASE3_SD_RECEIVER_SEARCH,
	V90_PHASE3_SD_RECEIVER_SD,
	V90_PHASE3_SD_RECEIVER_SD_BAR,
	V90_PHASE3_SD_RECEIVER_COMPLETE,
};

enum v90_phase3_sd_receiver_result {
	V90_PHASE3_SD_RECEIVER_OK,
	V90_PHASE3_SD_RECEIVER_SD_DETECTED,
	V90_PHASE3_SD_RECEIVER_SD_BAR_DETECTED,
	V90_PHASE3_SD_RECEIVER_SEQUENCE_COMPLETE,
	V90_PHASE3_SD_RECEIVER_INVALID_ARGUMENT,
	V90_PHASE3_SD_RECEIVER_INVALID_STATE,
	V90_PHASE3_SD_RECEIVER_INVALID_SEQUENCE,
};

struct v90_phase3_sd_receiver {
	enum g711_law law;
	enum v90_phase3_sd_receiver_stage stage;
	uint8_t training_ucode;
	uint8_t history[V90_PHASE3_SD_ACQUISITION_SYMBOLS];
	uint8_t history_count;
	uint8_t history_cursor;
	uint8_t next_phase;
	uint8_t bar_candidate_symbols;
	uint8_t bar_symbols;
	uint8_t bar_errors;
};

enum v90_phase3_sd_receiver_result v90_phase3_sd_receiver_init(
	struct v90_phase3_sd_receiver *receiver, enum g711_law law,
	uint8_t training_ucode);
bool v90_phase3_sd_receiver_is_valid(
	const struct v90_phase3_sd_receiver *receiver);
bool v90_phase3_sd_receiver_is_complete(
	const struct v90_phase3_sd_receiver *receiver);

/* Consume one raw network character.  Keeping the G.711 octet intact is
 * necessary to distinguish both zero characters in PCMU.  A call after
 * completion returns SEQUENCE_COMPLETE without consuming the supplied octet. */
enum v90_phase3_sd_receiver_result v90_phase3_sd_receiver_receive(
	struct v90_phase3_sd_receiver *receiver, uint8_t network_octet);

const char *v90_phase3_sd_receiver_result_name(
	enum v90_phase3_sd_receiver_result result);

#endif
