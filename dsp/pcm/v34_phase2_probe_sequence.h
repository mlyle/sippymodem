// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_H
#define SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_H

#include "v34_phase2_probe.h"

#include <stdbool.h>
#include <stdint.h>

enum sippymodem_v34_phase2_probe_sequence_phase {
	SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_PHASE_L1,
	SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_PHASE_L2,
	SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_PHASE_COMPLETE,
	SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_PHASE_LIMIT_REACHED,
};

enum sippymodem_v34_phase2_probe_sequence_event_kind {
	SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_EVENT_NONE,
	SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_EVENT_L2_STARTED,
	SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_EVENT_COMPLETED,
	SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_EVENT_LIMIT_REACHED,
};

struct sippymodem_v34_phase2_probe_sequence_event {
	enum sippymodem_v34_phase2_probe_sequence_event_kind kind;
	uint32_t l2_samples;
};

enum sippymodem_v34_phase2_probe_sequence_result {
	SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_OK,
	SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_INVALID_ARGUMENT,
};

struct sippymodem_v34_phase2_probe_sequence {
	struct sippymodem_v34_phase2_probe_transmitter waveform;
	enum sippymodem_v34_phase2_probe_sequence_phase phase;
	uint32_t samples_emitted;
	uint16_t round_trip_delay_samples;
};

/* The sequence emits the mandatory 160 ms L1 and then L2 until its owner
 * reports that line measurement is complete. Independently, it limits L2 to
 * 550 ms plus the measured round-trip delay. */
enum sippymodem_v34_phase2_probe_sequence_result
sippymodem_v34_phase2_probe_sequence_init(
	struct sippymodem_v34_phase2_probe_sequence *sequence,
	int16_t nominal_amplitude, uint16_t round_trip_delay_samples);
bool sippymodem_v34_phase2_probe_sequence_is_valid(
	const struct sippymodem_v34_phase2_probe_sequence *sequence);

/* finish_l2 is invalid during L1 and before the first L2 sample. A successful
 * finish call emits silence and reports the exact number of completed L2
 * samples. Complete and limit-reached sequences continue to emit silence. */
enum sippymodem_v34_phase2_probe_sequence_result
sippymodem_v34_phase2_probe_sequence_sample(
	struct sippymodem_v34_phase2_probe_sequence *sequence,
	bool finish_l2, int16_t *sample,
	struct sippymodem_v34_phase2_probe_sequence_event *event);

const char *sippymodem_v34_phase2_probe_sequence_result_name(
	enum sippymodem_v34_phase2_probe_sequence_result result);

#endif
