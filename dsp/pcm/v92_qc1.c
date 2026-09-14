// SPDX-License-Identifier: BSD-3-Clause

#include "v92_qc1.h"

enum {
	QC1_RECORD_BITS = 30,
	QC1_WINDOW_BITS = 22,
	QC1_WINDOW_MASK = (1u << QC1_WINDOW_BITS) - 1u,
	/* Last two marks + synchronization, matching the V.8 carrier-acquisition
	 * boundary: initial marks may establish lock rather than yield bits. */
	QC1_HEADER = 0xd55,
	QC1_WORD_MASK = 0x3ff,
};

size_t v92_qc1_bit_count(const struct v92_qc1_message *message)
{
	if (message == NULL || !pcm_modem_role_is_valid(message->sender) ||
	    (message->sender == V90_ANALOG_MODEM ?
		message->signal.qts_index > 15 : message->signal.anspcm_level > 3))
		return 0;
	return message->acknowledgment ? 70u : 60u;
}

static uint16_t message_word(const struct v92_qc1_message *message)
{
	uint16_t word = 1u | ((uint16_t)message->acknowledgment << 7) |
		((uint16_t)message->lapm << 6);

	if (message->sender == V90_DIGITAL_MODEM)
		return word | 0x100u |
			((uint16_t)message->signal.anspcm_level << 1);
	return word | ((message->signal.qts_index & 8u) << 2) |
		((message->signal.qts_index & 7u) << 1);
}

static uint8_t record_bit(uint16_t word, size_t index)
{
	if (index < 10)
		return 1;
	if (index < 20)
		return index & 1u;
	return (word >> (29u - index)) & 1u;
}

bool v92_qc1_bit(const struct v92_qc1_message *message, size_t index,
	uint8_t *bit)
{
	const size_t count = v92_qc1_bit_count(message);

	if (bit == NULL || count == 0 || index >= count)
		return false;
	*bit = record_bit(message_word(message), index % QC1_RECORD_BITS);
	return true;
}

static bool word_is_valid(uint16_t word)
{
	if (word > QC1_WORD_MASK)
		return false;
	/* Start/stop, role and reserved bits; QCA and LAPM are independent. */
	return word & 0x100u ? (word & 0x339u) == 0x101u :
		(word & 0x311u) == 1u;
}

static struct v92_qc1_message decode_word(uint16_t word)
{
	struct v92_qc1_message message = {
		.sender = word & 0x100u ? V90_DIGITAL_MODEM : V90_ANALOG_MODEM,
		.acknowledgment = (word & 0x80u) != 0,
		.lapm = (word & 0x40u) != 0,
	};

	if (message.sender == V90_DIGITAL_MODEM)
		message.signal.anspcm_level = (word >> 1) & 3u;
	else
		message.signal.qts_index =
			((word >> 1) & 7u) | ((word >> 2) & 8u);
	return message;
}

static bool receiver_is_valid(const struct v92_qc1_receiver *receiver)
{
	if (receiver == NULL || receiver->window_bits > QC1_WINDOW_BITS ||
	    receiver->window >= (1u << receiver->window_bits))
		return false;
	switch (receiver->stage) {
	case V92_QC1_SEEK:
		return receiver->candidate == 0 && receiver->elapsed_bits == 0;
	case V92_QC1_REPEAT:
		return receiver->window_bits == QC1_WINDOW_BITS &&
			word_is_valid(receiver->candidate) &&
			receiver->elapsed_bits < QC1_RECORD_BITS;
	case V92_QC1_ACK_MARKS:
		return receiver->window_bits == QC1_WINDOW_BITS &&
			word_is_valid(receiver->candidate) &&
			(receiver->candidate & 0x80u) != 0 &&
			receiver->elapsed_bits < 10;
	}
	return false;
}

bool v92_qc1_receiver_init(struct v92_qc1_receiver *receiver)
{
	if (receiver == NULL)
		return false;
	*receiver = (struct v92_qc1_receiver){ 0 };
	return true;
}

static void clear_candidate(struct v92_qc1_receiver *receiver)
{
	receiver->candidate = 0;
	receiver->elapsed_bits = 0;
	receiver->stage = V92_QC1_SEEK;
}

bool v92_qc1_receive_bit(struct v92_qc1_receiver *receiver, uint8_t bit,
	struct v92_qc1_message *message, bool *complete)
{
	struct v92_qc1_receiver next;
	uint16_t confirmed = 0;

	if (!receiver_is_valid(receiver) || bit > 1 ||
	    message == NULL || complete == NULL)
		return false;
	next = *receiver;
	next.window = ((next.window << 1) | bit) & QC1_WINDOW_MASK;
	if (next.window_bits < QC1_WINDOW_BITS)
		next.window_bits++;
	if (next.stage != V92_QC1_SEEK) {
		const bool acknowledgment = next.stage == V92_QC1_ACK_MARKS;
		const uint8_t expected = acknowledgment ? 1 :
			record_bit(next.candidate, next.elapsed_bits);

		/* All ten intervening marks belong to the confirmed sequence,
		 * unlike initial marks lost during carrier acquisition. */
		if (bit != expected)
			clear_candidate(&next);
		else if (++next.elapsed_bits == (acknowledgment ? 10 : 30)) {
			if (!acknowledgment && (next.candidate & 0x80u)) {
				next.stage = V92_QC1_ACK_MARKS;
				next.elapsed_bits = 0;
			} else {
				confirmed = next.candidate;
				clear_candidate(&next);
			}
		}
	}
	if (!confirmed && next.stage == V92_QC1_SEEK &&
	    next.window_bits == QC1_WINDOW_BITS &&
	    (next.window >> 10) == QC1_HEADER &&
	    word_is_valid(next.window & QC1_WORD_MASK)) {
		next.stage = V92_QC1_REPEAT;
		next.candidate = next.window & QC1_WORD_MASK;
	}
	*receiver = next;
	*complete = confirmed != 0;
	if (confirmed)
		*message = decode_word(confirmed);
	return true;
}
