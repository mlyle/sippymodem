// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_DOWNSTREAM_CONTROL_H
#define V92_DOWNSTREAM_CONTROL_H

#include "v92_cleardown.h"
#include "v92_control_rx.h"
#include "v92_phase4_cpd_tx.h"

enum v92_downstream_control_stage {
	V92_DOWNSTREAM_CONTROL_INITIAL_SUVD,
	V92_DOWNSTREAM_CONTROL_READY_FOR_CPD,
	V92_DOWNSTREAM_CONTROL_CPD,
	V92_DOWNSTREAM_CONTROL_STATUS_SUVD,
};

/* Digital-side 9.6.1 control exchange, shared by training, renegotiation and
 * fast parameter exchange. The caller supplies decoded upstream records;
 * this owner retains the downstream coder and the entire pending CPd tail.
 * Parameter selection never replaces the active downstream mapping. During
 * fast exchange that mapping is the preceding data mapping, not a training
 * constellation. Initialization discards all previous acknowledgments.
 * One instance is serialized across receive/transmit operations. */
struct v92_downstream_control {
	struct pcm_downstream_tx transmit;
	struct v92_phase4_cpd_tx initial_cpd;
	struct v92_cpu_parameters cpu;
	struct v92_suvu_parameters suvu;
	struct v92_cleardown cleardown;
	uint64_t cpd_start_frame;
	uint64_t status_start_frame;
	uint64_t acknowledgement_end_frame;
	uint16_t round_trip_delay_samples;
	enum v92_downstream_control_stage transmit_stage;
	bool previous_cpu;
	bool request_silence;
	bool suvu_received;
	bool cpu_received;
	bool transmit_acknowledges_cpu;
	bool peer_acknowledged_cpd;
	bool repeat_cpd;
	bool failed;
};

/* previous_cpu permits short CPus in a new exchange; NULL requires long CPu.
 * Its old acknowledgment is cleared. Downstream data-mode entry requires
 * silence clear and the matching previous CPu. Invalid initialization/selection
 * changes nothing. */
bool v92_downstream_control_init(struct v92_downstream_control *control,
	const struct pcm_downstream_tx *downstream,
	const struct v92_suvd_parameters *suvd, const struct v92_cpd_parameters *cpd,
	const struct v92_cpu_parameters *previous_cpu, uint16_t round_trip_delay_samples);
bool v92_downstream_control_is_valid(const struct v92_downstream_control *control);
bool v92_downstream_control_select(struct v92_downstream_control *control,
	const struct v92_cpd_parameters *cpd);
/* Training may defer SUVd until its channel measurement is complete. */
bool v92_downstream_control_ready(struct v92_downstream_control *control);
/* Damaged wire records are ignored, but incompatible selections fail the
 * exchange. Retry expiry waits through a whole valid peer record. */
bool v92_downstream_control_receive(struct v92_downstream_control *control,
	const struct v92_control_event *event);
/* A successful call produces one network octet, unless cleardown has just
 * completed (transmit.complete). Failures leave the output untouched. */
bool v92_downstream_control_transmit(struct v92_downstream_control *control,
	uint8_t *network_octet);
bool v92_downstream_control_start_cleardown(struct v92_downstream_control *control);
/* The receiver may begin E2u while the independent transmitter still drains
 * its current record. The caller uses v92_phase4_ed_tx for that final tail. */
bool v92_downstream_control_is_ed_ready(const struct v92_downstream_control *control);

#endif
