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
#include "v92_upstream_data_rx.h"
#include "v92_upstream_data_tx.h"
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
	/* Local output has drained or the peer sent a zero-rate CP/MP. */
	PCM_CALL_CLOSED,
};

enum pcm_call_result {
	PCM_CALL_OK,
	PCM_CALL_PHASE4_READY,
	PCM_CALL_V34_FALLBACK_READY,
	PCM_CALL_CLEARDOWN_COMPLETE,
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
	struct v34_info1c digital;
	struct v92_info1d v92_digital;
	/* The analogue modem retains both proposals until INFO1d selects which
	 * upstream waveform the connection can use. Digital V.92 INFO1d already
	 * contains its conventional probing fields. */
	struct {
		struct v90_info1a analog;
		struct v92_pcm_info1a v92_analog;
		/* Local policy; the analogue station chooses its INFO1a response. */
		bool pcm_upstream_enabled;
	};
};

struct pcm_call_analog_phase3_configuration {
	int16_t nominal_peak_amplitude;
	enum v90_dil_profile dil_profile;
};

struct pcm_call_digital_phase3_configuration {
	struct v90_jd_parameters jd;
};

struct pcm_call_v92_digital_phase3_configuration {
	struct v92_jd jd;
	struct v92_jp jp;
	double equalizer_adaptation;
	uint16_t su_minimum_magnitude;
};

union pcm_call_phase3_configuration {
	/* Roles are exclusive; upstream capabilities are not. INFO1 may select
	 * conventional upstream even after both modems advertise V.92. */
	struct {
		struct pcm_call_analog_phase3_configuration analog;
		struct v92_phase3_analog_station_configuration v92_analog;
	};
	struct {
		struct pcm_call_digital_phase3_configuration digital;
		struct pcm_call_v92_digital_phase3_configuration v92_digital;
	};
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
	/* Retain configured preferences across retrains. Rate and spectral
	 * lookahead are ceilings, constrained by the peer's current Jd. */
	struct v92_cpu_parameters cpu;
};

enum pcm_call_v92_upstream_selection {
	PCM_CALL_V92_UPSTREAM_CONFIGURED,
	PCM_CALL_V92_UPSTREAM_DIRECT,
};

