// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_PHASE2_H
#define PCM_PHASE2_H

#include "pcm_phase2_info1_exchange.h"

#include <stdbool.h>
#include <stdint.h>

enum pcm_phase2_stage {
	PCM_PHASE2_RANGING,
	PCM_PHASE2_INITIAL_PROBE,
	PCM_PHASE2_PROBE_TURNAROUND,
	PCM_PHASE2_RECIPROCAL_PROBE,
	PCM_PHASE2_INFO1,
};

enum pcm_phase2_result {
	PCM_PHASE2_OK,
	PCM_PHASE2_INVALID_ARGUMENT,
	PCM_PHASE2_INVALID_STATE,
};

enum pcm_phase2_failure {
	PCM_PHASE2_FAILURE_NONE,
	PCM_PHASE2_FAILURE_RANGING_TIMEOUT,
	PCM_PHASE2_FAILURE_REMOTE_PROBE_NOT_FOUND,
	PCM_PHASE2_FAILURE_LOCAL_PROBE_LIMIT,
	PCM_PHASE2_FAILURE_REMOTE_TONE_TIMEOUT,
	PCM_PHASE2_FAILURE_REMOTE_REVERSAL_TIMEOUT,
	PCM_PHASE2_FAILURE_INFO1_TIMEOUT,
	PCM_PHASE2_FAILURE_INTERNAL,
};

enum pcm_phase2_event_kind {
	PCM_PHASE2_EVENT_NONE,
	PCM_PHASE2_EVENT_ROUND_TRIP_DELAY,
	PCM_PHASE2_EVENT_INITIAL_PROBE_STARTED,
	PCM_PHASE2_EVENT_REMOTE_INITIAL_PROBE_MEASURED,
	PCM_PHASE2_EVENT_PROBE_TURNAROUND_STARTED,
	PCM_PHASE2_EVENT_RECIPROCAL_PROBE_STARTED,
	PCM_PHASE2_EVENT_READY_FOR_INFO1,
	PCM_PHASE2_EVENT_LOCAL_INFO1D_SENT,
	PCM_PHASE2_EVENT_PEER_INFO1D_RECEIVED,
	PCM_PHASE2_EVENT_LOCAL_INFO1A_SENT,
	PCM_PHASE2_EVENT_PEER_INFO1A_RECEIVED,
	PCM_PHASE2_EVENT_PEER_INFO1_REJECTED,
	PCM_PHASE2_EVENT_INFO1_RECOVERY_STARTED,
	PCM_PHASE2_EVENT_FAILED,
};

/* A compact recording boundary for Phase 2 progress. Round-trip delay is
 * selected by both the dedicated measurement event and the initial-probe
 * handoff, since Signal B completes its measurement at that handoff. Other
 * fields not selected by kind are zero. */
struct pcm_phase2_event {
	enum pcm_phase2_event_kind kind;
	enum pcm_phase2_failure failure;
	uint16_t round_trip_delay_samples;
};

union pcm_phase2_state {
	struct v90_phase2_ranging ranging;
	struct v90_phase2_initial_probe initial_probe;
	struct v90_phase2_probe_turnaround turnaround;
	struct v90_phase2_reciprocal_probe reciprocal_probe;
	struct pcm_phase2_info1_exchange info1;
};

/* Own one analog or digital modem's continuous INFO0, probing, and INFO1
 * timeline. Retained records and measurements survive waveform handoffs for
 * Phase 3 setup. A caller must serialize sample and control calls for one
 * instance; separate instances share no mutable state. */
struct pcm_phase2 {
	union pcm_phase2_state state;
	union pcm_info0_record local_info0;
	union pcm_info0_record peer_info0;
	struct v34_phase2_probe_receiver_measurement remote_probe;
	struct pcm_startup_selection startup;
	int16_t remote_receive_gain;
	enum pcm_phase2_stage stage;
	enum pcm_phase2_failure failure;
	enum v90_modem_role local_role;
	enum pcm_protocol info0_protocol;
	bool peer_info0_stored;
	bool remote_probe_stored;
};

/* The local INFO0 offer codec is immutable. V.92 initialization currently
 * admits the full Phase 2 branch only; short Phase 2 will be added as another
 * branch of this same owner. A V.92 offer may still negotiate down to V.90. */
enum pcm_phase2_result pcm_phase2_init_v90_analog(
	struct pcm_phase2 *phase2, int16_t nominal_amplitude,
	const struct v90_info0 *local_info0a);
enum pcm_phase2_result pcm_phase2_init_v90_digital(
	struct pcm_phase2 *phase2, int16_t nominal_amplitude,
	const struct v90_info0d *local_info0d);
enum pcm_phase2_result pcm_phase2_init_v92_analog(
	struct pcm_phase2 *phase2, int16_t nominal_amplitude,
	const struct v92_info0a *local_info0a);
enum pcm_phase2_result pcm_phase2_init_v92_digital(
	struct pcm_phase2 *phase2, int16_t nominal_amplitude,
	const struct v92_info0d *local_info0d);

/* Full retrain preserves the completed INFO0 agreement and begins directly
 * with the role tone after the call owner supplies the required silence. */
