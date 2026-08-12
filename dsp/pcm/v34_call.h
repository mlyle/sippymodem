// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_CALL_H
#define V34_CALL_H

#include "v34_phase2.h"
#include "v34_phase3_exchange.h"
#include "v34_phase4_exchange.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_call_stage {
	V34_CALL_PHASE2,
	V34_CALL_PHASE3,
	V34_CALL_PHASE4,
};

enum v34_call_result {
	V34_CALL_OK,
	V34_CALL_SOURCE_FAILURE,
	V34_CALL_FAILED,
	V34_CALL_INVALID_ARGUMENT,
	V34_CALL_INVALID_STATE,
	V34_CALL_NO_COMMON_RATE,
	V34_CALL_OUT_OF_RANGE,
	V34_CALL_CLIPPING_RISK,
	V34_CALL_SEQUENCE_MISMATCH,
	V34_CALL_NUMERIC_FAILURE,
};

union v34_local_info1 {
	struct v34_info1c calling;
	struct v34_info1a answering;
};

struct v34_call_configuration {
	enum v34_station_role local_role;
	int16_t phase2_nominal_amplitude;
	struct v34_info0 local_info0;
	/* The role selects calling INFO1c or answering INFO1a. The records are
	 * explicit so measurement-driven selection policy remains outside the
	 * waveform owner. */
	union v34_local_info1 local_info1;
	struct v34_phase3_exchange_configuration phase3;
	struct v34_phase4_exchange_configuration phase4;
};

union v34_call_state {
	struct v34_phase2 phase2;
	struct v34_phase3_exchange phase3;
	struct v34_phase4_exchange phase4;
};

/* One observation describes the owner that consumed the sample and the owner
 * of the following sample. Only the union member selected by `processed` is
 * meaningful. This compact boundary is suitable for recording call progress
 * without retaining sample vectors in the controller. */
struct v34_call_observation {
	enum v34_call_stage processed;
	enum v34_call_stage next;
	union {
		struct v34_phase2_event phase2;
		uint32_t phase3;
		uint32_t phase4;
	} events;
};

/* Own one continuous post-V.8 V.34 sample timeline. Phase storage is reused at
 * exact handoff boundaries. Calls on one instance must be serialized. */
struct v34_call {
	union v34_call_state state;
	union v34_local_info1 local_info1;
	struct v34_phase3_exchange_configuration phase3_configuration;
	struct v34_phase4_exchange_configuration phase4_configuration;
	enum v34_station_role local_role;
	enum v34_call_stage stage;
	bool failed;
};

enum v34_call_result v34_call_init(
	struct v34_call *call,
	const struct v34_call_configuration *configuration);

bool v34_call_is_valid(const struct v34_call *call);
bool v34_call_has_failed(const struct v34_call *call);
bool v34_call_is_online(const struct v34_call *call);
enum v34_call_stage v34_call_current_stage(const struct v34_call *call);

/* Consume and produce one simultaneous 8 kHz sample. The mapping source is
 * ignored before Phase 4 and NULL deliberately sends idle ones afterward.
 * `received` changes only when the Phase 4 observation contains
 * MAPPING_FRAME_RECEIVED. SOURCE_FAILURE consumes nothing and changes no
 * output. Terminal line failures consume the sample and expose its outputs. */
enum v34_call_result v34_call_sample(
	struct v34_call *call, int16_t received_sample,
	v34_mapping_frame_source source, void *source_context,
	int16_t *transmit_sample, struct v34_call_observation *observation,
	struct v34_received_mapping_frame *received);

const char *v34_call_result_name(enum v34_call_result result);

#endif /* V34_CALL_H */
