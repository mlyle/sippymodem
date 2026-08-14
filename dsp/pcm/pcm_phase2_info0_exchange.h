// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_PHASE2_INFO0_EXCHANGE_H
#define PCM_PHASE2_INFO0_EXCHANGE_H

#include "pcm_protocol.h"
#include "phase2_info0_exchange.h"
#include "v92_info0.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_modem_role {
	V90_ANALOG_MODEM,
	V90_DIGITAL_MODEM,
};

enum pcm_phase2_info0_exchange_result {
	PCM_PHASE2_INFO0_EXCHANGE_OK,
	PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT,
};

enum pcm_peer_info0_event {
	PCM_PEER_INFO0_NO_EVENT,
	PCM_PEER_INFO0_RECEIVED,
	PCM_PEER_INFO0_REJECTED,
};

union pcm_info0_record {
	struct v90_info0 analog;
	struct v90_info0d digital;
	struct v92_info0a v92_analog;
	struct v92_info0d v92_digital;
};

struct pcm_phase2_info0_exchange_event {
	enum pcm_peer_info0_event peer_info0_event;
	union pcm_info0_record peer_info0;
	struct v34_phase2_tone_event tone_event;
	enum pcm_protocol protocol;
};

struct pcm_phase2_info0_exchange {
	struct phase2_info0_exchange transport;
	union pcm_info0_record peer_info0;
	enum v90_modem_role local_role;
	enum pcm_protocol protocol;
	bool peer_info0_received;
};

/* Initial INFO0 clears its acknowledgment bit. V.90 emits reserved extension
 * bits as zero; V.92 carries its capability and short-Phase-2 request. The
 * protocol tag is immutable for the exchange. Recovery repetition and
 * acknowledgment belong to the ranging controller. */
enum pcm_phase2_info0_exchange_result
pcm_phase2_info0_exchange_init_v90_analog(
	struct pcm_phase2_info0_exchange *exchange,
	int16_t nominal_amplitude,
	const struct v90_info0 *local_info0a);
enum pcm_phase2_info0_exchange_result
pcm_phase2_info0_exchange_init_v90_digital(
	struct pcm_phase2_info0_exchange *exchange,
	int16_t nominal_amplitude,
	const struct v90_info0d *local_info0d);
enum pcm_phase2_info0_exchange_result
pcm_phase2_info0_exchange_init_v92_analog(
	struct pcm_phase2_info0_exchange *exchange,
	int16_t nominal_amplitude,
	const struct v92_info0a *local_info0a);
enum pcm_phase2_info0_exchange_result
pcm_phase2_info0_exchange_init_v92_digital(
	struct pcm_phase2_info0_exchange *exchange,
	int16_t nominal_amplitude,
	const struct v92_info0d *local_info0d);
bool pcm_phase2_info0_exchange_is_valid(
	const struct pcm_phase2_info0_exchange *exchange);

enum pcm_phase2_info0_exchange_result
pcm_phase2_info0_exchange_sample(
	struct pcm_phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct pcm_phase2_info0_exchange_event *event);

const char *pcm_phase2_info0_exchange_result_name(
	enum pcm_phase2_info0_exchange_result result);

#endif
