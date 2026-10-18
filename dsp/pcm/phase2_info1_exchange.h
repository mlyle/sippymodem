// SPDX-License-Identifier: BSD-3-Clause

#ifndef PHASE2_INFO1_EXCHANGE_H
#define PHASE2_INFO1_EXCHANGE_H

#include "phase2_signal.h"
#include "v34_phase2_info_audio.h"
#include "v34_phase2_tone.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	PHASE2_INFO1_SIGNAL_B_WAIT_BASE_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 700 / 1000,
	PHASE2_INFO1_SIGNAL_A_WAIT_BASE_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 2,
	/* V.34 11.3.1.2.1 and V.90 9.3.2.1 specify 70 +/- 5 ms of
	 * silence after INFO1a.  The sample-owned exchange can represent the
	 * nominal interval exactly instead of rounding it to media blocks. */
	PHASE2_INFO1_POST_INFO1A_SILENCE_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 70 / 1000,
};

enum phase2_info1_result {
	PHASE2_INFO1_OK,
	PHASE2_INFO1_INVALID_ARGUMENT,
};

enum phase2_info1_phase {
	PHASE2_INFO1_TRANSMITTING,
	PHASE2_INFO1_CARRIER_TAIL,
	PHASE2_INFO1_WAITING_FOR_PEER,
	PHASE2_INFO1_PEER_FRAME_PENDING,
	PHASE2_INFO1_SIGNAL_A_REPLY_NEEDED,
	PHASE2_INFO1_RECOVERING,
	PHASE2_INFO1_SILENCE,
	PHASE2_INFO1_COMPLETE,
	PHASE2_INFO1_FAILED,
};

enum phase2_info1_failure {
	PHASE2_INFO1_NO_FAILURE,
	PHASE2_INFO1_PEER_TIMEOUT,
};

enum phase2_info1_event_kind {
	PHASE2_INFO1_EVENT_NONE,
	PHASE2_INFO1_EVENT_LOCAL_FIRST_SENT,
	PHASE2_INFO1_EVENT_PEER_FRAME_STARTED,
	PHASE2_INFO1_EVENT_PEER_INFOMARKS,
	PHASE2_INFO1_EVENT_PEER_FRAME_PENDING,
	PHASE2_INFO1_EVENT_LOCAL_SECOND_SENT,
	PHASE2_INFO1_EVENT_PEER_FRAME_REJECTED,
	PHASE2_INFO1_EVENT_FAILED,
};

struct phase2_info1_event {
	struct v34_phase2_info_event frame_event;
	enum phase2_info1_event_kind kind;
	enum phase2_info1_failure failure;
};

struct phase2_info1_exchange {
	struct v34_phase2_info_audio_transmitter transmitter;
	struct v34_phase2_info_audio_receiver receiver;
	struct v34_phase2_tone_transmitter signal_a_tone;
	uint32_t peer_deadline_elapsed_samples;
	/* Samples after LOCAL_FIRST_SENT, including recovery and accepted-frame
	 * handoff. Unlike the response deadline this clock does not stop at a
	 * timeout. Saturates at UINT32_MAX; zero before the first record ends. */
	uint32_t first_record_elapsed_samples;
	/* short_exchange selects a remaining budget instead of full-startup RTD. */
	union {
		uint16_t round_trip_delay_samples;
		uint16_t short_receive_budget_samples;
	};
	uint16_t silence_samples;
	size_t first_payload_bit_count;
	size_t second_payload_bit_count;
	int16_t nominal_amplitude;
	uint8_t carrier_tail_symbols;
	enum phase2_signal_role signal_role;
	enum phase2_info1_phase phase;
	enum phase2_info1_failure failure;
	bool short_exchange;
};

/* Record ownership in a valid exchange, without revalidating DSP state.
 * Signal B supplies the first record (INFO1c/d), Signal A the second (INFO1a).
 * A pending peer frame is not yet owned; short exchanges have no first record. */
static inline bool phase2_info1_has_second_record(
	const struct phase2_info1_exchange *exchange)
{
	return exchange != NULL &&
	       (exchange->phase == PHASE2_INFO1_SILENCE ||
		exchange->phase == PHASE2_INFO1_COMPLETE ||
		(exchange->signal_role == PHASE2_SIGNAL_A &&
		 (exchange->phase == PHASE2_INFO1_TRANSMITTING ||
		  exchange->phase == PHASE2_INFO1_CARRIER_TAIL)));
}

