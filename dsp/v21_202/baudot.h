// SPDX-License-Identifier: BSD-3-Clause

#ifndef BAUDOT_H
#define BAUDOT_H

#include "fsk.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	BAUDOT_SAMPLE_RATE = 8000,
	BAUDOT_BLOCK_SAMPLES = 160,
	BAUDOT_TX_CODE_CAPACITY = 256,
	BAUDOT_RX_CHARACTER_CAPACITY = 128,
};

/* The 45 selector denotes the North American 500/11-symbol/s mode. */
enum baudot_rate {
	BAUDOT_RATE_45 = 45,
	BAUDOT_RATE_50 = 50,
};

enum baudot_result {
	BAUDOT_OK,
	BAUDOT_INVALID_ARGUMENT,
};

enum baudot_shift {
	BAUDOT_LETTERS,
	BAUDOT_FIGURES,
};

enum baudot_transmit_phase {
	BAUDOT_TX_IDLE,
	BAUDOT_TX_PREAMBLE,
	BAUDOT_TX_CODE,
	BAUDOT_TX_MARK_HOLD,
};

struct baudot_transmitter {
	struct fsk_tone_generator tones;
	enum baudot_rate rate;
	enum baudot_shift shift;
	enum baudot_transmit_phase phase;
	uint64_t generated_tone_samples;
	uint64_t transmitted_codes;
	uint16_t bit_samples;
	uint16_t stop_samples;
	uint16_t samples_remaining;
	uint16_t code_head;
	uint16_t code_count;
	uint8_t codes[BAUDOT_TX_CODE_CAPACITY];
	uint8_t current_code;
	uint8_t current_element;
	uint8_t current_bit;
	uint8_t characters_since_shift;
};

struct baudot_receiver {
	struct fsk_receiver detector;
	enum baudot_rate rate;
	enum baudot_shift shift;
	uint64_t received_characters;
	uint64_t framing_errors;
	uint64_t receive_overruns;
	uint16_t bit_samples;
	uint16_t quiet_samples;
	uint16_t stable_samples;
	uint16_t decision_countdown;
	uint16_t character_head;
	uint16_t character_count;
	uint8_t characters[BAUDOT_RX_CHARACTER_CAPACITY];
	uint8_t detector_bit;
	uint8_t stable_bit;
	uint8_t data_index;
	uint8_t code;
	bool carrier;
	bool framing;
};

enum baudot_result baudot_transmitter_init(
	struct baudot_transmitter *transmitter,
	enum baudot_rate rate);

/* Queues as many complete terminal characters as fit. A character and any
 * required shift codes are committed atomically. */
size_t baudot_queue_text(
	struct baudot_transmitter *transmitter,
	const uint8_t *text,
	size_t count);
size_t baudot_pending_codes(
	const struct baudot_transmitter *transmitter);
bool baudot_transmitting(
	const struct baudot_transmitter *transmitter);

/* Generates exactly count samples, padding an inactive transmitter with
 * silence. Oscillator phase remains continuous across every tone change. */
enum baudot_result baudot_generate(
	struct baudot_transmitter *transmitter,
	int16_t *output,
	size_t count);

enum baudot_result baudot_receiver_init(
	struct baudot_receiver *receiver,
	enum baudot_rate rate);
/* Clears carrier and partial-frame state without discarding decoded text,
 * statistics, or the active letters/figures column. */
void baudot_receiver_reset(
	struct baudot_receiver *receiver);
enum baudot_result baudot_receive(
	struct baudot_receiver *receiver,
	const int16_t *input,
	size_t count);
size_t baudot_drain_text(
	struct baudot_receiver *receiver,
	uint8_t *text,
	size_t capacity);
size_t baudot_pending_text(
	const struct baudot_receiver *receiver);

#endif /* BAUDOT_H */
