// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_CALL_H
#define V90_CALL_H

#include "v90_analog_path.h"
#include "v90_digital_path.h"
#include "pcm_phase2.h"
#include "v90_phase3_analog_station.h"
#include "v90_phase3_digital_station.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_call_stage {
	V90_CALL_PHASE2,
	V90_CALL_PHASE3,
	V90_CALL_PHASE3_MIXED,
	V90_CALL_PHASE4,
	V90_CALL_DATA_PATH,
};

enum v90_call_result {
	V90_CALL_OK,
	V90_CALL_PHASE4_READY,
	V90_CALL_SOURCE_FAILURE,
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
	/* First data CP sent after the initial CPt/Ri exchange. */
	struct v90_cp_parameters data_parameters;
};

struct v90_call_digital_phase4_configuration {
	/* Initial MP sent after TRN2d; the CP acknowledgement starts clear. */
	struct v90_mp_record initial_mp;
};

struct v90_call_configuration {
	enum v90_modem_role local_role;
	enum g711_law law;
	/* Rates locally enabled for the PCM downstream. The digital station
	 * advertises this mask in Jd; the analogue station intersects it with
	 * the received Jd before sending data CP. */
	uint32_t enabled_downstream_rate_mask;
	int16_t phase2_nominal_amplitude;
	union v90_call_local_info0 local_info0;
	union v90_call_local_info1 local_info1;
	union v90_call_phase3_configuration phase3;
	struct v90_call_analog_phase4_configuration analog_phase4;
	struct v90_call_digital_phase4_configuration digital_phase4;
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
	struct v90_phase4_analog_startup analog;
	struct v90_phase4_digital_startup digital;
};

union v90_call_data_path_state {
	struct v90_analog_path analog;
	struct v90_digital_path digital;
};

union v90_call_state {
	struct pcm_phase2 phase2;
	union v90_call_phase3_state phase3;
	union v90_call_mixed_state mixed;
	union v90_call_phase4_state phase4;
	union v90_call_data_path_state data_path;
};

/* `processed` owns the accepted network character; `next` owns the following
 * character. `phase2` is meaningful only when `processed` is PHASE2. */
struct v90_call_observation {
	enum v90_call_stage processed;
	enum v90_call_stage next;
	struct pcm_phase2_event phase2;
};

struct v90_call_data_sources {
	/* Conventional upstream frames originate at the analogue station. */
	v34_mapping_frame_source upstream;
	void *upstream_context;
	/* PCM downstream frames originate at the digital station. */
	v90_downstream_frame_source downstream;
	void *downstream_context;
};

enum v90_call_data_event_kind {
	V90_CALL_DATA_NONE,
	V90_CALL_DOWNSTREAM_FRAME,
	V90_CALL_UPSTREAM_FRAME,
};

union v90_call_received_frame {
	struct v90_pcm_stream_frame downstream;
	struct v34_received_mapping_frame upstream;
};

struct v90_call_data_event {
	/* DOWNSTREAM is delivered to analogue; UPSTREAM is delivered to digital. */
	union v90_call_received_frame frame;
	enum v90_call_data_event_kind kind;
};

/* Own one post-V.8 V.90 startup and its continuous role-specific data path.
 * Calls on one instance must be serialized; separate instances share no
 * state. */
struct v90_call {
	union v90_call_state state;
	union v90_call_local_info1 local_info1;
	union v90_call_phase3_configuration phase3_configuration;
	struct v90_call_analog_phase4_configuration
		analog_phase4_configuration;
	struct v90_call_digital_phase4_configuration
		digital_phase4_configuration;
	uint32_t enabled_downstream_rate_mask;
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
bool v90_call_is_online(const struct v90_call *call);
enum v90_call_stage v90_call_current_stage(const struct v90_call *call);

/* Consume and produce one simultaneous 8 ksymbol/s G.711 character through
 * the initial Phase 4 CPt/Ri exchange. A phase change takes effect on the
 * following call. PHASE4_READY consumes no character, changes no output, and
 * leaves both continuous directions available to the next owner. Terminal
 * protocol failures consume the character and expose the generated output and
 * observation. */
enum v90_call_result v90_call_octet(
	struct v90_call *call, uint8_t received_octet,
	uint8_t *transmit_octet,
	struct v90_call_observation *observation);

/* Transfer the exact completed Phase 4 handoff into the role-specific data
 * path without consuming a network character. Rejected calls are
 * transactional. */
enum v90_call_result v90_call_start_data_path(struct v90_call *call);

/* Report role-local transmit and receive rates after both directions are
 * online. PCM rates round the exact six-character frame rate to the nearest
 * integer bit/s. Rejected calls leave both outputs unchanged. */
bool v90_call_get_data_rates(
	const struct v90_call *call,
	uint32_t *transmit_bits_per_second,
	uint32_t *receive_bits_per_second);

/* Consume and produce one character after start_data_path. A NULL source, or
 * a NULL sources object, transmits idle ones in that direction. SOURCE_FAILURE
 * consumes neither the received character nor the clock interval and leaves
 * both outputs unchanged. Once transmission succeeds, receive failures consume
 * the interval, expose its generated output, and latch call failure. */
enum v90_call_result v90_call_data_octet(
	struct v90_call *call, uint8_t received_octet,
	const struct v90_call_data_sources *sources,
	uint8_t *transmit_octet,
	struct v90_call_data_event *event);

const char *v90_call_result_name(enum v90_call_result result);

#endif /* V90_CALL_H */
