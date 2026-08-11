// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE2_PROBE_SEQUENCE_H
#define V34_PHASE2_PROBE_SEQUENCE_H

#include "v34_phase2_probe.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_phase2_probe_sequence_phase {
	V34_PHASE2_PROBE_SEQUENCE_PHASE_L1,
	V34_PHASE2_PROBE_SEQUENCE_PHASE_L2,
	V34_PHASE2_PROBE_SEQUENCE_PHASE_COMPLETE,
	V34_PHASE2_PROBE_SEQUENCE_PHASE_LIMIT_REACHED,
};

enum v34_phase2_probe_sequence_event_kind {
	V34_PHASE2_PROBE_SEQUENCE_EVENT_NONE,
	V34_PHASE2_PROBE_SEQUENCE_EVENT_L2_STARTED,
	V34_PHASE2_PROBE_SEQUENCE_EVENT_COMPLETED,
	V34_PHASE2_PROBE_SEQUENCE_EVENT_LIMIT_REACHED,
};

struct v34_phase2_probe_sequence_event {
	enum v34_phase2_probe_sequence_event_kind kind;
	uint32_t l2_samples;
};

enum v34_phase2_probe_sequence_result {
	V34_PHASE2_PROBE_SEQUENCE_OK,
	V34_PHASE2_PROBE_SEQUENCE_INVALID_ARGUMENT,
};

struct v34_phase2_probe_sequence {
	struct v34_phase2_probe_transmitter waveform;
	enum v34_phase2_probe_sequence_phase phase;
	uint32_t samples_emitted;
	uint16_t round_trip_delay_samples;
};

/* The sequence emits the mandatory 160 ms L1 and then L2 until its owner
 * reports that line measurement is complete. Independently, it limits L2 to
 * 550 ms plus the measured round-trip delay. */
enum v34_phase2_probe_sequence_result
v34_phase2_probe_sequence_init(
	struct v34_phase2_probe_sequence *sequence,
	int16_t nominal_amplitude, uint16_t round_trip_delay_samples);
bool v34_phase2_probe_sequence_is_valid(
	const struct v34_phase2_probe_sequence *sequence);

/* finish_l2 is invalid during L1 and before the first L2 sample. A successful
 * finish call emits silence and reports the exact number of completed L2
 * samples. Complete and limit-reached sequences continue to emit silence. */
enum v34_phase2_probe_sequence_result
v34_phase2_probe_sequence_sample(
	struct v34_phase2_probe_sequence *sequence,
	bool finish_l2, int16_t *sample,
	struct v34_phase2_probe_sequence_event *event);

const char *v34_phase2_probe_sequence_result_name(
	enum v34_phase2_probe_sequence_result result);

#endif
