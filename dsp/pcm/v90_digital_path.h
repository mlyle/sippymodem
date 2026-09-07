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
	V90_DIGITAL_PATH_DOWNSTREAM_DATA_DRAIN,
	V90_DIGITAL_PATH_DOWNSTREAM_RD,
	V90_DIGITAL_PATH_DOWNSTREAM_TRAINING_SILENCE,
	V90_DIGITAL_PATH_DOWNSTREAM_SILENCE,
	V90_DIGITAL_PATH_DOWNSTREAM_RT,
};

enum v90_digital_path_upstream_stage {
	V90_DIGITAL_PATH_UPSTREAM_STARTUP,
	V90_DIGITAL_PATH_UPSTREAM_DATA,
};

enum v90_digital_path_result {
	V90_DIGITAL_PATH_OK,
	V90_DIGITAL_PATH_UPSTREAM_FRAME,
	V90_DIGITAL_PATH_RATE_RENEGOTIATION,
	V90_DIGITAL_PATH_CLEARDOWN_COMPLETE,
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

enum {
	V90_DIGITAL_PATH_MAPPING_FRAME_SYMBOLS =
		V34_4D_INTERVALS_PER_MAPPING_FRAME *
		V34_2D_SYMBOLS_PER_4D_INTERVAL,
	/* The early gate fits inside survivor traceback, so only frames produced
	 * while a candidate is active need buffering. */
	V90_DIGITAL_PATH_CANDIDATE_FRAME_CAPACITY =
		(1u + V34_PHASE3_S_HUNT_CANDIDATE_SYMBOLS +
		 V90_DIGITAL_PATH_MAPPING_FRAME_SYMBOLS - 1u) /
			V90_DIGITAL_PATH_MAPPING_FRAME_SYMBOLS,
	/* Once the peer finishes upstream B1, our downstream control can have at
	 * most Ed plus B1d left. At the maximum 3429 symbol/s, this bounds the
	 * complete post-B1 mapping frames that may need holding until both halves
	 * are online. */
	V90_DIGITAL_PATH_RRN_FRAME_CAPACITY =
		((V90_PHASE4_ED_FRAMES + V90_PHASE4_B1D_FRAMES) *
		 V90_CP_FRAME_INTERVALS * 3429u +
		 8000u * V90_DIGITAL_PATH_MAPPING_FRAME_SYMBOLS - 1u) /
			(8000u * V90_DIGITAL_PATH_MAPPING_FRAME_SYMBOLS),
	V90_DIGITAL_PATH_PENDING_FRAME_CAPACITY =
		V90_DIGITAL_PATH_CANDIDATE_FRAME_CAPACITY >
				V90_DIGITAL_PATH_RRN_FRAME_CAPACITY ?
			V90_DIGITAL_PATH_CANDIDATE_FRAME_CAPACITY :
			V90_DIGITAL_PATH_RRN_FRAME_CAPACITY,
};

union v90_digital_path_downstream {
	struct pcm_downstream_tx training;
	struct v90_downstream_data_stream data;
	struct v90_rd_tx rd;
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
	struct v34_received_mapping_frame
		pending_upstream_frames[
			V90_DIGITAL_PATH_PENDING_FRAME_CAPACITY];
	/* CPt fixes the Phase 4 constellations and K. Rate renegotiation reuses
	 * those fields with the spectral-shaping parameters of the preceding
	 * data mode. */
	struct v90_pcm_mapping training_mapping;
	struct v90_pcm_mapping control_mapping;
	struct v90_pcm_mapping data_mapping;
	struct v90_phase4_cp_rx control_symbols;
	struct v90_mp_record local_mp;
	uint32_t enabled_downstream_rate_mask;
	uint16_t peer_upstream_rate_mask;
	/* Six-symbol silence phase, then 0..408 Rt/Rt-bar symbols. The completed
	 * downstream control coder stays owned across this uncoded interval. */
	uint16_t reconditioning_symbols;
	enum g711_law law;
	enum v34_constellation_size rate_renegotiation_constellation;
	enum v90_digital_path_downstream_stage downstream_stage;
	enum v90_digital_path_upstream_stage upstream_stage;
	uint8_t pending_upstream_head;
	uint8_t pending_upstream_count;
	uint8_t upstream_rate_2400_bps_units;
	bool data_mapping_ready;
	bool rate_renegotiation_active;
	bool cleardown_requested;
	bool failed;
};

enum v90_digital_path_result v90_digital_path_init(
	struct v90_digital_path *path,
	const struct v90_phase4_digital_handoff *handoff,
	uint32_t enabled_downstream_rate_mask,
	enum v34_constellation_size rate_renegotiation_constellation);

bool v90_digital_path_is_valid(const struct v90_digital_path *path);
bool v90_digital_path_has_failed(const struct v90_digital_path *path);
bool v90_digital_path_downstream_is_online(
	const struct v90_digital_path *path);
bool v90_digital_path_upstream_is_online(
	const struct v90_digital_path *path);
bool v90_digital_path_is_online(const struct v90_digital_path *path);

/* Drain accepted data, send Rd/Rd-bar, then a zero-rate MP. An existing
 * TRN2d/MP exchange instead completes its current record without another Rd.
 * COMPLETE is reported only after both complete MP records and spectral
 * output drain. */
enum v90_digital_path_result v90_digital_path_start_cleardown(
	struct v90_digital_path *path);

/* Clock the independent downstream transmitter or upstream receiver.  These
 * directional operations let a call controller keep outgoing data continuous
 * while it qualifies an incoming full-retrain preamble. */
enum v90_digital_path_result v90_digital_path_transmit(
	struct v90_digital_path *path,
	v90_downstream_frame_source source, void *source_context,
	uint8_t *transmit_octet);
enum v90_digital_path_result v90_digital_path_receive_sample(
	struct v90_digital_path *path, int16_t received_sample,
	struct v34_received_mapping_frame *received);

/* Consume one upstream line sample and produce one simultaneous G.711 network
 * character. Downstream output is generated before the received sample changes
 * control state, so
 * a CP acknowledgement affects the following output character. A successful
 * upstream mapping frame changes `received` and returns UPSTREAM_FRAME.
 *
 * A downstream source failure is retryable: it changes neither path nor
 * outputs and the same received sample remains pending. Invalid calls have
 * the same transactional behavior. Terminal path errors latch failure and
 * expose the output already generated for the accepted clock character. */
enum v90_digital_path_result v90_digital_path_sample(
	struct v90_digital_path *path, int16_t received_sample,
	v90_downstream_frame_source source, void *source_context,
	uint8_t *transmit_octet,
	struct v34_received_mapping_frame *received);

/* Clock a path whose serialized owner has already validated it for the
 * current media block. Outputs must be distinct and outside the path. The
 * owner must validate the path again before releasing that block. */
enum v90_digital_path_result v90_digital_path_sample_owned(
	struct v90_digital_path *path, int16_t received_sample,
	v90_downstream_frame_source source, void *source_context,
	uint8_t *transmit_octet,
	struct v34_received_mapping_frame *received);

const char *v90_digital_path_result_name(
	enum v90_digital_path_result result);

#endif /* V90_DIGITAL_PATH_H */
