// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_CALL_H
#define V34_CALL_H

#include "v34_phase2.h"
#include "v34_phase3_exchange.h"
#include "v34_phase4_exchange.h"
#include "pcm_host_echo.h"
#include "v34_cleardown.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_call_stage {
	V34_CALL_PHASE2,
	V34_CALL_PHASE3,
	V34_CALL_PHASE4,
	V34_CALL_RETRAIN_SILENCE,
	V34_CALL_CLEARDOWN,
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
	V34_CALL_CLEARDOWN_COMPLETE,
};

union v34_local_info1 {
	struct v34_info1c calling;
	struct v34_info1a answering;
};

struct v34_call_configuration {
	enum v34_station_role local_role;
	int16_t phase2_nominal_amplitude;
	struct v34_info0 local_info0;
	/* The role selects calling INFO1c or answering INFO1a. Rate, carrier,
	 * and projected-rate fields are policy inputs; the call replaces each
	 * active pre-emphasis field from its retained remote probe measurement. */
	union v34_local_info1 local_info1;
	/* The call supplies the measured Phase-2 delay and echo-training policy. */
	struct v34_phase3_exchange_configuration phase3;
	struct v34_phase4_exchange_configuration phase4;
};

union v34_call_state {
	struct v34_phase2 phase2;
	struct v34_phase3_exchange phase3;
	struct v34_phase4_exchange phase4;
	struct v34_cleardown cleardown;
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
	struct pcm_host_echo echo;
	union v34_local_info1 local_info1;
	struct v34_info0 local_info0;
	struct v34_info0 peer_info0;
	struct v34_phase3_exchange_configuration phase3_configuration;
	struct v34_phase4_exchange_configuration phase4_configuration;
	struct v34_full_retrain retrain;
	uint32_t phase4_e_remaining_samples;
	uint32_t echo_quiet_samples;
	uint16_t round_trip_delay_samples;
	int16_t phase2_nominal_amplitude;
	enum v34_station_role local_role;
	enum v34_call_stage stage;
	bool peer_info0_stored;
	bool round_trip_delay_stored;
	bool retrain_active;
	bool failed;
};

enum v34_call_result v34_call_init(
	struct v34_call *call,
	const struct v34_call_configuration *configuration);

enum v34_call_result v34_call_init_from_phase2(
	struct v34_call *call,
	const struct v34_call_configuration *configuration,
	const struct v34_phase2_handoff *handoff);

/* Begin the role-specific full retrain with 70 ms of line silence. The
 * negotiated INFO0 capability records remain owned by the call. */
enum v34_call_result v34_call_start_retrain(struct v34_call *call);

/* Transfer the online line owners to the reciprocal MP-prime cleardown.
 * The outer modem lifecycle supplies the bounded peer-response deadline. */
enum v34_call_result v34_call_start_cleardown(struct v34_call *call);

bool v34_call_is_valid(const struct v34_call *call);
bool v34_call_has_failed(const struct v34_call *call);
bool v34_call_is_online(const struct v34_call *call);

/* Copy the negotiated data directions once Phase 4 has prepared them. */
bool v34_call_get_data_startup(
	const struct v34_call *call, struct v34_data_startup *startup);

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

#endif /* V34_CALL_H */
