// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_H
#define SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_H

#include "sippymodem_v34_phase2_probe_receiver.h"
#include "sippymodem_v34_phase2_probe_synchronizer.h"

#include <stdbool.h>
#include <stdint.h>

enum sippymodem_v34_phase2_probe_acquisition_phase {
	SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_SEARCHING,
	SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_RECEIVING,
	SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_NOT_FOUND,
	SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_COMPLETE,
	SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_LIMIT_REACHED,
};

enum sippymodem_v34_phase2_probe_acquisition_event_kind {
	SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_NONE,
	SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_ACQUIRED,
	SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_NOT_FOUND,
	SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_L2_STARTED,
	SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_MEASUREMENT_READY,
	SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_COMPLETED,
	SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_LIMIT_REACHED,
};

struct sippymodem_v34_phase2_probe_acquisition_event {
	enum sippymodem_v34_phase2_probe_acquisition_event_kind kind;
	uint16_t leading_samples;
	uint16_t replay_samples;
	double normalized_correlation;
	uint32_t l2_samples;
};

enum sippymodem_v34_phase2_probe_acquisition_result {
	SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_OK,
	SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_INVALID_ARGUMENT,
};

struct sippymodem_v34_phase2_probe_acquisition {
	struct sippymodem_v34_phase2_probe_synchronizer synchronizer;
	struct sippymodem_v34_phase2_probe_receiver receiver;
	enum sippymodem_v34_phase2_probe_acquisition_phase phase;
};

/* Begin within the bounded pre-L1 handoff interval. Acquisition retains and
 * replays the synchronization prefix internally, so every sample from the
 * selected edge contributes to the exact receive windows. */
enum sippymodem_v34_phase2_probe_acquisition_result
sippymodem_v34_phase2_probe_acquisition_init(
	struct sippymodem_v34_phase2_probe_acquisition *acquisition);
bool sippymodem_v34_phase2_probe_acquisition_is_valid(
	const struct sippymodem_v34_phase2_probe_acquisition *acquisition);

/* finish_l2 is accepted only after MEASUREMENT_READY. The completion call does
 * not consume received_sample, allowing the next protocol owner to receive
 * the first sample following L2. Terminal phases are idempotent. */
enum sippymodem_v34_phase2_probe_acquisition_result
sippymodem_v34_phase2_probe_acquisition_sample(
	struct sippymodem_v34_phase2_probe_acquisition *acquisition,
	int16_t received_sample, bool finish_l2,
	struct sippymodem_v34_phase2_probe_acquisition_event *event);

enum sippymodem_v34_phase2_probe_acquisition_result
sippymodem_v34_phase2_probe_acquisition_measure(
	const struct sippymodem_v34_phase2_probe_acquisition *acquisition,
	struct sippymodem_v34_phase2_probe_receiver_measurement *measurement);

const char *sippymodem_v34_phase2_probe_acquisition_result_name(
	enum sippymodem_v34_phase2_probe_acquisition_result result);

#endif
