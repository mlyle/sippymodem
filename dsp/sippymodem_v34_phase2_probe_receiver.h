// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_H
#define SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_H

#include "sippymodem_v34_phase2_probe_spectrum.h"

#include <stdbool.h>
#include <stdint.h>

enum sippymodem_v34_phase2_probe_receiver_phase {
	SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_MEASURE_L1,
	SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_MEASURE_L2,
	SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_WAIT_L2_END,
	SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_COMPLETE,
	SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_LIMIT_REACHED,
};

enum sippymodem_v34_phase2_probe_receiver_event_kind {
	SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_EVENT_NONE,
	SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_EVENT_L2_STARTED,
	SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_EVENT_MEASUREMENT_READY,
	SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_EVENT_COMPLETED,
	SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_EVENT_LIMIT_REACHED,
};

struct sippymodem_v34_phase2_probe_receiver_event {
	enum sippymodem_v34_phase2_probe_receiver_event_kind kind;
	uint32_t l2_samples;
};

enum sippymodem_v34_phase2_probe_receiver_result {
	SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_OK,
	SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT,
};

struct sippymodem_v34_phase2_probe_receiver {
	struct sippymodem_v34_phase2_probe_spectrum l1_spectrum;
	struct sippymodem_v34_phase2_probe_spectrum l2_spectrum;
	enum sippymodem_v34_phase2_probe_receiver_phase phase;
	uint32_t l2_samples;
};

struct sippymodem_v34_phase2_probe_receiver_measurement {
	struct sippymodem_v34_phase2_probe_spectrum_measurement l1;
	struct sippymodem_v34_phase2_probe_spectrum_measurement l2;
	uint32_t l2_samples;
};

/* The first sample passed after initialization must be the synchronized first
 * L1 sample. The receiver measures one exact 160 ms window at each level,
 * then waits for its protocol owner to report the end of L2. Independently,
 * it enforces the 500 ms receive limit. */
enum sippymodem_v34_phase2_probe_receiver_result
sippymodem_v34_phase2_probe_receiver_init(
	struct sippymodem_v34_phase2_probe_receiver *receiver);
bool sippymodem_v34_phase2_probe_receiver_is_valid(
	const struct sippymodem_v34_phase2_probe_receiver *receiver);

/* finish_l2 is valid only after the fixed L2 measurement is ready. That call
 * does not count received_sample as part of L2, allowing the owner to hand the
 * first sample of the following signal to its next receiver. */
enum sippymodem_v34_phase2_probe_receiver_result
sippymodem_v34_phase2_probe_receiver_sample(
	struct sippymodem_v34_phase2_probe_receiver *receiver,
	int16_t received_sample, bool finish_l2,
	struct sippymodem_v34_phase2_probe_receiver_event *event);

/* Measurement is available from MEASUREMENT_READY onward. Both spectra span
 * whole probe periods, so their complex amplitudes use exact normalization. */
enum sippymodem_v34_phase2_probe_receiver_result
sippymodem_v34_phase2_probe_receiver_measure(
	const struct sippymodem_v34_phase2_probe_receiver *receiver,
	struct sippymodem_v34_phase2_probe_receiver_measurement *measurement);

const char *sippymodem_v34_phase2_probe_receiver_result_name(
	enum sippymodem_v34_phase2_probe_receiver_result result);

#endif
