// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_ANALOG_PATH_H
#define V90_ANALOG_PATH_H

#include "v34_data_stream.h"
#include "v90_downstream_data_rx.h"
#include "v90_phase4_analog_startup.h"
#include "v90_phase4_e_line_tx.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_analog_path_upstream_stage {
	V90_ANALOG_PATH_UPSTREAM_CP,
	V90_ANALOG_PATH_UPSTREAM_E,
	V90_ANALOG_PATH_UPSTREAM_DATA,
};

enum v90_analog_path_downstream_stage {
	V90_ANALOG_PATH_DOWNSTREAM_TRAINING,
	V90_ANALOG_PATH_DOWNSTREAM_DATA,
};

enum v90_analog_path_result {
	V90_ANALOG_PATH_OK,
	V90_ANALOG_PATH_DOWNSTREAM_FRAME,
	V90_ANALOG_PATH_SOURCE_FAILURE,
	V90_ANALOG_PATH_FAILED,
	V90_ANALOG_PATH_SEQUENCE_MISMATCH,
	V90_ANALOG_PATH_INVALID_ARGUMENT,
	V90_ANALOG_PATH_INVALID_STATE,
	V90_ANALOG_PATH_INVALID_PARAMETERS,
	V90_ANALOG_PATH_NO_COMMON_RATE,
	V90_ANALOG_PATH_OUT_OF_RANGE,
	V90_ANALOG_PATH_CLIPPING_RISK,
	V90_ANALOG_PATH_NUMERIC_FAILURE,
	V90_ANALOG_PATH_INTERNAL_ERROR,
};

union v90_analog_path_upstream {
	struct v90_phase4_cp_line_tx cp;
	struct v90_phase4_e_line_tx e;
	struct v34_data_stream data;
};

union v90_analog_path_downstream {
	struct v90_downstream_rx training;
	struct v90_downstream_data_rx data;
};

/* Own the analogue station's continuous 8 kHz path after the initial CPt/Ri
 * exchange. CP/CP-prime, E, B1, and upstream data share one QAM line; TRN2d,
 * MP/MP-prime, Ed, B1d, and downstream data share one network input clock.
 * Calls on one instance must be serialized. */
struct v90_analog_path {
	union v90_analog_path_upstream upstream;
	union v90_analog_path_downstream downstream;
	struct v90_cp_parameters data_parameters;
	struct v90_pcm_mapping data_mapping;
	struct v90_mp_record peer_mp;
	enum g711_law law;
	enum v90_analog_path_upstream_stage upstream_stage;
	enum v90_analog_path_downstream_stage downstream_stage;
	bool peer_mp_received;
	bool peer_mp_prime_received;
	bool cp_prime_sent;
	bool finishing_cp;
	bool ed_armed;
	bool receive_pending;
	bool failed;
};

enum v90_analog_path_result v90_analog_path_init(
	struct v90_analog_path *path,
	const struct v90_phase4_analog_handoff *handoff,
	const struct v90_cp_parameters *data_parameters);

bool v90_analog_path_is_valid(const struct v90_analog_path *path);
bool v90_analog_path_has_failed(const struct v90_analog_path *path);
bool v90_analog_path_upstream_is_online(
	const struct v90_analog_path *path);
bool v90_analog_path_downstream_is_online(
	const struct v90_analog_path *path);
bool v90_analog_path_is_online(const struct v90_analog_path *path);

/* Generate the analogue station's character for one clock interval. An OK
 * result commits that half of the interval and requires one receive call.
 * SOURCE_FAILURE and rejected calls change neither path nor output. */
enum v90_analog_path_result v90_analog_path_transmit(
	struct v90_analog_path *path,
	v34_mapping_frame_source source, void *source_context,
	uint8_t *transmit_octet);

/* Complete an interval started by transmit. A post-B1d data frame changes
 * `received` and returns DOWNSTREAM_FRAME. Invalid calls leave the pending
 * interval and output unchanged; terminal receive errors consume the interval
 * and latch failure. */
enum v90_analog_path_result v90_analog_path_receive(
	struct v90_analog_path *path, uint8_t received_octet,
	struct v90_pcm_stream_frame *received);

const char *v90_analog_path_result_name(
	enum v90_analog_path_result result);

#endif /* V90_ANALOG_PATH_H */
