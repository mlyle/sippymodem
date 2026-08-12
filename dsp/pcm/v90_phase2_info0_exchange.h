// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE2_INFO0_EXCHANGE_H
#define V90_PHASE2_INFO0_EXCHANGE_H

#include "phase2_info0_exchange.h"
#include "v90_info0.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_modem_role {
	V90_ANALOG_MODEM,
	V90_DIGITAL_MODEM,
};

enum v90_phase2_info0_exchange_result {
	V90_PHASE2_INFO0_EXCHANGE_OK,
	V90_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT,
};

enum v90_peer_info0_event {
	V90_PEER_INFO0_NO_EVENT,
	V90_PEER_INFO0_RECEIVED,
	V90_PEER_INFO0_REJECTED,
};

union v90_info0_record {
	struct v90_info0 analog;
	struct v90_info0d digital;
};

struct v90_phase2_info0_exchange_event {
	enum v90_peer_info0_event peer_info0_event;
	union v90_info0_record peer_info0;
	struct v34_phase2_tone_event tone_event;
};

struct v90_phase2_info0_exchange {
	struct phase2_info0_exchange transport;
	union v90_info0_record peer_info0;
	enum v90_modem_role local_role;
	bool peer_info0_received;
};

/* Initial INFO0 clears its acknowledgment bit. Recovery repetition and
 * acknowledgment belong to the ranging controller. */
enum v90_phase2_info0_exchange_result
v90_phase2_info0_exchange_init_analog(
	struct v90_phase2_info0_exchange *exchange,
	int16_t nominal_amplitude,
	const struct v90_info0 *local_info0a);
enum v90_phase2_info0_exchange_result
v90_phase2_info0_exchange_init_digital(
	struct v90_phase2_info0_exchange *exchange,
	int16_t nominal_amplitude,
	const struct v90_info0d *local_info0d);
bool v90_phase2_info0_exchange_is_valid(
	const struct v90_phase2_info0_exchange *exchange);

enum v90_phase2_info0_exchange_result
v90_phase2_info0_exchange_sample(
	struct v90_phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct v90_phase2_info0_exchange_event *event);

const char *v90_phase2_info0_exchange_result_name(
	enum v90_phase2_info0_exchange_result result);

#endif