struct pcm_call_v92_digital_phase4_configuration {
	struct v92_suvd_parameters suvd;
	enum pcm_call_v92_upstream_selection upstream_selection;
	union {
		/* A caller-owned mapping is fixed: do not replace its constellation
		 * or filters when the peer cannot transmit its selected rate. */
		struct v92_cpd_parameters cpd;
		/* Construct the complete direct mapping after Ja, selecting the
		 * highest usable peer-enabled rate at or below this local maximum. Retain
		 * this policy across retrains rather than pinning the first rate.
		 * After TRN2u, compensate the measured channel unless all retained
		 * training samples remain in their expected G.711 cells. */
		uint8_t direct_maximum_data_rate_number;
	};
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

/* V.90 9.2.1.1.8 retains V.90 Phase 2 for subsequent retrains even when
 * INFO1a selects V.34 data. Keep only the digital call agreement, not the
 * retired PCM training/data graph, with the conventional owner. */
struct v90_fallback_restart {
	struct v90_info0d local_info0;
	struct v90_info0 peer_info0;
	struct v34_info1c local_info1;
	struct pcm_call_digital_phase3_configuration phase3;
	struct pcm_call_digital_phase4_configuration phase4;
	uint16_t round_trip_delay_samples;
	int16_t phase2_nominal_amplitude;
	enum g711_law law;
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

/* PCM upstream replaces only the conventional direction. The downstream
 * union crosses E2u unchanged and continues the shared Ed/B1d/data epoch. */
struct pcm_call_v92_analog_data {
	struct v92_upstream_data_tx upstream;
	union v92_phase4_e2u_analog_downstream downstream;
	struct v92_cpu_parameters cpu;
	enum v92_phase4_e2u_downstream_stage downstream_stage;
};

struct pcm_call_v92_digital_data {
	struct v92_upstream_data_rx upstream;
	union v92_phase4_e2u_digital_downstream downstream;
	struct v92_cpu_parameters cpu;
	enum v92_phase4_e2u_downstream_stage downstream_stage;
	double b1u_squared_error;
};

union pcm_call_data_path_state {
	struct v90_analog_path analog;
	struct v90_digital_path digital;
	struct pcm_call_v92_analog_data v92_analog;
	struct pcm_call_v92_digital_data v92_digital;
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
	/* PCM upstream has no conventional mapping request or auxiliary bit. */
	v92_upstream_frame_source pcm_upstream;
	void *pcm_upstream_context;
};

enum pcm_call_data_event_kind {
	PCM_CALL_DATA_NONE,
	PCM_CALL_DOWNSTREAM_FRAME,
	PCM_CALL_UPSTREAM_FRAME,
	PCM_CALL_PCM_UPSTREAM_FRAME,
};

union pcm_call_received_frame {
	struct v90_pcm_stream_frame downstream;
	struct v34_received_mapping_frame upstream;
	struct v92_upstream_data_frame pcm_upstream;
};

struct pcm_call_data_event {
	/* DOWNSTREAM is delivered to analogue; UPSTREAM is delivered to digital. */
	union pcm_call_received_frame frame;
	enum pcm_call_data_event_kind kind;
};

/* Own one post-V.8 PCM startup. V.90 and V.92 share the conventional-upstream
 * path. With PCM upstream selected, V.92 starts downstream Phase 4 during
 * the split CPt/E1u handoff, continues through CP/SUV control, and owns the
 * post-control E2u/Ed crossing into continuous PCM upstream and downstream
 * data. Calls on one instance must be serialized. */
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
	struct v34_full_retrain retrain;
	struct v92_jd v92_peer_jd;
	uint32_t enabled_downstream_rate_mask;
	uint32_t b1_deadline_remaining_samples;
	uint32_t rate_renegotiation_deadline_remaining_samples;
	uint16_t round_trip_delay_samples;
	int16_t phase2_nominal_amplitude;
	enum pcm_protocol protocol;
	enum v90_modem_role local_role;
	enum g711_law law;
	enum pcm_call_stage stage;
	bool phase3_entry_stored;
	bool peer_info0_stored;
	bool v92_peer_jd_stored;
	bool round_trip_delay_stored;
	bool b1_deadline_armed;
	bool rate_renegotiation_deadline_armed;
	bool retrain_active;
	bool failed;
};

enum pcm_call_result pcm_call_init(
	struct pcm_call *call,
	const struct pcm_call_configuration *configuration);

/* Start a full retrain while retaining the negotiated PCM protocol and INFO0
 * records. The following line character begins the 70 ms silence interval. */
enum pcm_call_result pcm_call_start_retrain(struct pcm_call *call);

/* Begin V.90 zero-rate cleardown on the current continuous data or CP/MP
 * line. A rate exchange can be cancelled before CONNECT or during retrain;
 * pre-control startup and an already committed E/Ed are not interrupted.
 * Subsequent data_octet calls ignore host sources and return
 * CLEARDOWN_COMPLETE only after the final control/filter output drains.
 * PCM-upstream zero-rate cleardown is not yet implemented and is rejected here.
 * A completed local or peer cleardown is idempotent and cannot retrain. */
enum pcm_call_result pcm_call_start_cleardown(struct pcm_call *call);

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
	struct v34_phase2_handoff *handoff,
	struct v90_fallback_restart *restart);

/* Resume the original V.90 digital agreement after V.34 data. The shared
 * retrain detector/response is transferred at a media-block boundary, so
 * neither its remaining silence nor an accepted peer tone is restarted.
 * Rejected calls leave the destination unchanged. */
enum pcm_call_result pcm_call_init_from_v34(
	struct pcm_call *call, const struct v90_fallback_restart *restart,
	const struct v34_full_retrain *response);

/* Consume and produce one simultaneous 8 ksymbol/s G.711 character.
 * `received_octet` retains exact PCM codeword semantics while
 * `received_sample` is the conventional physical line sample. V.90 digital
 * Phase 2, conventional upstream Phase 3, its final return drain, digital
 * Phase 4, and retrain observation consume that sample without G.711
 * requantization. Phase 2 gain measurements thus describe the same physical
 * input inherited by Phase 3. Analogue PCM downstream and V.92 PCM upstream,
 * including their Phase 2 frontends, retain exact octets.
 * A phase change takes effect
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

/* Transfer the exact role-local Phase 4 handoff without consuming a network
 * character. V.92 resets upstream coding once at B1u while preserving the
 * trained line and the independent downstream training/data epoch. */
enum pcm_call_result pcm_call_start_data_path(struct pcm_call *call);

/* Inspect only the retained INFO1 selection, not the child DSP graph. False
 * before selection and during a new Phase 2/retrain epoch. The session can
 * remain V.92 while the selected upstream uses conventional V.34 modulation. */
bool pcm_call_uses_conventional_upstream(const struct pcm_call *call);

/* Report role-local transmit and receive rates after both directions are
 * online. PCM rates round the exact mapping-frame rate to the nearest
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
 * the interval, expose its generated output, and latch call failure.
 * CLEARDOWN_COMPLETE is orderly termination, not failure. The call enters
 * CLOSED; subsequent character calls return silence and no host event without
 * consuming a source or advancing the retained diagnostic state. */
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
