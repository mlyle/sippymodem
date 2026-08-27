// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE2_DPSK_H
#define V34_PHASE2_DPSK_H

#include "qam.h"
#include "v34_phase2_signal.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_PHASE2_DPSK_BIT_RATE = 600,
	V34_PHASE2_DPSK_PULSE_SPAN_SYMBOLS = 6,
	V34_PHASE2_DPSK_INPUT_BLOCK = 4,
	V34_PHASE2_DPSK_CARRIER_FILTER_TAPS = 60,
	V34_PHASE2_DPSK_INTERPOLATION = 3,
	V34_PHASE2_DPSK_INTERPOLATOR_TAPS = 12,
	V34_PHASE2_DPSK_DIFFERENTIAL_DELAY =
		V34_PHASE2_SAMPLE_RATE *
		V34_PHASE2_DPSK_INTERPOLATION /
		V34_PHASE2_DPSK_BIT_RATE,
	V34_PHASE2_DPSK_DETECTOR_DECIMATION = 4,
	V34_PHASE2_DPSK_LOWPASS_TAPS = 80,
	V34_PHASE2_DPSK_TIMING_OBSERVATIONS =
		V34_PHASE2_SAMPLE_RATE *
		V34_PHASE2_DPSK_INTERPOLATION /
		(V34_PHASE2_DPSK_BIT_RATE *
		 V34_PHASE2_DPSK_DETECTOR_DECIMATION),
};

enum v34_phase2_dpsk_result {
	V34_PHASE2_DPSK_OK,
	V34_PHASE2_DPSK_INVALID_ARGUMENT,
};

struct v34_phase2_dpsk_transmitter {
	struct qam_modulator carrier;
	uint32_t guard_phase;
	uint32_t guard_phase_step;
	int16_t guard_amplitude;
	enum v34_station_role local_role;
	bool point_inverted;
	bool initial_point_pending;
};

struct v34_phase2_dpsk_receiver {
	int16_t input[V34_PHASE2_DPSK_INPUT_BLOCK];
	int16_t carrier_history[V34_PHASE2_DPSK_CARRIER_FILTER_TAPS];
	int16_t interpolation_history[
		V34_PHASE2_DPSK_INTERPOLATOR_TAPS];
	int16_t delay_history[
		V34_PHASE2_DPSK_DIFFERENTIAL_DELAY];
	int16_t lowpass_history[V34_PHASE2_DPSK_LOWPASS_TAPS];
	double detector_output;
	double line_power;
	double symbol_power;
	size_t delay_index;
	size_t lowpass_index;
	size_t carrier_index;
	uint8_t input_count;
	uint8_t decimation_phase;
	uint8_t observations_until_bit;
	enum v34_station_role local_role;
	bool timing_locked;
};

struct v34_phase2_dpsk_measurement {
	double line_power;
	double symbol_power;
	bool timing_locked;
};

/* nominal_amplitude is the unadjusted Phase 2 peak level. The answering
 * carrier is reduced by 1 dB and its guard tone by 7 dB from this level. */
enum v34_phase2_dpsk_result
v34_phase2_dpsk_transmitter_init(
	struct v34_phase2_dpsk_transmitter *transmitter,
	enum v34_station_role local_role,
	int16_t nominal_amplitude);
bool v34_phase2_dpsk_transmitter_is_valid(
	const struct v34_phase2_dpsk_transmitter *transmitter);
bool v34_phase2_dpsk_transmitter_can_accept_bit(
	const struct v34_phase2_dpsk_transmitter *transmitter);

/* Keep presenting a bit until bit_accepted becomes true. The first accepted
 * symbol is the arbitrary reference point and does not consume a bit. */
enum v34_phase2_dpsk_result
v34_phase2_dpsk_transmitter_sample(
	struct v34_phase2_dpsk_transmitter *transmitter,
	uint8_t bit, int16_t *sample, bool *bit_accepted);

/* local_role selects the opposite station's carrier. The first recovered
 * point establishes the differential reference and does not produce a bit. */
enum v34_phase2_dpsk_result
v34_phase2_dpsk_receiver_init(
	struct v34_phase2_dpsk_receiver *receiver,
	enum v34_station_role local_role);
bool v34_phase2_dpsk_receiver_is_valid(
	const struct v34_phase2_dpsk_receiver *receiver);
enum v34_phase2_dpsk_result
v34_phase2_dpsk_receiver_reset(
	struct v34_phase2_dpsk_receiver *receiver);
enum v34_phase2_dpsk_result
v34_phase2_dpsk_receiver_sample(
	struct v34_phase2_dpsk_receiver *receiver,
	int16_t sample, uint8_t *bit, bool *bit_ready);
struct v34_phase2_dpsk_measurement
v34_phase2_dpsk_receiver_measure(
	const struct v34_phase2_dpsk_receiver *receiver);

#endif
