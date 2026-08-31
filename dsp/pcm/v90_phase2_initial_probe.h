// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE2_INITIAL_PROBE_H
#define V90_PHASE2_INITIAL_PROBE_H

#include "phase2_initial_probe.h"
#include "v90_phase2_ranging.h"

#include <stdbool.h>
#include <stdint.h>

struct v90_phase2_initial_probe {
	struct v90_phase2_ranging ranging;
	struct phase2_initial_probe control;
};

/* Continue after the role-appropriate V.90 ranging event. The analog modem
 * begins L1 on the next sample and the digital modem begins bounded probe
 * acquisition on the next received sample. */
enum phase2_initial_probe_result v90_phase2_initial_probe_init(
	struct v90_phase2_initial_probe *probe,
	const struct v90_phase2_ranging *completed_ranging);
bool v90_phase2_initial_probe_is_valid(
	const struct v90_phase2_initial_probe *probe);

enum phase2_initial_probe_result v90_phase2_initial_probe_sample(
	struct v90_phase2_initial_probe *probe,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_initial_probe_event *event);

enum phase2_initial_probe_result v90_phase2_initial_probe_measure(
	const struct v90_phase2_initial_probe *probe,
	struct v34_phase2_probe_receiver_measurement *measurement);
enum phase2_initial_probe_result v90_phase2_initial_probe_receive_gain(
	const struct v90_phase2_initial_probe *probe, int16_t *receive_gain);

#endif
