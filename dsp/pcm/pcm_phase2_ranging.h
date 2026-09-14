// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_PHASE2_RANGING_H
#define PCM_PHASE2_RANGING_H

#include "phase2_ranging.h"
#include "pcm_phase2_info0_exchange.h"
#include "v92_short_ranging.h"

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
	struct v92_short_ranging_step short_step;
};

enum pcm_ranging_path {
	PCM_RANGING_FULL,
	PCM_RANGING_SHORT_PENDING,
	PCM_RANGING_SHORT_ACTIVE,
	PCM_RANGING_SHORT_FINISHED,
	PCM_RANGING_SHORT_FULL_RECOVERY,
};

struct pcm_phase2_ranging {
	struct pcm_phase2_info0_exchange info0_exchange;
	union {
		struct phase2_ranging_control control;
		struct v92_short_ranging short_control;
	};
	enum pcm_ranging_path path;
	uint16_t actual_tone_samples;
	uint16_t since_info0_samples;
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

/* One call consumes and produces one 8 kHz line-terminal sample. Full ranging
 * hands the following sample to probing. Mutual V.92 short agreement instead
 * reports the INFO1a handoff or recovery action in short_step; no probe or
 * INFO1d is manufactured. The call owner retains its overall training limit. */
enum pcm_phase2_ranging_result pcm_phase2_ranging_sample(
	struct pcm_phase2_ranging *ranging,
	int16_t received_sample, int16_t *transmit_sample,
	struct pcm_phase2_ranging_event *event);

/* After RECEIVE_INFO1A, retain this ranging context while the INFO1a owner
 * consumes short_step.info1a_remaining_samples. On expiry, restart full
 * digital ranging using that retained context and the original INFO0s.
 * This entry point is not an independent timer. False leaves state intact. */
bool pcm_phase2_ranging_recover_short_info1(
	struct pcm_phase2_ranging *ranging);

const char *pcm_phase2_ranging_result_name(
	enum pcm_phase2_ranging_result result);

#endif
