// SPDX-License-Identifier: BSD-3-Clause

#ifndef FSK_H
#define FSK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	/* One bounded receiver can cover a 75-baud symbol at 8 kHz. */
	FSK_MAX_DETECTOR_WINDOW = 128,
};

struct fsk_channel {
	uint32_t sample_rate;
	uint32_t symbol_rate_numerator;
	uint32_t symbol_rate_denominator;
	uint32_t mark_frequency_hz;
	uint32_t space_frequency_hz;
	size_t detector_window;
	unsigned int metric_smoothing_shift;
};

enum fsk_result {
	FSK_OK,
	FSK_INVALID_ARGUMENT,
};

struct fsk_tone_generator {
	uint32_t sample_rate;
	uint32_t mark_phase_step;
	uint32_t space_phase_step;
	uint32_t carrier_phase;
	int16_t amplitude;
};

struct fsk_transmitter {
	struct fsk_channel channel;
	struct fsk_tone_generator tones;
	uint64_t symbol_accumulator;
	uint8_t held_bit;
	bool holding_bit;
};

struct fsk_tone_projection {
	int16_t in_phase[FSK_MAX_DETECTOR_WINDOW];
	int16_t quadrature[FSK_MAX_DETECTOR_WINDOW];
};

struct fsk_receiver {
	struct fsk_channel channel;
	struct fsk_tone_projection mark;
	struct fsk_tone_projection space;
	int16_t history[FSK_MAX_DETECTOR_WINDOW];
	size_t history_index;
	size_t history_count;
	int64_t mark_power;
	int64_t space_power;
	uint64_t line_power;
	int64_t smoothed_metric;
	uint32_t samples_per_symbol_q16;
	uint32_t sample_countdown_q16;
	uint64_t emitted_bits;
	uint8_t last_bit;
	bool timing_locked;
};

struct fsk_measurement {
	uint64_t mark_power;
	uint64_t space_power;
	uint64_t line_power;
	uint32_t dominance_q8;
	bool ready;
};

/* The measurement is shared; each mode owns its power and dominance floors. */
static inline bool fsk_measurement_tone_present(
	const struct fsk_measurement *measurement,
	uint64_t minimum_line_power, uint32_t minimum_dominance_q8)
{
	return measurement != NULL && measurement->ready &&
	       measurement->line_power >= minimum_line_power &&
	       measurement->dominance_q8 >= minimum_dominance_q8;
}

static inline bool fsk_measurement_mark_present(
	const struct fsk_measurement *measurement,
	uint64_t minimum_line_power, uint32_t minimum_dominance_q8)
{
	return fsk_measurement_tone_present(
		       measurement, minimum_line_power, minimum_dominance_q8) &&
	       measurement->mark_power > measurement->space_power;
}

enum fsk_result fsk_tone_generator_init(
	struct fsk_tone_generator *generator,
	const struct fsk_channel *channel,
	int16_t amplitude);
enum fsk_result fsk_tone_generator_sample(
	struct fsk_tone_generator *generator,
	uint8_t bit,
	int16_t *sample);

enum fsk_result fsk_transmitter_init(
	struct fsk_transmitter *transmitter,
	const struct fsk_channel *channel,
	int16_t amplitude);
bool fsk_transmitter_can_accept_bit(
	const struct fsk_transmitter *transmitter);
/* The input bit is latched at a symbol boundary. bit_consumed becomes true on
 * the final generated sample for the currently latched bit. */
enum fsk_result fsk_transmitter_sample(
	struct fsk_transmitter *transmitter,
	uint8_t bit,
	int16_t *sample,
	bool *bit_consumed);

enum fsk_result fsk_receiver_init(
	struct fsk_receiver *receiver,
	const struct fsk_channel *channel);
void fsk_receiver_reset(
	struct fsk_receiver *receiver);
/* Advances only the generated tone-pair detector. Use this when an external
 * asynchronous framer owns sampling instants; use receiver_sample when the
 * FSK receiver should also recover the synchronous symbol clock. */
enum fsk_result fsk_receiver_detect(
	struct fsk_receiver *receiver,
	int16_t sample,
	uint8_t *bit,
	bool *decision_ready);
enum fsk_result fsk_receiver_sample(
	struct fsk_receiver *receiver,
	int16_t sample,
	uint8_t *bit,
	bool *bit_ready);
struct fsk_measurement fsk_receiver_measure(
	const struct fsk_receiver *receiver);

#endif
