// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V21_H
#define SIPPYMODEM_V21_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V21_SAMPLE_RATE = 8000,
	SIPPYMODEM_V21_BAUD = 300,
	SIPPYMODEM_V21_DETECTOR_WINDOW = 27,
};

enum sippymodem_v21_channel {
	SIPPYMODEM_V21_CHANNEL_1,
	SIPPYMODEM_V21_CHANNEL_2,
};

enum sippymodem_v21_result {
	SIPPYMODEM_V21_OK,
	SIPPYMODEM_V21_INVALID_ARGUMENT,
};

struct sippymodem_v21_transmitter {
	enum sippymodem_v21_channel channel;
	uint32_t mark_phase_step;
	uint32_t space_phase_step;
	uint32_t carrier_phase;
	uint32_t baud_accumulator;
	uint8_t held_bit;
	bool holding_bit;
};

struct sippymodem_v21_tone_projection {
	int16_t in_phase[SIPPYMODEM_V21_DETECTOR_WINDOW];
	int16_t quadrature[SIPPYMODEM_V21_DETECTOR_WINDOW];
};

struct sippymodem_v21_receiver {
	enum sippymodem_v21_channel channel;
	struct sippymodem_v21_tone_projection mark;
	struct sippymodem_v21_tone_projection space;
	int16_t history[SIPPYMODEM_V21_DETECTOR_WINDOW];
	size_t history_index;
	size_t history_count;
	int64_t mark_power;
	int64_t space_power;
	uint64_t line_power;
	int64_t smoothed_metric;
	uint32_t samples_per_bit_q16;
	uint32_t sample_countdown_q16;
	uint64_t emitted_bits;
	uint8_t last_bit;
	bool timing_locked;
};

struct sippymodem_v21_measurement {
	uint64_t mark_power;
	uint64_t space_power;
	uint64_t line_power;
	uint32_t dominance_q8;
	bool ready;
};

enum sippymodem_v21_result sippymodem_v21_transmitter_init(
	struct sippymodem_v21_transmitter *transmitter,
	enum sippymodem_v21_channel channel);
/* The input bit is latched at a symbol boundary. bit_consumed becomes true on
 * the final generated sample for that bit. */
enum sippymodem_v21_result sippymodem_v21_transmitter_sample(
	struct sippymodem_v21_transmitter *transmitter,
	uint8_t bit,
	int16_t *sample,
	bool *bit_consumed);

enum sippymodem_v21_result sippymodem_v21_receiver_init(
	struct sippymodem_v21_receiver *receiver,
	enum sippymodem_v21_channel channel);
void sippymodem_v21_receiver_reset(
	struct sippymodem_v21_receiver *receiver);
enum sippymodem_v21_result sippymodem_v21_receiver_sample(
	struct sippymodem_v21_receiver *receiver,
	int16_t sample,
	uint8_t *bit,
	bool *bit_ready);
struct sippymodem_v21_measurement sippymodem_v21_receiver_measure(
	const struct sippymodem_v21_receiver *receiver);

const char *sippymodem_v21_result_name(
	enum sippymodem_v21_result result);

#endif
