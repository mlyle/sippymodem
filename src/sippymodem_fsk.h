// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_FSK_H
#define SIPPYMODEM_FSK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	/* One bounded receiver can cover a 75-baud symbol at 8 kHz. */
	SIPPYMODEM_FSK_MAX_DETECTOR_WINDOW = 128,
};

struct sippymodem_fsk_channel {
	uint32_t sample_rate;
	uint32_t baud;
	uint32_t mark_frequency_hz;
	uint32_t space_frequency_hz;
	size_t detector_window;
	unsigned int metric_smoothing_shift;
};

enum sippymodem_fsk_result {
	SIPPYMODEM_FSK_OK,
	SIPPYMODEM_FSK_INVALID_ARGUMENT,
};

struct sippymodem_fsk_transmitter {
	struct sippymodem_fsk_channel channel;
	uint32_t mark_phase_step;
	uint32_t space_phase_step;
	uint32_t carrier_phase;
	uint64_t baud_accumulator;
	int16_t amplitude;
	uint8_t held_bit;
	bool holding_bit;
};

struct sippymodem_fsk_tone_projection {
	int16_t in_phase[SIPPYMODEM_FSK_MAX_DETECTOR_WINDOW];
	int16_t quadrature[SIPPYMODEM_FSK_MAX_DETECTOR_WINDOW];
};

struct sippymodem_fsk_receiver {
	struct sippymodem_fsk_channel channel;
	struct sippymodem_fsk_tone_projection mark;
	struct sippymodem_fsk_tone_projection space;
	int16_t history[SIPPYMODEM_FSK_MAX_DETECTOR_WINDOW];
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

struct sippymodem_fsk_measurement {
	uint64_t mark_power;
	uint64_t space_power;
	uint64_t line_power;
	uint32_t dominance_q8;
	bool ready;
};

enum sippymodem_fsk_result sippymodem_fsk_transmitter_init(
	struct sippymodem_fsk_transmitter *transmitter,
	const struct sippymodem_fsk_channel *channel,
	int16_t amplitude);
bool sippymodem_fsk_transmitter_can_accept_bit(
	const struct sippymodem_fsk_transmitter *transmitter);
/* The input bit is latched at a symbol boundary. bit_consumed becomes true on
 * the final generated sample for the currently latched bit. */
enum sippymodem_fsk_result sippymodem_fsk_transmitter_sample(
	struct sippymodem_fsk_transmitter *transmitter,
	uint8_t bit,
	int16_t *sample,
	bool *bit_consumed);

enum sippymodem_fsk_result sippymodem_fsk_receiver_init(
	struct sippymodem_fsk_receiver *receiver,
	const struct sippymodem_fsk_channel *channel);
void sippymodem_fsk_receiver_reset(
	struct sippymodem_fsk_receiver *receiver);
enum sippymodem_fsk_result sippymodem_fsk_receiver_sample(
	struct sippymodem_fsk_receiver *receiver,
	int16_t sample,
	uint8_t *bit,
	bool *bit_ready);
struct sippymodem_fsk_measurement sippymodem_fsk_receiver_measure(
	const struct sippymodem_fsk_receiver *receiver);

const char *sippymodem_fsk_result_name(
	enum sippymodem_fsk_result result);

#endif
