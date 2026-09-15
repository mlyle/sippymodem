// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_CALL_H
#define PCM_CALL_H

#include "pcm_phase3_entry.h"
#include "pcm_downstream_rx.h"
#include "pcm_downstream_equalizer.h"
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
#include "v92_rate_pause.h"
#include "v92_upstream_data_rx.h"
#include "v92_upstream_data_tx.h"
#include "v92_ru_line_rx.h"
#include "v92_pcm_phase3_ru.h"
#include "v92_data_quality.h"
#include "v92_qc_analog.h"
#include "v92_qc_digital.h"
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
	/* Agreed QC1/QCA1 opening, before INFO0 selects full or short Phase 2. */
	PCM_CALL_QC,
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
	PCM_CALL_V8_FALLBACK_READY,
};

union pcm_call_local_info1 {
	struct v34_info1c digital;
	struct v92_info1d v92_digital;
	/* The analogue modem retains both proposals until INFO1d selects which
	 * upstream waveform the connection can use. Digital V.92 INFO1d already
	 * contains its conventional probing fields. The training Ucode is a
	 * local ceiling; the transmitted INFO1a also respects peer INFO0d power. */
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
	/* Zero selects the minimum. A host echo owner may request a longer
	 * aligned TRN1d within the protocol deadline. Retained across retrains. */
	uint32_t trn1d_symbols;
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
	/* Generate direct G.711 downstream constellations within the peer's
	 * power limit after each rate selection (including V.92 CPt/CPu).
	 * Seeds must use one shared row without a separate codec plane.
	 * False preserves caller-supplied constellations without remapping. */
	bool generate_constellations;
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
	/* The silence preference also applies to each Ru/Rd rate exchange,
	 * but is cleared for the fresh status exchange following its pause. */
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
	/* As with SUVu, retain this silence preference across rate requests. */
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

/* Explicit inputs from a prior or caller-configured short-startup profile.
 * They do not stand in for a received INFO1d or fresh probe. The upstream
 * mode and conventional carrier/rate identify the retained training policy.
 * Digital receive gain is used only for conventional upstream; PCM leaves
 * all conventional-only fields zero. The analogue role retains prior RTD,
 * while the digital role obtains a fresh value during short ranging.
 * Configured settings permit a local short INFO0 offer; both wire offers
 * must select short startup before these settings are used. The call can
 * export its trained settings but does not cache them or perform the Quick
 * Connect Phase 1 handshake. */
struct pcm_call_short_startup {
	struct pcm_phase3_short_training training;
	uint16_t round_trip_delay_samples;
	int16_t digital_receive_gain;
	enum pcm_upstream_mode upstream;
	enum v34_symbol_rate conventional_symbol_rate;
	bool conventional_high_carrier;
	bool configured;
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
	struct pcm_call_short_startup short_startup;
	struct pcm_call_analog_phase4_configuration analog_phase4;
	struct pcm_call_digital_phase4_configuration digital_phase4;
	struct pcm_call_v92_analog_phase4_configuration v92_analog_phase4;
	struct pcm_call_v92_digital_phase4_configuration v92_digital_phase4;
};

/* V.90 9.2.1.1.8 and V.92 9.3 retain the agreed PCM Phase 2 for subsequent
 * retrains even when INFO1a selects V.34 data. Keep the digital proposals
 * and rate policy, not the retired training/data graph, with that owner. */
struct pcm_fallback_restart {
	union pcm_info0_record local_info0;
	union pcm_info0_record peer_info0;
	union pcm_call_local_info1 local_info1;
	struct pcm_call_short_startup short_startup;
	struct pcm_call_digital_phase3_configuration phase3;
	struct pcm_call_v92_digital_phase3_configuration v92_phase3;
	struct pcm_call_digital_phase4_configuration phase4;
	struct pcm_call_v92_digital_phase4_configuration v92_phase4;
	uint32_t enabled_downstream_rate_mask;
	uint16_t round_trip_delay_samples;
	int16_t phase2_nominal_amplitude;
	uint8_t upstream_retry_ceiling;
	enum pcm_protocol protocol;
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
};

enum pcm_call_v92_phase4_child {
	PCM_CALL_V92_PHASE4_CONTROL,
	PCM_CALL_V92_PHASE4_E2U,
	PCM_CALL_V92_PHASE4_RU,
	PCM_CALL_V92_PHASE4_PAUSE,
};

struct pcm_call_v92_rate_request {
	struct v92_ru_line_rx receive;
	/* Rd can finish before Ru. Keep TRN2d/SUVd continuous until the
	 * completed receive side can join the ordinary control station. */
	struct pcm_downstream_tx transmit;
	struct v92_cpd_parameters cpd;
	struct v92_upstream_plan plan;
	struct v92_cpu_parameters cpu;
};

union pcm_call_v92_phase4_owner {
	struct pcm_call_v92_analog_phase4_control analog;
	struct v92_phase4_digital_station digital;
	struct v92_phase4_e2u e2u;
	struct pcm_call_v92_rate_request ru;
	struct {
		struct v92_rate_pause_analog signal;
		struct v92_pcm_mapping mapping;
	} analog_pause;
	struct {
		struct v92_rate_pause_digital signal;
		struct v92_pcm_mapping mapping;
		struct v92_cpu_parameters cpu;
		struct v92_upstream_plan plan;
	} digital_pause;
};

/* Rd transmission overlaps reception of the peer's post-Ru TRN2u. */
enum pcm_call_v92_rate_prefix_stage {
	PCM_CALL_V92_PREFIX_NONE,
	PCM_CALL_V92_PREFIX_DATA,
	PCM_CALL_V92_PREFIX_DRAIN,
	PCM_CALL_V92_PREFIX_RD,
	PCM_CALL_V92_PREFIX_RU,
};

/* Drain only the already encoded data frame before Ru. Its transmission and
 * the peer's Rd reception finish independently of each other. */
struct pcm_call_v92_analog_rate_prefix {
	struct v92_pcm_phase3_ru transmit;
	struct pcm_r_rx receive;
	int16_t queue[V92_PCM_MAPPING_FRAME_SYMBOLS];
	uint8_t queue_count;
};

struct pcm_call_v92_phase4_state {
	union pcm_call_v92_phase4_owner owner;
	union {
		struct v90_downstream_data_stream data;
		struct pcm_r_tx rd;
		struct pcm_call_v92_analog_rate_prefix analog;
	} prefix;
	struct v90_pcm_mapping training_mapping;
	/* A collided fast exchange drains only its current Rf/control frame
	 * before Rd, never resumes the already finished data source. */
	uint8_t prefix_queue[V90_CP_FRAME_INTERVALS];
	uint8_t prefix_queue_count;
	enum v92_constellation_size rate_renegotiation_constellation;
	enum pcm_call_v92_rate_prefix_stage prefix_stage;
	enum pcm_call_v92_phase4_child child;
	/* Carry the peer's request from status through E2u into the timed pause. */
	bool peer_requested_silence;
	/* Retain a local hangup through Ru/Rd, TRN2 and any committed pause.
	 * The control station takes over at a complete status-record boundary. */
	bool cleardown_requested;
};

union pcm_call_phase4_state {
	struct v90_phase4_analog_startup analog;
	struct v90_phase4_digital_startup digital;
	struct pcm_call_v92_phase4_state v92;
};

/* The analogue fast exchange keeps its existing data transmitter through RM,
 * control, E2u and FB1u. The accepted CPd only takes effect at the next B1u. */
struct pcm_call_v92_analog_fast {
	struct v92_upstream_control control;
	struct v92_pcm_mapping mapping;
	struct v92_cleardown cleardown;
	uint8_t record[V92_CPU_MAX_FRAME_BYTES];
	size_t record_bits, record_index;
	uint32_t deadline_remaining, repeat_remaining, transmitted_symbols;
	bool rm_complete, finish_started, b1_started;
	bool zero_record, zero_ack;
};

/* PCM upstream replaces only the conventional direction. The downstream
 * union crosses E2u unchanged and continues the shared Ed/B1d/data epoch. */
struct pcm_call_v92_analog_data {
	struct v92_upstream_data_tx upstream;
	struct pcm_call_v92_analog_fast fast;
	struct pcm_r_rx rd;
	struct pcm_r_rx rf;
	struct v90_pcm_mapping training_mapping;
	union v92_phase4_e2u_analog_downstream downstream;
	struct v92_cpu_parameters cpu;
	enum v92_constellation_size rate_renegotiation_constellation;
	enum v92_phase4_e2u_downstream_stage downstream_stage;
};

enum pcm_call_v92_fast_stage {
	PCM_CALL_V92_FAST_IDLE,
	PCM_CALL_V92_FAST_WAIT_PRIME,
	PCM_CALL_V92_FAST_DRAIN,
	PCM_CALL_V92_FAST_RF,
	PCM_CALL_V92_FAST_CONTROL,
	PCM_CALL_V92_FAST_FINISH,
};

/* Digital response to RM retains the live upstream decoder. The downstream
 * data owner drains before Rf, then takes back Ed/B1d at mutual acknowledgment.
 * Receive E2u/FB1u and transmit Ed have independent completion boundaries. */
struct pcm_call_v92_fast_exchange {
	struct v92_downstream_control control;
	struct v92_control_rx receive;
	struct pcm_r_tx rf;
	uint64_t b1_frame;
	uint32_t deadline_remaining;
	enum pcm_call_v92_fast_stage stage;
	bool rm_complete;
	bool b1_started;
};

struct pcm_call_v92_digital_data {
	struct v92_upstream_data_rx upstream;
	struct pcm_call_v92_fast_exchange fast;
	struct v92_ru_line_rx ru;
	struct v90_pcm_mapping training_mapping;
	union v92_phase4_e2u_digital_downstream downstream;
	struct v92_cpu_parameters cpu;
	enum v92_phase4_e2u_downstream_stage downstream_stage;
	/* Quality-score residual over post-acquisition B1u validation only,
	 * with identified post-codec host gain removed. */
	double b1u_squared_error;
	/* Freeze a quality-triggering observation while peer control qualifies.
	 * Already in quality-score units: a timeout charges the original failure
	 * without scaling it again or forgiving lost data. */
	double pending_squared_error;
	uint16_t quality_guard_samples;
	bool pending_ambiguous;
	bool pending_uncertain;
	/* A known media hole requires recovery, not a lower mapping rate. */
	bool pending_media_loss;
	/* Missing media can reject B1u before the first delayed decision. */
	bool b1u_decode_failed;
};

union pcm_call_data_path_state {
	struct v90_analog_path analog;
	struct v90_digital_path digital;
	struct pcm_call_v92_analog_data v92_analog;
	struct pcm_call_v92_digital_data v92_digital;
};

union pcm_call_state {
	union {
		struct v92_qc_analog analog;
		struct v92_qc_digital digital;
	} qc;
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
	struct pcm_call_short_startup short_startup;
	struct pcm_call_analog_phase4_configuration
		analog_phase4_configuration;
	struct pcm_call_digital_phase4_configuration
		digital_phase4_configuration;
	struct pcm_call_v92_analog_phase4_configuration
		v92_analog_phase4_configuration;
	struct pcm_call_v92_digital_phase4_configuration
		v92_digital_phase4_configuration;
	struct v34_full_retrain retrain;
	/* V.92 PCM channel memory, independent of control/data coding epochs.
	 * A full retrain clears it; Ri learns the next channel. */
	struct pcm_downstream_equalizer downstream_equalizer;
	struct v92_data_quality pcm_upstream_quality;
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
	/* Zero until B1u or online quality lowers the direct-selection ceiling.
	 * Retained through retrains; never changes the configured maximum. */
	uint8_t v92_upstream_retry_ceiling;
	/* Failed B1d lowers only generated downstream selections. Zero means
	 * no restriction beyond the configured mask and the current peer Jd. */
	uint8_t downstream_retry_ceiling;
	/* Carry QTS's six-symbol phase through Phase 2 and the wait for Ja.
	 * The continuous downstream transmitter takes ownership at the first Sd. */
	uint8_t qc_frame_interval;
	bool qc_frame_pending;
	bool phase3_entry_stored;
	bool peer_info0_stored;
	bool v92_peer_jd_stored;
	bool round_trip_delay_stored;
	bool b1_deadline_armed;
	bool rate_renegotiation_deadline_armed;
	bool v92_rate_renegotiation_active;
	bool retrain_active;
	bool failed;
};

enum pcm_call_result pcm_call_init(
	struct pcm_call *call,
	const struct pcm_call_configuration *configuration);

/* Begin immediately after the final QC1/QCA1 sample. The role-matched records
 * must contain exactly one ACK. This owns QTS/ANSpcm/TONEq and shares the final
 * 75 ms silence with Phase 2's receiver, without adding another quiet interval.
 * Digital frame alignment is preserved from QTS through the first Sd.
 * QC opening alone never selects short Phase 2: that requires retained training
 * and agreement in INFO0. The host owns admission and the overall call timeout.
 * V8_FALLBACK_READY asks the host to restart normal V.8; repeated octet() calls
 * then leave all state and outputs unchanged. Invalid init changes no state. */
enum pcm_call_result pcm_call_init_qc(
	struct pcm_call *call, const struct pcm_call_configuration *configuration,
	const struct v92_qc1_message *local, const struct v92_qc1_message *peer);

/* Start a full retrain while retaining the negotiated PCM protocol and INFO0
 * records. The following line character begins the 70 ms silence interval. */
enum pcm_call_result pcm_call_start_retrain(struct pcm_call *call);

/* Initiate a PCM-upstream rate exchange from either online endpoint.
 * Drain the accepted data frame before Ru/Rd; subsequent media uses octet()
 * through Phase 4 and the ordinary start_data_path() handoff. No new host
 * frame is consumed until data resumes after B1. Full retrain remains separate.
 * Unsupported protocols and non-online calls are rejected unchanged. */
enum pcm_call_result pcm_call_start_rate_renegotiation(struct pcm_call *call);

/* V.92 PCM-upstream fast exchange, from either online role. Keep the current
 * data-frame alignment and trained line, drain accepted data before RM/Rf,
 * and exchange parameters in the old mapping. Continue with data_octet();
 * new B1, not control acknowledgment alone, makes the call online again. */
enum pcm_call_result pcm_call_start_fast_exchange(struct pcm_call *call);

/* Begin V.90 zero-rate cleardown on the current continuous data or CP/MP
 * line. A rate exchange can be cancelled before CONNECT or during retrain;
 * pre-control startup and an already committed E/Ed are not interrupted.
 * Subsequent data_octet calls ignore host sources and return
 * CLEARDOWN_COMPLETE only after the final control/filter output drains.
 * V.92 PCM-upstream Phase 4 control can also be cancelled with zero-rate CP.
 * Online PCM upstream first enters a nonsilent Ru/Rd rate exchange. The
 * accepted frame drains before the request; Phase 4 cancellation continues
 * through pcm_call_octet(). An unanswered or broken entry closes rather than
 * starting another retrain. Pre-control startup is still rejected.
 * A completed local or peer cleardown is idempotent and cannot retrain. */
enum pcm_call_result pcm_call_start_cleardown(struct pcm_call *call);

bool pcm_call_is_valid(const struct pcm_call *call);
bool pcm_call_has_failed(const struct pcm_call *call);
bool pcm_call_phase4_is_ready(const struct pcm_call *call);
bool pcm_call_is_online(const struct pcm_call *call);
enum pcm_call_stage pcm_call_current_stage(const struct pcm_call *call);

/* Export the actual V.92 training selection and live receive gain only from
 * stable online data, not retrain, rate exchange, or cleardown. No call state
 * is changed; rejection leaves the non-overlapping output unchanged. The host
 * owns admission after useful data, peer/role/law identity, and expiry. These
 * inputs alone do not establish agreement to use Quick Connect next time. */
bool pcm_call_get_short_startup(
	const struct pcm_call *call, struct pcm_call_short_startup *profile);

/* Inspect only the active owner's shutdown state, not the child DSP graph.
 * The serialized host uses this between media-block validations to let an
 * accepted protocol cleardown finish independently of the CONNECT timeout.
 * Includes peer-initiated cleardown and the completed CLOSED state. */
bool pcm_call_is_disconnecting(const struct pcm_call *call);

/* A PCM digital endpoint can receive a conventional V.34 INFO1a after its
 * INFO1d offer. Copy the completed Phase 2 records and receive state without
 * consuming a line character. Rejected calls leave the handoff unchanged. */
enum pcm_call_result pcm_call_v34_fallback_handoff(
	const struct pcm_call *call,
	struct v34_phase2_handoff *handoff,
	struct pcm_fallback_restart *restart);

/* Resume the original PCM digital agreement after V.34 data. The shared
 * retrain detector/response is transferred at a media-block boundary, so
 * neither its remaining silence nor an accepted peer tone is restarted.
 * Rejected calls leave the destination unchanged. */
enum pcm_call_result pcm_call_init_from_v34(
	struct pcm_call *call, const struct pcm_fallback_restart *restart,
	const struct v34_full_retrain *response);

/* Consume and produce one simultaneous 8 ksymbol/s G.711 character.
 * `received_octet` retains exact PCM codeword semantics while
 * `received_sample` is the conventional physical line sample. V.90 digital
 * Phase 2, conventional upstream Phase 3, its final return drain, digital
 * Phase 4, and retrain observation consume that sample without G.711
 * requantization. Phase 2 gain measurements thus describe the same physical
 * input inherited by Phase 3. Analogue PCM downstream and V.92 PCM upstream,
 * including their Phase 2 frontends, retain exact octets. With PCM upstream
 * selected, qualified Ri trains the analogue downstream sample equalizer;
 * the physical sample remains unchanged for retrain observation.
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
 * upstream receiver and the online retrain detector.
 * `received_available` is false only for known missing media, never from
 * its sample value. The V.92 PCM-upstream receiver rejects affected B1u
 * and clamps online payload pending bounded recovery; other paths keep
 * their existing signal-based behavior. A NULL source, or
 * a NULL sources object, transmits idle ones in that direction. SOURCE_FAILURE
 * consumes neither the received character nor the clock interval and leaves
 * both outputs unchanged. Once transmission succeeds, receive failures consume
 * the interval, expose its generated output, and latch call failure.
 * CLEARDOWN_COMPLETE is orderly termination, not failure. The call enters
 * CLOSED; subsequent character calls return silence and no host event without
 * consuming a source or advancing the retained diagnostic state. */
enum pcm_call_result pcm_call_data_octet(
	struct pcm_call *call, uint8_t received_octet, int16_t received_sample,
	bool received_available,
	const struct pcm_call_data_sources *sources,
	uint8_t *transmit_octet,
	struct pcm_call_data_event *event);

/* Consume a data character while the caller exclusively owns a call already
 * validated for the current media block. Outputs must be distinct and outside
 * the call. Validate the call again before releasing the block. host_sample
 * preserves the gain-only signed sample before independent echo subtraction;
 * received_sample is the cleaned line sample. Pass equal samples when no
 * echo conditioning was applied. PCM upstream alone uses this metadata. */
enum pcm_call_result pcm_call_data_octet_owned(
	struct pcm_call *call, uint8_t received_octet, int16_t received_sample,
	int16_t host_sample, bool received_available,
	const struct pcm_call_data_sources *sources,
	uint8_t *transmit_octet,
	struct pcm_call_data_event *event);

const char *pcm_call_result_name(enum pcm_call_result result);

#endif /* PCM_CALL_H */
