// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_SU_RX_H
#define V92_SU_RX_H

#include <stdbool.h>
#include <stdint.h>

enum {
	V92_SU_RX_MIN_STABLE_SYMBOLS = 16,
	V92_SU_RX_PHASE_SYMBOLS = 96,
	V92_SU_RX_PHASE_SETTLE_SYMBOLS = 60,
};

enum v92_su_rx_mode {
	V92_SU_RX_TRANSITION_ONLY,
	V92_SU_RX_MEASURE_PHASE,
};

enum v92_su_rx_stage {
	V92_SU_RX_ACQUIRING,
	V92_SU_RX_FIRST_PHASE,
	V92_SU_RX_WAITING_TRANSITION,
	V92_SU_RX_SECOND_PHASE,
	V92_SU_RX_COMPLETE,
	V92_SU_RX_FAILED,
};

enum v92_su_rx_result {
	V92_SU_RX_OK,
	V92_SU_RX_ACQUIRED,
	V92_SU_RX_TRANSITION,
	V92_SU_RX_PHASE_READY,
	V92_SU_RX_DONE,
	V92_SU_RX_FAILURE,
	V92_SU_RX_INVALID_ARGUMENT,
	V92_SU_RX_INVALID_STATE,
};

/* Detect Su from its six-symbol periodicity rather than from an absolute
 * waveform phase. In phase-measurement mode the receiver retains one
 * contiguous probe window on each side of the specified half-symbol change
 * and derives the fractional adjustment carried by Jp. The same component in
 * transition-only mode detects the later response that terminates Jp. */
struct v92_su_rx {
	int32_t phase_component[2];
	uint64_t total_symbols;
	uint64_t last_periodic_symbol;
	uint64_t transition_symbol;
	uint32_t stable_symbols;
	uint16_t phase_samples[2];
	uint16_t phase_extension_q16;
	uint16_t peak;
	uint16_t minimum_magnitude;
	enum v92_su_rx_mode mode;
	enum v92_su_rx_stage stage;
	uint8_t history_signs;
	uint8_t history_valid;
	uint8_t comparison_same;
	uint8_t comparison_opposite;
	uint8_t comparison_valid;
};

enum v92_su_rx_result v92_su_rx_init(
	struct v92_su_rx *receiver, enum v92_su_rx_mode mode,
	uint16_t minimum_magnitude);
bool v92_su_rx_is_valid(const struct v92_su_rx *receiver);
bool v92_su_rx_is_complete(const struct v92_su_rx *receiver);
bool v92_su_rx_has_failed(const struct v92_su_rx *receiver);

/* Consume one signed 8 kHz symbol. A detected transition is consumed. Invalid
 * and terminal calls leave the receiver unchanged. */
enum v92_su_rx_result v92_su_rx_sample(
	struct v92_su_rx *receiver, int16_t sample);

/* Available only after a successful phase-measurement run. */
bool v92_su_rx_phase_extension(
	const struct v92_su_rx *receiver, uint16_t *extension_q16);

/* Convert the two measured 4 kHz components into the Jp field. This pure
 * helper is public so phase estimators and compact generated tests share one
 * definition without sharing detector state. */
bool v92_su_phase_extension(
	int32_t first_component, int32_t second_component,
	uint16_t *extension_q16);

const char *v92_su_rx_stage_name(enum v92_su_rx_stage stage);
const char *v92_su_rx_result_name(enum v92_su_rx_result result);

#endif /* V92_SU_RX_H */
