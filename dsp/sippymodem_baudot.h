// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_BAUDOT_H
#define SIPPYMODEM_BAUDOT_H

#include "sippymodem_fsk.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	SIPPYMODEM_BAUDOT_SAMPLE_RATE = 8000,
	SIPPYMODEM_BAUDOT_BLOCK_SAMPLES = 160,
	SIPPYMODEM_BAUDOT_TX_CODE_CAPACITY = 256,
	SIPPYMODEM_BAUDOT_RX_CHARACTER_CAPACITY = 128,
};

/* The 45 selector denotes the North American 500/11-symbol/s mode. */
enum sippymodem_baudot_rate {
	SIPPYMODEM_BAUDOT_RATE_45 = 45,
	SIPPYMODEM_BAUDOT_RATE_50 = 50,
};

enum sippymodem_baudot_result {
	SIPPYMODEM_BAUDOT_OK,
	SIPPYMODEM_BAUDOT_INVALID_ARGUMENT,
};

enum sippymodem_baudot_shift {
	SIPPYMODEM_BAUDOT_LETTERS,
	SIPPYMODEM_BAUDOT_FIGURES,
};

enum sippymodem_baudot_transmit_phase {
	SIPPYMODEM_BAUDOT_TX_IDLE,
	SIPPYMODEM_BAUDOT_TX_PREAMBLE,
	SIPPYMODEM_BAUDOT_TX_CODE,
	SIPPYMODEM_BAUDOT_TX_MARK_HOLD,
};

struct sippymodem_baudot_transmitter {
	struct sippymodem_fsk_tone_generator tones;
	enum sippymodem_baudot_rate rate;
	enum sippymodem_baudot_shift shift;
	enum sippymodem_baudot_transmit_phase phase;
	uint64_t generated_tone_samples;
	uint64_t transmitted_codes;
	uint16_t bit_samples;
	uint16_t stop_samples;
	uint16_t samples_remaining;
	uint16_t code_head;
	uint16_t code_count;
	uint8_t codes[SIPPYMODEM_BAUDOT_TX_CODE_CAPACITY];
	uint8_t current_code;
	uint8_t current_element;
	uint8_t current_bit;
	uint8_t characters_since_shift;
};

struct sippymodem_baudot_receiver {
	struct sippymodem_fsk_receiver detector;
	enum sippymodem_baudot_rate rate;
	enum sippymodem_baudot_shift shift;
	uint64_t received_characters;
	uint64_t framing_errors;
	uint64_t receive_overruns;
	uint16_t bit_samples;
	uint16_t quiet_samples;
	uint16_t stable_samples;
	uint16_t decision_countdown;
	uint16_t character_head;
	uint16_t character_count;
	uint8_t characters[SIPPYMODEM_BAUDOT_RX_CHARACTER_CAPACITY];
	uint8_t detector_bit;
	uint8_t stable_bit;
	uint8_t data_index;
	uint8_t code;
	bool carrier;
	bool framing;
};

enum sippymodem_baudot_result sippymodem_baudot_transmitter_init(
	struct sippymodem_baudot_transmitter *transmitter,
	enum sippymodem_baudot_rate rate);

/* Queues as many complete terminal characters as fit. A character and any
 * required shift codes are committed atomically. */
size_t sippymodem_baudot_queue_text(
	struct sippymodem_baudot_transmitter *transmitter,
	const uint8_t *text,
	size_t count);
size_t sippymodem_baudot_pending_codes(
	const struct sippymodem_baudot_transmitter *transmitter);
bool sippymodem_baudot_transmitting(
	const struct sippymodem_baudot_transmitter *transmitter);

/* Generates exactly count samples, padding an inactive transmitter with
 * silence. Oscillator phase remains continuous across every tone change. */
enum sippymodem_baudot_result sippymodem_baudot_generate(
	struct sippymodem_baudot_transmitter *transmitter,
	int16_t *output,
	size_t count);

enum sippymodem_baudot_result sippymodem_baudot_receiver_init(
	struct sippymodem_baudot_receiver *receiver,
	enum sippymodem_baudot_rate rate);
/* Clears carrier and partial-frame state without discarding decoded text,
 * statistics, or the active letters/figures column. */
void sippymodem_baudot_receiver_reset(
	struct sippymodem_baudot_receiver *receiver);
enum sippymodem_baudot_result sippymodem_baudot_receive(
	struct sippymodem_baudot_receiver *receiver,
	const int16_t *input,
	size_t count);
size_t sippymodem_baudot_drain_text(
	struct sippymodem_baudot_receiver *receiver,
	uint8_t *text,
	size_t capacity);
size_t sippymodem_baudot_pending_text(
	const struct sippymodem_baudot_receiver *receiver);

const char *sippymodem_baudot_result_name(
	enum sippymodem_baudot_result result);

#endif
