// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE2_DPSK_H
#define SIPPYMODEM_V34_PHASE2_DPSK_H

#include "sippymodem_qam.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_PHASE2_DPSK_SAMPLE_RATE = 8000,
	SIPPYMODEM_V34_PHASE2_DPSK_BIT_RATE = 600,
	SIPPYMODEM_V34_PHASE2_CALLING_CARRIER_HZ = 1200,
	SIPPYMODEM_V34_PHASE2_ANSWERING_GUARD_HZ = 1800,
	SIPPYMODEM_V34_PHASE2_ANSWERING_CARRIER_HZ = 2400,
};

enum sippymodem_v34_phase2_station_role {
	SIPPYMODEM_V34_PHASE2_CALLING_STATION,
	SIPPYMODEM_V34_PHASE2_ANSWERING_STATION,
};

enum sippymodem_v34_phase2_dpsk_result {
	SIPPYMODEM_V34_PHASE2_DPSK_OK,
	SIPPYMODEM_V34_PHASE2_DPSK_INVALID_ARGUMENT,
};

struct sippymodem_v34_phase2_dpsk_transmitter {
	struct sippymodem_qam_modulator carrier;
	uint32_t guard_phase;
	uint32_t guard_phase_step;
	int16_t guard_amplitude;
	enum sippymodem_v34_phase2_station_role local_role;
	bool point_inverted;
	bool initial_point_pending;
};

struct sippymodem_v34_phase2_dpsk_receiver {
	struct sippymodem_qam_demodulator carrier;
	struct sippymodem_qam_point previous_point;
	enum sippymodem_v34_phase2_station_role local_role;
	bool has_previous_point;
};

/* nominal_amplitude is the unadjusted Phase 2 peak level. The answering
 * carrier is reduced by 1 dB and its guard tone by 7 dB from this level. */
enum sippymodem_v34_phase2_dpsk_result
sippymodem_v34_phase2_dpsk_transmitter_init(
	struct sippymodem_v34_phase2_dpsk_transmitter *transmitter,
	enum sippymodem_v34_phase2_station_role local_role,
	int16_t nominal_amplitude);
bool sippymodem_v34_phase2_dpsk_transmitter_is_valid(
	const struct sippymodem_v34_phase2_dpsk_transmitter *transmitter);
bool sippymodem_v34_phase2_dpsk_transmitter_can_accept_bit(
	const struct sippymodem_v34_phase2_dpsk_transmitter *transmitter);

/* Keep presenting a bit until bit_accepted becomes true. The first accepted
 * symbol is the arbitrary reference point and does not consume a bit. */
enum sippymodem_v34_phase2_dpsk_result
sippymodem_v34_phase2_dpsk_transmitter_sample(
	struct sippymodem_v34_phase2_dpsk_transmitter *transmitter,
	uint8_t bit, int16_t *sample, bool *bit_accepted);

/* local_role selects the opposite station's carrier. The first recovered
 * point establishes the differential reference and does not produce a bit. */
enum sippymodem_v34_phase2_dpsk_result
sippymodem_v34_phase2_dpsk_receiver_init(
	struct sippymodem_v34_phase2_dpsk_receiver *receiver,
	enum sippymodem_v34_phase2_station_role local_role);
enum sippymodem_v34_phase2_dpsk_result
sippymodem_v34_phase2_dpsk_receiver_reset(
	struct sippymodem_v34_phase2_dpsk_receiver *receiver);
enum sippymodem_v34_phase2_dpsk_result
sippymodem_v34_phase2_dpsk_receiver_sample(
	struct sippymodem_v34_phase2_dpsk_receiver *receiver,
	int16_t sample, uint8_t *bit, bool *bit_ready);
struct sippymodem_qam_measurement
sippymodem_v34_phase2_dpsk_receiver_measure(
	const struct sippymodem_v34_phase2_dpsk_receiver *receiver);

const char *sippymodem_v34_phase2_dpsk_result_name(
	enum sippymodem_v34_phase2_dpsk_result result);

#endif
