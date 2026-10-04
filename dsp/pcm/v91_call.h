// SPDX-License-Identifier: BSD-3-Clause
#ifndef V91_CALL_H
#define V91_CALL_H

#include "v91_control.h"
#include "v91_profile.h"
#include "v90_dil_sequence.h"
#include "v90_pcm_frame_encoder.h"
#include "v90_pcm_frame_decoder.h"
#include "pcm_binary_sign.h"

enum v91_tx_stage {
	V91_TX_SILENCE, V91_TX_EZ, V91_TX_INFO, V91_TX_J, V91_TX_PHIL,
	V91_TX_EM, V91_TX_DIL, V91_TX_SCR, V91_TX_CP, V91_TX_ES,
	V91_TX_B1, V91_TX_DATA,
};
enum v91_rx_stage {
	V91_RX_INFO, V91_RX_J, V91_RX_EM, V91_RX_DIL, V91_RX_CP,
	V91_RX_B1, V91_RX_DATA,
};
enum v91_failure {
	V91_FAILURE_NONE, V91_FAILURE_INFO_TIMEOUT, V91_FAILURE_TRAINING_TIMEOUT,
	V91_FAILURE_DESCRIPTOR, V91_FAILURE_CONSTELLATION, V91_FAILURE_CHANNEL,
	V91_FAILURE_MEDIA, V91_FAILURE_B1, V91_FAILURE_PEER_CLEARDOWN,
	V91_FAILURE_RECOVERY_EXHAUSTED, V91_FAILURE_LOCAL_CLEARDOWN, V91_FAILURE_TRANSPARENT, V91_FAILURE_CONTROL, V91_FAILURE_RETRAIN,
};
/* The caller owns one instance per call. Startup probes never share memory
 * between directions or calls. Bits are unpacked and in transmission order. */
struct v91_call {
	struct pcm_octet_probe probe;
	struct v91_info_receiver info_receiver;
	struct pcm_octet_channel channel;
	struct v90_dil_parameters requested_dil, peer_dil;
	struct v90_dil_sequence tx_dil, rx_dil;
	struct pcm_binary_sign tx_sign, rx_sign;
	struct v90_pcm_frame_encoder encoder;
	struct v90_pcm_frame_decoder decoder;
	struct v90_pcm_mapping rx_mapping;
	struct v91_info local_info, peer_info;
	struct v91_cp local_cp, peer_cp;
	uint8_t tx_record[V90_DIL_MAX_DESCRIPTOR_BYTES];
	uint8_t rx_record[V90_DIL_MAX_DESCRIPTOR_BYTES];
	uint8_t tx_control[12], rx_control[12];
	unsigned tx_control_position, rx_control_position;
	uint8_t tx_octets[6], rx_ucodes[6], rx_signs[6];
	size_t tx_length, tx_position, rx_length, rx_position;
	uint32_t samples, info_started, tx_count, rx_count, tx_dil_length, rx_dil_length;
	unsigned zeroes, ones;
	enum v91_tx_stage tx_stage;
	enum v91_rx_stage rx_stage;
	enum v91_failure failure;
	uint32_t recovery_deadline, cp_deadline, healthy_samples;
	uint8_t recovery_attempts;
	unsigned retrains;
	uint8_t maximum_frame_bits;
	bool recovering, closing, close_after_frame, cp_deadline_active;
	bool have_info, have_info_ack, have_peer_dil, have_cp, have_cp_ack, have_es;
};
struct v91_event {
	uint8_t bits[PCM_FRAME_MAX_BITS];
	uint8_t count;
	bool online, retraining;
	enum v91_failure failure;
};
typedef void (*v91_bit_source)(void *context, uint8_t *bits, size_t count);
bool v91_call_init(struct v91_call *call, enum g711_law law);
bool v91_call_hangup(struct v91_call *call);
const char *v91_failure_name(enum v91_failure failure);
bool v91_call_retrain(struct v91_call *call);
bool v91_call_process(struct v91_call *call, uint8_t receive, bool available,
	uint8_t *transmit, v91_bit_source source, void *context, struct v91_event *event);
#endif
