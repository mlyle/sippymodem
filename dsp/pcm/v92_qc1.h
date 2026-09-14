// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_QC1_H
#define V92_QC1_H

#include "pcm_protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* V.92 Tables 2/4/11/13. Calling/answering selects V.21 L/H;
 * analogue/digital selects the payload independently of the station role. */
struct v92_qc1_message {
	enum v90_modem_role sender;
	bool acknowledgment;
	bool lapm;
	union {
		uint8_t qts_index; /* 0..14 UQTS; 15 means cleardown from on-hold. */
		uint8_t anspcm_level; /* 0..3: -9.5, -12, -15, -18 dBm0. */
	} signal;
};

enum v92_qc1_receive_stage {
	V92_QC1_SEEK,
	V92_QC1_REPEAT,
	V92_QC1_ACK_MARKS,
};

struct v92_qc1_receiver {
	uint32_t window;
	uint16_t candidate;
	uint8_t window_bits;
	uint8_t elapsed_bits;
	enum v92_qc1_receive_stage stage;
};

/* One sequence, never implicit repetition. Zero count means invalid input. */
size_t v92_qc1_bit_count(const struct v92_qc1_message *message);
bool v92_qc1_bit(const struct v92_qc1_message *message, size_t index,
	uint8_t *bit);
bool v92_qc1_receiver_init(struct v92_qc1_receiver *receiver);
/* Confirm identical records at their wire spacing; QCA1 additionally consumes
 * its ten trailing marks. Invalid calls change no output or receiver state.
 * The message output is written only when complete is true. Carrier loss or
 * restarting Phase 1 requires reinitializing this bit-stream decoder. */
bool v92_qc1_receive_bit(struct v92_qc1_receiver *receiver, uint8_t bit,
	struct v92_qc1_message *message, bool *complete);

#endif