enum pcm_phase2_result pcm_phase2_init_retrain_v90_analog(
	struct pcm_phase2 *phase2, int16_t nominal_amplitude,
	const struct v90_info0 *local_info0a,
	const struct v90_info0d *peer_info0d);
enum pcm_phase2_result pcm_phase2_init_retrain_v90_digital(
	struct pcm_phase2 *phase2, int16_t nominal_amplitude,
	const struct v90_info0d *local_info0d,
	const struct v90_info0 *peer_info0a);
enum pcm_phase2_result pcm_phase2_init_retrain_v92_analog(
	struct pcm_phase2 *phase2, int16_t nominal_amplitude,
	const struct v92_info0a *local_info0a,
	const struct v92_info0d *peer_info0d);
enum pcm_phase2_result pcm_phase2_init_retrain_v92_digital(
	struct pcm_phase2 *phase2, int16_t nominal_amplitude,
	const struct v92_info0d *local_info0d,
	const struct v92_info0a *peer_info0a);

bool pcm_phase2_is_valid(const struct pcm_phase2 *phase2);
bool pcm_phase2_has_failed(const struct pcm_phase2 *phase2);
bool pcm_phase2_is_ready_for_info1(const struct pcm_phase2 *phase2);
bool pcm_phase2_is_ready_for_phase3(const struct pcm_phase2 *phase2);
/* A V.90 digital modem may receive a conventional V.34 INFO1a instead of
 * the PCM-selecting record. The completed Phase 2 owner remains available for
 * a typed handoff to the conventional call path. */
bool pcm_phase2_is_ready_for_v34_fallback(
	const struct pcm_phase2 *phase2);

/* Typed access prevents interpreting the role-dependent union through the
 * wrong member. Failed calls leave the destination unchanged. */
enum pcm_phase2_result pcm_phase2_peer_info0a(
	const struct pcm_phase2 *phase2, struct v90_info0 *peer_info0a);
enum pcm_phase2_result pcm_phase2_peer_info0d(
	const struct pcm_phase2 *phase2, struct v90_info0d *peer_info0d);
enum pcm_phase2_result pcm_phase2_peer_v92_info0a(
	const struct pcm_phase2 *phase2, struct v92_info0a *peer_info0a);
enum pcm_phase2_result pcm_phase2_peer_v92_info0d(
	const struct pcm_phase2 *phase2, struct v92_info0d *peer_info0d);

/* Copy the negotiated protocol and Phase 2 mode after peer INFO0 arrives. */
enum pcm_phase2_result pcm_phase2_startup_selection(
	const struct pcm_phase2 *phase2,
	struct pcm_startup_selection *selection);

/* The caller supplies mode policy only at the two explicit INFO1 boundaries.
 * The digital modem sends INFO1d immediately; the analog modem continues
 * Tone A until peer_info1d() succeeds and one reply function supplies INFO1a. */
enum pcm_phase2_result pcm_phase2_start_v90_digital_info1(
	struct pcm_phase2 *phase2, const struct v34_info1c *local_info1d);
enum pcm_phase2_result pcm_phase2_start_v92_digital_info1(
	struct pcm_phase2 *phase2, const struct v92_info1d *local_info1d);
enum pcm_phase2_result pcm_phase2_start_analog_info1(
	struct pcm_phase2 *phase2);
enum pcm_phase2_result pcm_phase2_reply_conventional(
	struct pcm_phase2 *phase2, const struct v90_info1a *local_info1a);
enum pcm_phase2_result pcm_phase2_reply_pcm(
	struct pcm_phase2 *phase2, const struct v92_pcm_info1a *local_info1a);

enum pcm_phase2_result pcm_phase2_peer_info1d(
	const struct pcm_phase2 *phase2, struct v34_info1c *peer_info1d);
enum pcm_phase2_result pcm_phase2_peer_info1a(
	const struct pcm_phase2 *phase2, struct v90_info1a *peer_info1a);
enum pcm_phase2_result pcm_phase2_peer_v92_info1d(
	const struct pcm_phase2 *phase2, struct v92_info1d *peer_info1d);
enum pcm_phase2_result pcm_phase2_peer_pcm_info1a(
	const struct pcm_phase2 *phase2, struct v92_pcm_info1a *peer_info1a);

/* Consume and produce exactly one 8 kHz line-terminal sample. A stage change
 * applies after the current sample, so the next call belongs to the new owner.
 * Successful calls always initialize both outputs. */
enum pcm_phase2_result pcm_phase2_sample(
	struct pcm_phase2 *phase2, int16_t received_sample,
	int16_t *transmit_sample, struct pcm_phase2_event *event);

/* Copy the completed remote probe measurement at or after the INFO1 boundary. */
enum pcm_phase2_result pcm_phase2_measure_remote_probe(
	const struct pcm_phase2 *phase2,
	struct v34_phase2_probe_receiver_measurement *measurement);
enum pcm_phase2_result pcm_phase2_receive_gain(
	const struct pcm_phase2 *phase2, int16_t *receive_gain);

const char *pcm_phase2_result_name(enum pcm_phase2_result result);
const char *pcm_phase2_failure_name(enum pcm_phase2_failure failure);

#endif /* PCM_PHASE2_H */
