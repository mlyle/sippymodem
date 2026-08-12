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
	uint16_t round_trip_delay_samples;
	size_t first_payload_bit_count;
	size_t second_payload_bit_count;
	int16_t nominal_amplitude;
	uint8_t carrier_tail_symbols;
	enum phase2_signal_role signal_role;
	enum phase2_info1_phase phase;
	enum phase2_info1_failure failure;
};

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

/* A mode adapter must resolve each pending peer frame exactly once. Accepting
 * advances the role-specific sequence; rejecting resumes acquisition and
 * charges the current sample to the peer deadline. */
enum phase2_info1_result phase2_info1_accept_peer(
	struct phase2_info1_exchange *exchange);
enum phase2_info1_result phase2_info1_reject_peer(
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
