// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_S_PATTERN_RX_H
#define PCM_S_PATTERN_RX_H

#include "v90_pcm_symbol.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	PCM_S_PATTERN_SYMBOLS = 6,
	PCM_S_PATTERN_BAR_SYMBOLS = 48,
	PCM_S_PATTERN_ACQUISITION_SYMBOLS = 18,
	PCM_S_PATTERN_ACQUISITION_MAX_ERRORS = 2,
	PCM_S_PATTERN_BAR_QUALIFY_SYMBOLS = 12,
	PCM_S_PATTERN_BAR_MAX_ERRORS = 2,
};

enum pcm_s_pattern_rx_stage {
	PCM_S_PATTERN_RX_SEARCH,
	PCM_S_PATTERN_RX_LOCKED,
	PCM_S_PATTERN_RX_REVERSED,
	PCM_S_PATTERN_RX_COMPLETE,
};

enum pcm_s_pattern_rx_result {
	PCM_S_PATTERN_RX_OK,
	PCM_S_PATTERN_RX_ACQUIRED,
	PCM_S_PATTERN_RX_REVERSAL_DETECTED,
	PCM_S_PATTERN_RX_SEQUENCE_COMPLETE,
	PCM_S_PATTERN_RX_INVALID_ARGUMENT,
	PCM_S_PATTERN_RX_INVALID_STATE,
	PCM_S_PATTERN_RX_INVALID_SEQUENCE,
};

struct pcm_s_pattern_rx {
	enum g711_law law;
	enum pcm_s_pattern_rx_stage stage;
	uint8_t signal_ucode;
	uint8_t history[PCM_S_PATTERN_ACQUISITION_SYMBOLS];
	uint8_t history_count;
	uint8_t history_cursor;
	uint8_t next_phase;
	uint8_t bar_candidate_symbols;
	uint8_t bar_symbols;
	uint8_t bar_errors;
	int32_t level;
};

/* Acquire W, +0, W, -W, -0, -W, then its 48-symbol reversal. Shared by
 * Phase 3 Sd and Quick Connect QTS. signal_ucode is W's actual seven-bit
 * magnitude, not INFO1's training code or QC1's UQTS index. Protocol owners
 * retain and validate that metadata, and own signal duration/timeouts. */
enum pcm_s_pattern_rx_result pcm_s_pattern_rx_init(
	struct pcm_s_pattern_rx *receiver, enum g711_law law,
	uint8_t signal_ucode);
bool pcm_s_pattern_rx_is_valid(
	const struct pcm_s_pattern_rx *receiver);
bool pcm_s_pattern_rx_is_complete(
	const struct pcm_s_pattern_rx *receiver);

/* Consume one network character. Qualify the six-sample waveform at the
 * acquired level, not exact G.711 amplitudes. Raw characters
 * retain the distinction between signed zeros for sub-threshold signals.
 * A call after completion does not consume the supplied octet. */
enum pcm_s_pattern_rx_result pcm_s_pattern_rx_receive(
	struct pcm_s_pattern_rx *receiver, uint8_t network_octet);

const char *pcm_s_pattern_rx_result_name(
	enum pcm_s_pattern_rx_result result);

#endif
