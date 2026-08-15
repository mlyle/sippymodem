// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_CALL_H
#define PCM_CALL_H

#include "pcm_phase3_entry.h"
#include "pcm_downstream_rx.h"
#include "pcm_downstream_tx.h"
#include "v90_analog_path.h"
#include "v90_digital_path.h"
#include "pcm_phase2.h"
#include "v90_phase3_analog_station.h"
#include "v90_phase3_digital_station.h"
#include "v92_phase3_analog_station.h"
#include "v92_phase3_digital_station.h"

#include <stdbool.h>
#include <stdint.h>

enum pcm_call_stage {
	PCM_CALL_PHASE2,
	PCM_CALL_PHASE3,
	PCM_CALL_PHASE3_MIXED,
	PCM_CALL_PHASE4_ENTRY,
	PCM_CALL_PHASE4,
	PCM_CALL_DATA_PATH,
};

enum pcm_call_result {
	PCM_CALL_OK,
	PCM_CALL_PHASE4_ENTRY_READY,
	PCM_CALL_PHASE4_READY,
	PCM_CALL_SOURCE_FAILURE,
	PCM_CALL_FAILED,
	PCM_CALL_INVALID_ARGUMENT,
	PCM_CALL_INVALID_STATE,
	PCM_CALL_INVALID_SELECTION,
	PCM_CALL_UNUSABLE_SELECTION,
	PCM_CALL_MEDIA_MISMATCH,
	PCM_CALL_OUT_OF_RANGE,
	PCM_CALL_CLIPPING_RISK,
	PCM_CALL_SEQUENCE_MISMATCH,
	PCM_CALL_NUMERIC_FAILURE,
};

union pcm_call_local_info0 {
	struct v90_info0 analog;
	struct v90_info0d digital;
	struct v92_info0a v92_analog;
	struct v92_info0d v92_digital;
};

union pcm_call_local_info1 {
	struct v90_info1a analog;
	struct v34_info1c digital;
	struct v92_pcm_info1a v92_analog;
	struct v92_info1d v92_digital;
};

struct pcm_call_analog_phase3_configuration {
	int16_t nominal_peak_amplitude;
};

struct pcm_call_digital_phase3_configuration {
	struct v90_phase3_analog_rx_configuration receive;
	struct v90_jd_parameters jd;
};

struct pcm_call_v92_digital_phase3_configuration {
	struct v92_jd jd;
	struct v92_jp jp;
	double equalizer_adaptation;
	uint16_t su_minimum_magnitude;
};

union pcm_call_phase3_configuration {
	struct pcm_call_analog_phase3_configuration analog;
	struct pcm_call_digital_phase3_configuration digital;
	struct v92_phase3_analog_station_configuration v92_analog;
	struct pcm_call_v92_digital_phase3_configuration v92_digital;
};

struct pcm_call_analog_phase4_configuration {
	/* Initial CPt sent when the conventional Phase 3 line completes. */
	struct v90_cp_parameters training_parameters;
	/* First data CP sent after the initial CPt/Ri exchange. */
	struct v90_cp_parameters data_parameters;
};

struct pcm_call_digital_phase4_configuration {
	/* Initial MP sent after TRN2d; the CP acknowledgement starts clear. */
	struct v90_mp_record initial_mp;
};

struct pcm_call_configuration {
	enum pcm_protocol protocol;
	enum v90_modem_role local_role;
	enum g711_law law;
	/* Rates locally enabled for the PCM downstream. The digital station
	 * advertises this mask in Jd; the analogue station intersects it with
	 * the received Jd before sending data CP. */
	uint32_t enabled_downstream_rate_mask;
	int16_t phase2_nominal_amplitude;
	union pcm_call_local_info0 local_info0;
	union pcm_call_local_info1 local_info1;
	union pcm_call_phase3_configuration phase3;
	struct pcm_call_analog_phase4_configuration analog_phase4;
	struct pcm_call_digital_phase4_configuration digital_phase4;
};

union pcm_call_phase3_state {
	struct v90_phase3_analog_station analog;
	struct v90_phase3_digital_station digital;
	struct v92_phase3_analog_station v92_analog;
	struct v92_phase3_digital_station v92_digital;
};

/* Ri starts as soon as the raw digital-character direction completes Phase 3.
 * The conventional direction remains in its continuous Phase 3 line until its
 * independent handoff boundary. */
struct pcm_call_analog_mixed_state {
	struct v90_phase3_analog_station phase3;
	struct v90_phase4_ri_rx receive;
};

struct pcm_call_digital_mixed_state {
	struct v90_phase3_digital_station phase3;
	struct v90_phase4_ri_tx transmit;
};

/* V.92 starts the shared downstream Phase 4 epoch as soon as Ri-bar ends,
 * while the independent CPt/E1u direction finishes in its Phase 3 child. */
struct pcm_call_v92_analog_mixed_state {
	struct v92_phase3_analog_station phase3;
	struct pcm_downstream_rx receive;
};

struct pcm_call_v92_digital_mixed_state {
	struct v92_phase3_digital_station phase3;
	struct pcm_downstream_tx transmit;
};

