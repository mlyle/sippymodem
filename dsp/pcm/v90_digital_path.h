// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_DIGITAL_PATH_H
#define V90_DIGITAL_PATH_H

#include "v34_data_line_receiver.h"
#include "v90_downstream_data_stream.h"
#include "v90_phase4_digital_startup.h"
#include "v90_upstream_rx.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_digital_path_downstream_stage {
	V90_DIGITAL_PATH_DOWNSTREAM_TRAINING,
	V90_DIGITAL_PATH_DOWNSTREAM_DATA,
};

enum v90_digital_path_upstream_stage {
	V90_DIGITAL_PATH_UPSTREAM_STARTUP,
	V90_DIGITAL_PATH_UPSTREAM_DATA,
};

enum v90_digital_path_result {
	V90_DIGITAL_PATH_OK,
	V90_DIGITAL_PATH_UPSTREAM_FRAME,
	V90_DIGITAL_PATH_SOURCE_FAILURE,
	V90_DIGITAL_PATH_FAILED,
	V90_DIGITAL_PATH_SEQUENCE_MISMATCH,
	V90_DIGITAL_PATH_INVALID_ARGUMENT,
	V90_DIGITAL_PATH_INVALID_PARAMETERS,
	V90_DIGITAL_PATH_NO_COMMON_RATE,
	V90_DIGITAL_PATH_OUT_OF_RANGE,
	V90_DIGITAL_PATH_NUMERIC_FAILURE,
	V90_DIGITAL_PATH_INTERNAL_ERROR,
};

union v90_digital_path_downstream {
	struct pcm_downstream_tx training;
	struct v90_downstream_data_stream data;
};

union v90_digital_path_upstream {
	struct v90_upstream_rx startup;
	struct v34_data_line_receiver data;
};

/* Own the digital station's continuous 8 kHz path after the initial CPt/Ri
 * exchange. Downstream training and B1d/data share one output clock; upstream
 * CP/E/B1 and conventional data share one input clock. Calls on one instance
 * must be serialized, while distinct instances share no mutable state. */
struct v90_digital_path {
	union v90_digital_path_downstream downstream;
	union v90_digital_path_upstream upstream;
	struct v90_pcm_mapping data_mapping;
	uint32_t enabled_downstream_rate_mask;
	enum g711_law law;
	enum v90_digital_path_downstream_stage downstream_stage;
	enum v90_digital_path_upstream_stage upstream_stage;
	bool data_mapping_ready;
	bool failed;
};

enum v90_digital_path_result v90_digital_path_init(
	struct v90_digital_path *path,
	const struct v90_phase4_digital_handoff *handoff,
	uint32_t enabled_downstream_rate_mask);

bool v90_digital_path_is_valid(const struct v90_digital_path *path);
bool v90_digital_path_has_failed(const struct v90_digital_path *path);
bool v90_digital_path_downstream_is_online(
	const struct v90_digital_path *path);
bool v90_digital_path_upstream_is_online(
	const struct v90_digital_path *path);
bool v90_digital_path_is_online(const struct v90_digital_path *path);

/* Clock the independent downstream transmitter or upstream receiver.  These
 * directional operations let a call controller keep outgoing data continuous
 * while it qualifies an incoming full-retrain preamble. */
enum v90_digital_path_result v90_digital_path_transmit(
	struct v90_digital_path *path,
	v90_downstream_frame_source source, void *source_context,
	uint8_t *transmit_octet);
enum v90_digital_path_result v90_digital_path_receive(
	struct v90_digital_path *path, uint8_t received_octet,
	struct v34_received_mapping_frame *received);

/* Consume and produce one simultaneous G.711 network character. Downstream
 * output is generated before the received character changes control state, so
 * a CP acknowledgement affects the following output character. A successful
 * upstream mapping frame changes `received` and returns UPSTREAM_FRAME.
 *
 * A downstream source failure is retryable: it changes neither path nor
 * outputs and the same received character remains pending. Invalid calls have
 * the same transactional behavior. Terminal path errors latch failure and
 * expose the output already generated for the accepted clock character. */
enum v90_digital_path_result v90_digital_path_octet(
	struct v90_digital_path *path, uint8_t received_octet,
	v90_downstream_frame_source source, void *source_context,
	uint8_t *transmit_octet,
	struct v34_received_mapping_frame *received);

const char *v90_digital_path_result_name(
	enum v90_digital_path_result result);

#endif /* V90_DIGITAL_PATH_H */
