// SPDX-License-Identifier: BSD-3-Clause

#ifndef PHASE2_PROBE_H
#define PHASE2_PROBE_H

#include "v34_phase2_probe_acquisition.h"
#include "v34_phase2_probe_sequence.h"

/* Both probe turns use the same DSP path and failure contract. Their owners
 * retain the role-specific sequencing, acknowledgement, and recovery rules. */
enum phase2_probe_failure {
	PHASE2_PROBE_NO_FAILURE,
	PHASE2_PROBE_REMOTE_NOT_FOUND,
	PHASE2_PROBE_LOCAL_TRANSMIT_LIMIT,
};

union phase2_probe_path {
	struct v34_phase2_probe_acquisition receive;
	struct v34_phase2_probe_sequence transmit;
};

#endif