union pcm_call_mixed_state {
	struct pcm_call_analog_mixed_state analog;
	struct pcm_call_digital_mixed_state digital;
	struct pcm_call_v92_analog_mixed_state v92_analog;
	struct pcm_call_v92_digital_mixed_state v92_digital;
};

union pcm_call_phase4_state {
	struct v90_phase4_analog_startup analog;
	struct v90_phase4_digital_startup digital;
};

union pcm_call_data_path_state {
	struct v90_analog_path analog;
	struct v90_digital_path digital;
};

union pcm_call_state {
	struct pcm_phase2 phase2;
	union pcm_call_phase3_state phase3;
	union pcm_call_mixed_state mixed;
	union pcm_call_phase4_state phase4;
	union pcm_call_data_path_state data_path;
};

/* `processed` owns the accepted network character; `next` owns the following
 * character. `phase2` is meaningful only when `processed` is PHASE2. */
struct pcm_call_observation {
	enum pcm_call_stage processed;
	enum pcm_call_stage next;
	struct pcm_phase2_event phase2;
};

struct pcm_call_data_sources {
	/* Conventional upstream frames originate at the analogue station. */
	v34_mapping_frame_source upstream;
	void *upstream_context;
	/* PCM downstream frames originate at the digital station. */
	v90_downstream_frame_source downstream;
	void *downstream_context;
};

enum pcm_call_data_event_kind {
	PCM_CALL_DATA_NONE,
	PCM_CALL_DOWNSTREAM_FRAME,
	PCM_CALL_UPSTREAM_FRAME,
};

union pcm_call_received_frame {
	struct v90_pcm_stream_frame downstream;
	struct v34_received_mapping_frame upstream;
};

struct pcm_call_data_event {
	/* DOWNSTREAM is delivered to analogue; UPSTREAM is delivered to digital. */
	union pcm_call_received_frame frame;
	enum pcm_call_data_event_kind kind;
};

/* Own one post-V.8 PCM startup. V.90 continues through its role-specific data
 * path. V.92 starts the shared downstream Phase 4 epoch during the split
 * CPt/E1u handoff, then stops transactionally when both Phase 3 directions
 * are complete. Calls on one instance must be serialized. */
struct pcm_call {
	union pcm_call_state state;
	struct pcm_phase3_entry phase3_entry;
	union pcm_call_local_info1 local_info1;
	union pcm_call_phase3_configuration phase3_configuration;
	struct pcm_call_analog_phase4_configuration
		analog_phase4_configuration;
	struct pcm_call_digital_phase4_configuration
		digital_phase4_configuration;
	uint32_t enabled_downstream_rate_mask;
	enum pcm_protocol protocol;
	enum v90_modem_role local_role;
	enum g711_law law;
	enum pcm_call_stage stage;
	bool phase3_entry_stored;
	bool failed;
};

enum pcm_call_result pcm_call_init(
	struct pcm_call *call,
	const struct pcm_call_configuration *configuration);

bool pcm_call_is_valid(const struct pcm_call *call);
bool pcm_call_has_failed(const struct pcm_call *call);
bool pcm_call_phase4_entry_is_ready(const struct pcm_call *call);
bool pcm_call_phase4_is_ready(const struct pcm_call *call);
bool pcm_call_is_online(const struct pcm_call *call);
enum pcm_call_stage pcm_call_current_stage(const struct pcm_call *call);

/* Consume and produce one simultaneous 8 ksymbol/s G.711 character. A phase
 * change takes effect on the following call. PHASE4_ENTRY_READY stops V.92
 * after its overlapping TRN2d/E1u handoff; PHASE4_READY stops V.90 after its
 * initial Phase 4 exchange. Both results consume no character, change no
 * output, and leave continuous state available to the next owner. Terminal
 * protocol failures consume the character and expose generated output and
 * observation. */
enum pcm_call_result pcm_call_octet(
	struct pcm_call *call, uint8_t received_octet,
	uint8_t *transmit_octet,
	struct pcm_call_observation *observation);

/* Transfer the exact completed Phase 4 handoff into the role-specific data
 * path without consuming a network character. Rejected calls are
 * transactional. */
enum pcm_call_result pcm_call_start_data_path(struct pcm_call *call);

/* Report role-local transmit and receive rates after both directions are
 * online. PCM rates round the exact six-character frame rate to the nearest
 * integer bit/s. Rejected calls leave both outputs unchanged. */
bool pcm_call_get_data_rates(
	const struct pcm_call *call,
	uint32_t *transmit_bits_per_second,
	uint32_t *receive_bits_per_second);

/* Consume and produce one character after start_data_path. A NULL source, or
 * a NULL sources object, transmits idle ones in that direction. SOURCE_FAILURE
 * consumes neither the received character nor the clock interval and leaves
 * both outputs unchanged. Once transmission succeeds, receive failures consume
 * the interval, expose its generated output, and latch call failure. */
enum pcm_call_result pcm_call_data_octet(
	struct pcm_call *call, uint8_t received_octet,
	const struct pcm_call_data_sources *sources,
	uint8_t *transmit_octet,
	struct pcm_call_data_event *event);

const char *pcm_call_result_name(enum pcm_call_result result);

#endif /* PCM_CALL_H */
