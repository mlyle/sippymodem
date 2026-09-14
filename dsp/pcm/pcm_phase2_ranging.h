// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_PHASE2_RANGING_H
#define PCM_PHASE2_RANGING_H

#include "phase2_ranging.h"
#include "pcm_phase2_info0_exchange.h"

#include <stdbool.h>
#include <stdint.h>

enum pcm_phase2_ranging_result {
	PCM_PHASE2_RANGING_OK,
	PCM_PHASE2_RANGING_INVALID_ARGUMENT,
};

struct pcm_phase2_ranging_event {
	struct pcm_phase2_info0_exchange_event exchange_event;
	enum phase2_ranging_event_kind kind;
	uint16_t round_trip_delay_samples;
};

struct pcm_phase2_ranging {
	struct pcm_phase2_info0_exchange info0_exchange;
	struct phase2_ranging_control control;
};

/* V.90 assigns Signal A to the analog modem and Signal B to the digital
 * modem. These endpoints retain the mode-specific INFO0 records while using
 * the shared Signal A/B reversal and round-trip controller. */
enum pcm_phase2_ranging_result pcm_phase2_ranging_init_analog(
	struct pcm_phase2_ranging *ranging,
	int16_t nominal_amplitude,
	const struct v90_info0 *local_info0a);
enum pcm_phase2_ranging_result pcm_phase2_ranging_init_digital(
	struct pcm_phase2_ranging *ranging,
	int16_t nominal_amplitude,
	const struct v90_info0d *local_info0d);
bool pcm_phase2_ranging_is_valid(
	const struct pcm_phase2_ranging *ranging);

/* One call consumes and produces one 8 kHz line-terminal sample. A readiness
 * event hands the following sample to the first line-probing procedure. */
enum pcm_phase2_ranging_result pcm_phase2_ranging_sample(
	struct pcm_phase2_ranging *ranging,
	int16_t received_sample, int16_t *transmit_sample,
	struct pcm_phase2_ranging_event *event);

const char *pcm_phase2_ranging_result_name(
	enum pcm_phase2_ranging_result result);

#endif
