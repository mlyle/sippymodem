// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_CALL_H
#define V90_CALL_H

#include "v90_phase2.h"
#include "v90_phase3_analog_station.h"
#include "v90_phase3_digital_station.h"
#include "v90_phase4_analog_station.h"
#include "v90_phase4_digital_station.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_call_stage {
	V90_CALL_PHASE2,
	V90_CALL_PHASE3,
	V90_CALL_PHASE3_MIXED,
	V90_CALL_PHASE4,
};

enum v90_call_result {
	V90_CALL_OK,
	V90_CALL_PHASE4_READY,
	V90_CALL_FAILED,
	V90_CALL_INVALID_ARGUMENT,
	V90_CALL_INVALID_STATE,
	V90_CALL_INVALID_SELECTION,
	V90_CALL_UNUSABLE_SELECTION,
	V90_CALL_MEDIA_MISMATCH,
	V90_CALL_OUT_OF_RANGE,
	V90_CALL_CLIPPING_RISK,
	V90_CALL_SEQUENCE_MISMATCH,
	V90_CALL_NUMERIC_FAILURE,
};

union v90_call_local_info0 {
	struct v90_info0 analog;
	struct v90_info0d digital;
};

union v90_call_local_info1 {
	struct v90_info1a analog;
	struct v34_info1c digital;
};

struct v90_call_analog_phase3_configuration {
	int16_t nominal_peak_amplitude;
};

struct v90_call_digital_phase3_configuration {
	struct v90_phase3_analog_rx_configuration receive;
	struct v90_jd_parameters jd;
};

union v90_call_phase3_configuration {
	struct v90_call_analog_phase3_configuration analog;
	struct v90_call_digital_phase3_configuration digital;
};

struct v90_call_analog_phase4_configuration {
	/* Initial CPt sent when the conventional Phase 3 line completes. */
	struct v90_cp_parameters training_parameters;
};

struct v90_call_configuration {
	enum v90_modem_role local_role;
	enum g711_law law;
	int16_t phase2_nominal_amplitude;
	union v90_call_local_info0 local_info0;
	union v90_call_local_info1 local_info1;
	union v90_call_phase3_configuration phase3;
	struct v90_call_analog_phase4_configuration analog_phase4;
};

union v90_call_phase3_state {
	struct v90_phase3_analog_station analog;
	struct v90_phase3_digital_station digital;
};

/* Ri starts as soon as the raw digital-character direction completes Phase 3.
 * The conventional direction remains in its continuous Phase 3 line until its
 * independent handoff boundary. */
struct v90_call_analog_mixed_state {
	struct v90_phase3_analog_station phase3;
	struct v90_phase4_ri_rx receive;
};

struct v90_call_digital_mixed_state {
	struct v90_phase3_digital_station phase3;
	struct v90_phase4_ri_tx transmit;
};

union v90_call_mixed_state {
	struct v90_call_analog_mixed_state analog;
	struct v90_call_digital_mixed_state digital;
};

union v90_call_phase4_state {
	struct v90_phase4_analog_station analog;
	struct v90_phase4_digital_station digital;
};

union v90_call_state {
	struct v90_phase2 phase2;
	union v90_call_phase3_state phase3;
	union v90_call_mixed_state mixed;
	union v90_call_phase4_state phase4;
};

/* `processed` owns the accepted network character; `next` owns the following
 * character. `phase2` is meaningful only when `processed` is PHASE2. */
struct v90_call_observation {
	enum v90_call_stage processed;
	enum v90_call_stage next;
	struct v90_phase2_event phase2;
};

/* Own one post-V.8 V.90 startup through entry into the role-specific Phase 4
 * station. Calls on one instance must be serialized; separate instances share
 * no state. */
struct v90_call {
	union v90_call_state state;
	union v90_call_local_info1 local_info1;
	union v90_call_phase3_configuration phase3_configuration;
	struct v90_call_analog_phase4_configuration
		analog_phase4_configuration;
	enum v90_modem_role local_role;
	enum g711_law law;
	enum v90_call_stage stage;
	bool failed;
};

enum v90_call_result v90_call_init(
	struct v90_call *call,
	const struct v90_call_configuration *configuration);

bool v90_call_is_valid(const struct v90_call *call);
bool v90_call_has_failed(const struct v90_call *call);
bool v90_call_phase4_is_ready(const struct v90_call *call);
enum v90_call_stage v90_call_current_stage(const struct v90_call *call);

/* Consume and produce one simultaneous 8 ksymbol/s G.711 character during
 * Phases 2 and 3. A phase change takes effect on the following call.
 * PHASE4_READY
 * consumes no character, changes no output, and leaves the initialized station
 * available to the next owner. Terminal protocol failures consume the
 * character and expose the generated output and observation. */
enum v90_call_result v90_call_octet(
	struct v90_call *call, uint8_t received_octet,
	uint8_t *transmit_octet,
	struct v90_call_observation *observation);

const char *v90_call_result_name(enum v90_call_result result);

#endif /* V90_CALL_H */
