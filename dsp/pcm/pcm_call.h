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
#include "v92_phase4_analog_station.h"
#include "v92_phase4_digital_station.h"
#include "v92_phase4_e2u.h"
#include "v34_phase2_tone.h"
#include "v34_phase2_selection.h"

#include <stdbool.h>
#include <stdint.h>

enum pcm_call_stage {
	PCM_CALL_PHASE2,
	PCM_CALL_PHASE3,
	PCM_CALL_PHASE3_MIXED,
	PCM_CALL_PHASE4,
	PCM_CALL_DATA_PATH,
	PCM_CALL_RETRAIN_SILENCE,
};

enum {
	PCM_CALL_RETRAIN_SILENCE_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 70 / 1000,
};

enum pcm_call_result {
	PCM_CALL_OK,
	PCM_CALL_PHASE4_READY,
	PCM_CALL_V34_FALLBACK_READY,
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

union pcm_call_local_info1 {
	struct v90_info1a analog;
	struct v34_info1c digital;
	struct v92_pcm_info1a v92_analog;
	struct v92_info1d v92_digital;
};

struct pcm_call_analog_phase3_configuration {
	int16_t nominal_peak_amplitude;
	enum v90_dil_profile dil_profile;
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

struct pcm_call_v92_analog_phase4_configuration {
	struct v92_suvu_parameters suvu;
	struct v92_cpu_parameters cpu;
};

struct pcm_call_v92_digital_phase4_configuration {
	struct v92_suvd_parameters suvd;
	struct v92_cpd_base_parameters cpd;
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
	union pcm_info0_record local_info0;
	union pcm_call_local_info1 local_info1;
	union pcm_call_phase3_configuration phase3;
	struct pcm_call_analog_phase4_configuration analog_phase4;
	struct pcm_call_digital_phase4_configuration digital_phase4;
	struct pcm_call_v92_analog_phase4_configuration v92_analog_phase4;
	struct pcm_call_v92_digital_phase4_configuration v92_digital_phase4;
};

union pcm_call_phase3_state {
	struct v90_phase3_analog_station analog;
	struct v90_phase3_digital_station digital;
	struct v92_phase3_analog_station v92_analog;
	struct v92_phase3_digital_station v92_digital;
};

/* The conventional direction remains in its continuous Phase 3 line after
 * the raw digital-character direction completes. Ri cannot causally respond
 * until CPt begins, so Phase 4 owns a fresh, RTD-guarded Ri search. */
struct pcm_call_analog_mixed_state {
	struct v90_phase3_analog_station phase3;
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
	struct v92_suvd_parameters early_suvd;
	bool early_suvd_received;
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

struct pcm_call_v92_analog_phase4_control {
	struct v92_phase4_analog_station station;
	uint64_t observed_cpu_records;
	uint32_t cpu_repeat_interval_samples;
	uint32_t cpu_repeat_remaining_samples;
	bool awaiting_cpu_completion;
};

enum pcm_call_v92_phase4_child {
	PCM_CALL_V92_PHASE4_CONTROL,
	PCM_CALL_V92_PHASE4_E2U,
};

union pcm_call_v92_phase4_owner {
	struct pcm_call_v92_analog_phase4_control analog;
	struct v92_phase4_digital_station digital;
	struct v92_phase4_e2u e2u;
};

struct pcm_call_v92_phase4_state {
	union pcm_call_v92_phase4_owner owner;
	enum pcm_call_v92_phase4_child child;
};

union pcm_call_phase4_state {
	struct v90_phase4_analog_startup analog;
	struct v90_phase4_digital_startup digital;
	struct pcm_call_v92_phase4_state v92;
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
	struct {
		uint16_t samples_remaining;
	} retrain_silence;
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
 * CPt/E1u handoff, continues through the CP/SUV control exchange, and owns the
 * post-control E2u/Ed crossing. Calls on one instance must be serialized. */
struct pcm_call {
	union pcm_call_state state;
	struct pcm_phase3_entry phase3_entry;
	union pcm_info0_record local_info0;
	union pcm_info0_record peer_info0;
	union pcm_call_local_info1 local_info1;
	union pcm_call_phase3_configuration phase3_configuration;
	struct pcm_call_analog_phase4_configuration
		analog_phase4_configuration;
	struct pcm_call_digital_phase4_configuration
		digital_phase4_configuration;
	struct pcm_call_v92_analog_phase4_configuration
		v92_analog_phase4_configuration;
	struct pcm_call_v92_digital_phase4_configuration
		v92_digital_phase4_configuration;
	struct v34_retrain_detector retrain_detector;
	uint32_t enabled_downstream_rate_mask;
	uint32_t v92_peer_downstream_rate_mask;
	uint32_t b1_deadline_remaining_samples;
	uint16_t round_trip_delay_samples;
	int16_t phase2_nominal_amplitude;
	enum pcm_protocol protocol;
	enum v90_modem_role local_role;
	enum g711_law law;
	enum pcm_call_stage stage;
	bool phase3_entry_stored;
	bool peer_info0_stored;
	bool v92_peer_downstream_rate_stored;
	bool round_trip_delay_stored;
	bool b1_deadline_armed;
	bool retrain_active;
	bool retrain_peer_tone_retained;
	bool failed;
};

enum pcm_call_result pcm_call_init(
	struct pcm_call *call,
	const struct pcm_call_configuration *configuration);

/* Start a full retrain while retaining the negotiated PCM protocol and INFO0
 * records. The following line character begins the 70 ms silence interval. */
enum pcm_call_result pcm_call_start_retrain(struct pcm_call *call);

bool pcm_call_is_valid(const struct pcm_call *call);
bool pcm_call_has_failed(const struct pcm_call *call);
bool pcm_call_phase4_is_ready(const struct pcm_call *call);
bool pcm_call_is_online(const struct pcm_call *call);
enum pcm_call_stage pcm_call_current_stage(const struct pcm_call *call);

/* A V.90 digital endpoint can receive a conventional V.34 INFO1a after its
 * INFO1d offer. Copy the completed Phase 2 records and receive state without
 * consuming a line character. Rejected calls leave the handoff unchanged. */
enum pcm_call_result pcm_call_v34_fallback_handoff(
	const struct pcm_call *call,
	struct v34_phase2_handoff *handoff);

/* Consume and produce one simultaneous 8 ksymbol/s G.711 character.
 * `received_octet` retains exact PCM codeword semantics while
 * `received_sample` is the decoded physical line sample. Phase 2 and Phase 3
 * retain their existing normalized-codeword frontends; digital Phase 4 and
 * retrain observation consume the physical sample. A phase change takes effect
 * on the following call. PHASE4_READY stops V.90 after its
 * initial Phase 4 exchange and V.92 after its role-local E2u interval. The
 * peer may still need continuous media before reaching its own handoff. This
 * result consumes no character, changes no output, and leaves that continuous
 * state available to the next owner. V34_FALLBACK_READY similarly retains the
 * completed Phase 2 state for a conventional V.34 owner. Terminal protocol
 * failures consume the character and expose generated output and observation. */
enum pcm_call_result pcm_call_octet(
	struct pcm_call *call, uint8_t received_octet, int16_t received_sample,
	uint8_t *transmit_octet,
	struct pcm_call_observation *observation);

/* Transfer an exact completed V.90 Phase 4 handoff into its role-specific data
 * path without consuming a network character. V.92 B1u/data continuation is a
 * later owner and is rejected transactionally here. */
enum pcm_call_result pcm_call_start_data_path(struct pcm_call *call);

/* Report role-local transmit and receive rates after both directions are
 * online. PCM rates round the exact six-character frame rate to the nearest
 * integer bit/s. Rejected calls leave both outputs unchanged. */
bool pcm_call_get_data_rates(
	const struct pcm_call *call,
	uint32_t *transmit_bits_per_second,
	uint32_t *receive_bits_per_second);

/* Consume and produce one character after start_data_path. `received_octet`
 * preserves exact PCM codeword semantics for the analogue downstream receiver;
 * `received_sample` is the decoded physical line sample owned by the digital
 * upstream receiver and the online retrain detector. A NULL source, or
 * a NULL sources object, transmits idle ones in that direction. SOURCE_FAILURE
 * consumes neither the received character nor the clock interval and leaves
 * both outputs unchanged. Once transmission succeeds, receive failures consume
 * the interval, expose its generated output, and latch call failure. */
enum pcm_call_result pcm_call_data_octet(
	struct pcm_call *call, uint8_t received_octet, int16_t received_sample,
	const struct pcm_call_data_sources *sources,
	uint8_t *transmit_octet,
	struct pcm_call_data_event *event);

/* Consume a data character while the caller exclusively owns a call already
 * validated for the current media block. Outputs must be distinct and outside
 * the call. Validate the call again before releasing the block. */
enum pcm_call_result pcm_call_data_octet_owned(
	struct pcm_call *call, uint8_t received_octet, int16_t received_sample,
	const struct pcm_call_data_sources *sources,
	uint8_t *transmit_octet,
	struct pcm_call_data_event *event);

const char *pcm_call_result_name(enum pcm_call_result result);

#endif /* PCM_CALL_H */