static inline bool phase2_info1_has_first_record(
	const struct phase2_info1_exchange *exchange)
{
	return exchange != NULL && !exchange->short_exchange &&
	       (exchange->signal_role == PHASE2_SIGNAL_B ||
		exchange->phase == PHASE2_INFO1_SIGNAL_A_REPLY_NEEDED ||
		phase2_info1_has_second_record(exchange));
}

/* One-way INFO1a after short ranging. Signal A starts phase-continuously
 * from the retained tone; Signal B emits silence and receives the record.
 * Its budget is the remaining interval from the original B reversal, not
 * a fresh protocol timeout. No INFO1d or synthetic peer record is created. */
enum phase2_info1_result phase2_info1_init_short_signal_a(
	struct phase2_info1_exchange *exchange,
	const struct v34_phase2_tone_transmitter *signal_a_tone,
	const uint8_t *payload, size_t payload_bit_count);
enum phase2_info1_result phase2_info1_init_short_signal_b(
	struct phase2_info1_exchange *exchange,
	uint16_t remaining_samples, size_t payload_bit_count);

/* Signal B sends the first record immediately and then waits for Signal A's
 * second record. The supplied payload is copied into bounded transmitter
 * state. */
enum phase2_info1_result phase2_info1_init_signal_b(
	struct phase2_info1_exchange *exchange,
	int16_t nominal_amplitude, uint16_t round_trip_delay_samples,
	const uint8_t *first_payload, size_t first_payload_bit_count,
	size_t second_payload_bit_count);

/* Signal A preserves the supplied Tone A oscillator while receiving the
 * first record. elapsed_samples retains the reciprocal-probe reversal epoch
 * used by the two-second-plus-two-RTD deadline. */
enum phase2_info1_result phase2_info1_init_signal_a(
	struct phase2_info1_exchange *exchange,
	const struct v34_phase2_tone_transmitter *signal_a_tone,
	int16_t nominal_amplitude, uint16_t round_trip_delay_samples,
	uint32_t elapsed_samples, size_t first_payload_bit_count,
	size_t second_payload_bit_count);

bool phase2_info1_is_valid(const struct phase2_info1_exchange *exchange);

/* Reception continues across the timeout-recovery boundary. Call owners use
 * the same interval for retrain-tone observation so a pre-deadline preamble
 * cannot lose its qualifying silence. This query does not validate state. */
bool phase2_info1_is_waiting_for_peer(
	const struct phase2_info1_exchange *exchange);

/* A mode adapter must resolve each pending peer frame exactly once. Accepting
 * advances the role-specific sequence; rejecting resumes acquisition and
 * charges the current sample to an unexpired peer deadline. A late rejection
 * instead preserves recovery at the expired deadline. */
enum phase2_info1_result phase2_info1_accept_peer(
	struct phase2_info1_exchange *exchange);
enum phase2_info1_result phase2_info1_reject_peer(
	struct phase2_info1_exchange *exchange);

/* A mode owner may replace the role-specific peer deadline with a recovery
 * procedure. Recovery preserves the INFO receiver and local waveform: Signal
 * B remains silent while Signal A retains Tone A. A late peer INFO frame can
 * therefore complete the same exchange without reconstructing Phase 2.
 * Retained Tone A is only an ownership handoff: a Signal A call owner must
 * initiate full retrain or supply INFOMARKSa, not remain on steady tone.
 * This recovery is unavailable to short exchanges: their owner must send
 * Tone B and return to full Phase 2 after a missing INFO1a (V.92 9.4.1.2.3). */
enum phase2_info1_result phase2_info1_recover(
	struct phase2_info1_exchange *exchange);

/* Restart Signal B's retained first record after a mode owner recognizes a
 * peer recovery marker. The peer receiver and deadline restart with the
 * record; Signal A and ordinary first transmission are not repeatable. */
enum phase2_info1_result phase2_info1_repeat_signal_b(
	struct phase2_info1_exchange *exchange);

/* Supply Signal A's selected response after its first peer record is accepted.
 * The DPSK carrier begins phase-continuously from the retained Tone A. */
enum phase2_info1_result phase2_info1_reply_signal_a(
	struct phase2_info1_exchange *exchange,
	const uint8_t *second_payload, size_t second_payload_bit_count);

/* One call consumes and produces one 8 kHz line sample. The caller must accept
 * or reject PEER_FRAME_PENDING before requesting another sample. Terminal
 * states are silent and stable. */
enum phase2_info1_result phase2_info1_sample(
	struct phase2_info1_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_info1_event *event);

const char *phase2_info1_result_name(enum phase2_info1_result result);

#endif
