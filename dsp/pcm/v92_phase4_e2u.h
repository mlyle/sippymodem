// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PHASE4_E2U_H
#define V92_PHASE4_E2U_H

#include "v92_e2u_rx.h"
#include "v92_phase4_analog_station.h"
#include "v92_phase4_digital_station.h"
#include "v92_phase4_record_tx.h"
#include "v90_downstream_data_rx.h"
#include "v90_downstream_data_stream.h"

#include <stdbool.h>
#include <stdint.h>

enum v92_phase4_e2u_result {
	V92_PHASE4_E2U_OK,
	V92_PHASE4_E2U_COMPLETE,
	V92_PHASE4_E2U_FAILED,
	V92_PHASE4_E2U_INVALID_ARGUMENT,
	V92_PHASE4_E2U_INVALID_PARAMETERS,
	V92_PHASE4_E2U_INVALID_STATE,
	V92_PHASE4_E2U_INTERNAL_ERROR,
};

enum v92_phase4_e2u_downstream_stage {
	V92_PHASE4_E2U_DOWNSTREAM_TRAINING,
	V92_PHASE4_E2U_DOWNSTREAM_DATA,
};

union v92_phase4_e2u_analog_downstream {
	struct pcm_downstream_rx training;
	struct v90_downstream_data_rx data;
};

union v92_phase4_e2u_digital_downstream {
	struct pcm_downstream_tx training;
	struct v90_downstream_data_stream data;
};

struct v92_phase4_e2u_analog_state {
	struct v92_phase4_record_tx transmit;
	union v92_phase4_e2u_analog_downstream downstream;
	struct v92_cpu_parameters cpu;
	struct v92_pcm_mapping mapping;
	enum v92_phase4_e2u_downstream_stage downstream_stage;
};

struct v92_phase4_e2u_digital_state {
	struct v92_e2u_rx receive;
	struct v92_upstream_control_line_rx control;
	union v92_phase4_e2u_digital_downstream downstream;
	struct v92_cpu_parameters cpu;
	struct v92_pcm_mapping mapping;
	enum v92_phase4_e2u_downstream_stage downstream_stage;
};

union v92_phase4_e2u_state {
	struct v92_phase4_e2u_analog_state analog;
	struct v92_phase4_e2u_digital_state digital;
};

/* Own the duplex E2u/Ed crossing. E2u determines completion; the independent
 * downstream path may still be in Ed training or may already have entered
 * B1d data coding. */
struct v92_phase4_e2u {
	union v92_phase4_e2u_state state;
	enum v90_modem_role local_role;
	bool failed;
};

struct v92_phase4_e2u_analog_handoff {
	struct v92_phase4_modulator transmit;
	union v92_phase4_e2u_analog_downstream downstream;
	struct v92_cpu_parameters cpu;
	struct v92_pcm_mapping mapping;
	enum v92_phase4_e2u_downstream_stage downstream_stage;
};

struct v92_phase4_e2u_digital_handoff {
	struct v92_upstream_rx receive;
	union v92_phase4_e2u_digital_downstream downstream;
	struct v92_cpu_parameters cpu;
	struct v92_pcm_mapping mapping;
	enum v92_phase4_e2u_downstream_stage downstream_stage;
};

struct v92_phase4_e2u_handoff {
	union {
		struct v92_phase4_e2u_analog_handoff analog;
		struct v92_phase4_e2u_digital_handoff digital;
	} state;
	enum v90_modem_role local_role;
};

enum v92_phase4_e2u_result v92_phase4_e2u_init_analog(
	struct v92_phase4_e2u *owner,
	const struct v92_phase4_analog_handoff *handoff);
enum v92_phase4_e2u_result v92_phase4_e2u_init_digital(
	struct v92_phase4_e2u *owner,
	const struct v92_phase4_digital_handoff *handoff);

bool v92_phase4_e2u_is_valid(const struct v92_phase4_e2u *owner);
bool v92_phase4_e2u_has_failed(const struct v92_phase4_e2u *owner);
bool v92_phase4_e2u_is_complete(const struct v92_phase4_e2u *owner);

/* Consume and produce one simultaneous 8 kHz media interval. The authoritative
 * downstream codeword and upstream sample are supplied together; only the
 * role-owned input is used. COMPLETE consumes the final E2u interval. */
enum v92_phase4_e2u_result v92_phase4_e2u_next(
	struct v92_phase4_e2u *owner, uint8_t received_octet,
	int16_t received_sample, uint8_t *transmit_octet);

bool v92_phase4_e2u_handoff_is_valid(
	const struct v92_phase4_e2u_handoff *handoff);
enum v92_phase4_e2u_result v92_phase4_e2u_handoff(
	const struct v92_phase4_e2u *owner,
	struct v92_phase4_e2u_handoff *handoff);

/* Continue the independent downstream epoch during or after E2u. The caller
 * exclusively owns and validates the inherited union, stage and selections;
 * outputs must be distinct. A transmit SOURCE_FAILURE consumes nothing.
 * Receive BUFFERING includes accepted control records and the Ed boundary;
 * subsequent B1d/data results retain the shared downstream receiver contract. */
enum v90_downstream_data_rx_result v92_downstream_receive_owned(
	union v92_phase4_e2u_analog_downstream *downstream,
	enum v92_phase4_e2u_downstream_stage *stage,
	const struct v92_cpu_parameters *cpu, struct v92_pcm_mapping *mapping,
	uint8_t octet, struct v90_pcm_stream_frame *frame);
enum v90_downstream_data_stream_result v92_downstream_transmit_owned(
	union v92_phase4_e2u_digital_downstream *downstream,
	enum v92_phase4_e2u_downstream_stage *stage,
	const struct v92_cpu_parameters *cpu,
	v90_downstream_frame_source source, void *context, uint8_t *octet);

const char *v92_phase4_e2u_result_name(enum v92_phase4_e2u_result result);

#endif /* V92_PHASE4_E2U_H */
