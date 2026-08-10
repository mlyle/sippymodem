// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE2_INFO_AUDIO_H
#define SIPPYMODEM_V34_PHASE2_INFO_AUDIO_H

#include "sippymodem_v34_phase2_dpsk.h"
#include "sippymodem_v34_phase2_info.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum sippymodem_v34_phase2_info_audio_result {
	SIPPYMODEM_V34_PHASE2_INFO_AUDIO_OK,
	SIPPYMODEM_V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT,
};

struct sippymodem_v34_phase2_info_audio_transmitter {
	struct sippymodem_v34_phase2_dpsk_transmitter modulation;
	struct sippymodem_v34_phase2_info_generator frame;
	bool frame_complete;
};

/* One transmitter emits one INFO sequence, including its arbitrary reference
 * point. A Phase 2 controller owns any following tone or repeated group. */
enum sippymodem_v34_phase2_info_audio_result
sippymodem_v34_phase2_info_audio_transmitter_init(
	struct sippymodem_v34_phase2_info_audio_transmitter *transmitter,
	enum sippymodem_v34_phase2_station_role local_role,
	int16_t nominal_amplitude, const uint8_t *payload,
	size_t payload_bit_count);
bool sippymodem_v34_phase2_info_audio_transmitter_is_valid(
	const struct sippymodem_v34_phase2_info_audio_transmitter *transmitter);

/* frame_complete is true on the sample that accepts the final sequence bit.
 * Producing another sample from a completed one-shot transmitter is invalid. */
enum sippymodem_v34_phase2_info_audio_result
sippymodem_v34_phase2_info_audio_transmitter_sample(
	struct sippymodem_v34_phase2_info_audio_transmitter *transmitter,
	int16_t *sample, bool *frame_complete);

const char *sippymodem_v34_phase2_info_audio_result_name(
	enum sippymodem_v34_phase2_info_audio_result result);

#endif
