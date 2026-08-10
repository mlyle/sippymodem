// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_baudot_datapump.h"

#include "modem.h"
#include "modem_param.h"
#include "sippymodem_baudot.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
	BAUDOT_TERMINAL_RECEIVE_CHUNK = 64,
	BAUDOT_MAX_CODES_PER_CHARACTER = 3,
};

struct sippymodem_baudot_datapump {
	struct dp dp;
	struct sippymodem_baudot_transmitter transmitter;
	struct sippymodem_baudot_receiver receiver;
	uint8_t terminal_receive[BAUDOT_TERMINAL_RECEIVE_CHUNK];
	size_t terminal_receive_offset;
	size_t terminal_receive_count;
	uint8_t held_terminal_character;
	bool held_terminal_character_valid;
	bool receive_suppressed;
	bool linked;
};

static struct dp *create_baudot(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations);
static int delete_baudot(struct dp *dp);
static int process_baudot(
	struct dp *dp, void *receive, void *transmit, int count);

static struct dp_operations baudot_operations = {
	.name = "sippymodem Baudot TTY/TDD",
	.create = create_baudot,
	.delete = delete_baudot,
	.process = process_baudot,
};

static struct sippymodem_baudot_datapump *baudot_from_dp(struct dp *dp)
{
	struct sippymodem_baudot_datapump *datapump;

	if (dp == NULL || dp->id != DP_BAUDOT ||
	    dp->op != &baudot_operations || dp->modem == NULL ||
	    dp->dp_data == NULL)
		return NULL;
	datapump = dp->dp_data;
	if (&datapump->dp != dp ||
	    datapump->terminal_receive_count >
		    sizeof(datapump->terminal_receive) ||
	    datapump->terminal_receive_offset >
		    datapump->terminal_receive_count)
		return NULL;
	return datapump;
}

static bool selected_rate(
	const struct modem *modem, enum sippymodem_baudot_rate *rate)
{
	if (modem == NULL || rate == NULL)
		return false;
	if (modem->min_rate <= SIPPYMODEM_BAUDOT_RATE_45 &&
	    modem->max_rate >= SIPPYMODEM_BAUDOT_RATE_45) {
		*rate = SIPPYMODEM_BAUDOT_RATE_45;
		return true;
	}
	if (modem->min_rate <= SIPPYMODEM_BAUDOT_RATE_50 &&
	    modem->max_rate >= SIPPYMODEM_BAUDOT_RATE_50) {
		*rate = SIPPYMODEM_BAUDOT_RATE_50;
		return true;
	}
	return false;
}

static struct dp *create_baudot(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations)
{
	struct sippymodem_baudot_datapump *datapump;
	enum sippymodem_baudot_rate rate;

	if (modem == NULL || id != DP_BAUDOT ||
	    (caller != 0 && caller != 1) || sample_rate != MODEM_RATE ||
	    max_fragment != MODEM_FRAG || operations != &baudot_operations ||
	    operations->use_count == INT_MAX || !selected_rate(modem, &rate))
		return NULL;
	datapump = calloc(1, sizeof(*datapump));
	if (datapump == NULL)
		return NULL;
	if (sippymodem_baudot_transmitter_init(
		    &datapump->transmitter, rate) != SIPPYMODEM_BAUDOT_OK ||
	    sippymodem_baudot_receiver_init(
		    &datapump->receiver, rate) != SIPPYMODEM_BAUDOT_OK) {
		free(datapump);
		return NULL;
	}
	datapump->dp = (struct dp){
		.id = id,
		.modem = modem,
		.status = DPSTAT_OK,
		.op = operations,
		.dp_data = datapump,
	};
	operations->use_count++;
	return &datapump->dp;
}

static int delete_baudot(struct dp *dp)
{
	struct sippymodem_baudot_datapump *datapump = baudot_from_dp(dp);

	if (datapump == NULL || baudot_operations.use_count <= 0)
		return -1;
	baudot_operations.use_count--;
	datapump->dp.dp_data = NULL;
	free(datapump);
	return 0;
}

static void queue_terminal_text(
	struct sippymodem_baudot_datapump *datapump)
{
	struct modem *modem = datapump->dp.modem;

	while (modem->get_chars != NULL &&
	       sippymodem_baudot_pending_codes(&datapump->transmitter) <=
		       SIPPYMODEM_BAUDOT_TX_CODE_CAPACITY -
			       BAUDOT_MAX_CODES_PER_CHARACTER) {
		if (!datapump->held_terminal_character_valid) {
			if (modem->get_chars(
				    modem,
				    (char *)&datapump->held_terminal_character,
				    1) != 1)
				break;
			datapump->held_terminal_character_valid = true;
		}
		if (sippymodem_baudot_queue_text(
			    &datapump->transmitter,
			    &datapump->held_terminal_character, 1) != 1)
			break;
		datapump->held_terminal_character_valid = false;
	}
}

static void flush_terminal_receive(
	struct sippymodem_baudot_datapump *datapump)
{
	struct modem *modem = datapump->dp.modem;
	size_t remaining;
	int written;

	if (modem->put_chars == NULL ||
	    datapump->terminal_receive_offset >=
		    datapump->terminal_receive_count)
		return;
	remaining = datapump->terminal_receive_count -
		    datapump->terminal_receive_offset;
	written = modem->put_chars(
		modem,
		(const char *)datapump->terminal_receive +
			datapump->terminal_receive_offset,
		(int)remaining);
	if (written <= 0)
		return;
	if ((size_t)written > remaining)
		written = (int)remaining;
	datapump->terminal_receive_offset += (size_t)written;
	if (datapump->terminal_receive_offset ==
	    datapump->terminal_receive_count) {
		datapump->terminal_receive_offset = 0;
		datapump->terminal_receive_count = 0;
	}
}

static void collect_terminal_receive(
	struct sippymodem_baudot_datapump *datapump)
{
	if (datapump->terminal_receive_count != 0)
		return;
	datapump->terminal_receive_count = sippymodem_baudot_drain_text(
		&datapump->receiver, datapump->terminal_receive,
		sizeof(datapump->terminal_receive));
}

static enum sippymodem_baudot_result process_audio(
	struct sippymodem_baudot_datapump *datapump,
	const int16_t *receive,
	int16_t *transmit,
	size_t count)
{
	for (size_t index = 0; index < count; ++index) {
		bool transmitting = sippymodem_baudot_transmitting(
			&datapump->transmitter);

		if (transmitting) {
			if (!datapump->receive_suppressed) {
				sippymodem_baudot_receiver_reset(
					&datapump->receiver);
				datapump->receive_suppressed = true;
			}
			if (sippymodem_baudot_generate(
				    &datapump->transmitter, &transmit[index], 1) !=
			    SIPPYMODEM_BAUDOT_OK)
				return SIPPYMODEM_BAUDOT_INVALID_ARGUMENT;
			if (!sippymodem_baudot_transmitting(
				    &datapump->transmitter)) {
				sippymodem_baudot_receiver_reset(
					&datapump->receiver);
				datapump->receive_suppressed = false;
			}
		} else {
			int16_t input_sample = receive[index];

			if (datapump->receive_suppressed) {
				sippymodem_baudot_receiver_reset(
					&datapump->receiver);
				datapump->receive_suppressed = false;
			}
			transmit[index] = 0;
			if (sippymodem_baudot_receive(
				    &datapump->receiver, &input_sample, 1) !=
			    SIPPYMODEM_BAUDOT_OK)
				return SIPPYMODEM_BAUDOT_INVALID_ARGUMENT;
		}
	}
	return SIPPYMODEM_BAUDOT_OK;
}

static int enter_link(struct sippymodem_baudot_datapump *datapump)
{
	int rate = datapump->transmitter.rate == SIPPYMODEM_BAUDOT_RATE_50 ?
			   50 : 45;

	if (modem_set_param(datapump->dp.modem, MDMPRM_RX_RATE, rate) != 0 ||
	    modem_set_param(datapump->dp.modem, MDMPRM_TX_RATE, rate) != 0)
		return DPSTAT_ERROR;
	datapump->linked = true;
	datapump->dp.status = DPSTAT_CONNECT;
	return DPSTAT_CONNECT;
}

static int process_baudot(
	struct dp *dp, void *receive_buffer, void *transmit_buffer, int count)
{
	struct sippymodem_baudot_datapump *datapump = baudot_from_dp(dp);
	const int16_t *receive = receive_buffer;
	int16_t *transmit = transmit_buffer;

	if (datapump == NULL || receive == NULL || transmit == NULL ||
	    count <= 0 || count > MODEM_FRAG) {
		if (transmit != NULL && count > 0 && count <= MODEM_FRAG)
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
		return DPSTAT_ERROR;
	}
	flush_terminal_receive(datapump);
	queue_terminal_text(datapump);
	if (process_audio(
		    datapump, receive, transmit, (size_t)count) !=
	    SIPPYMODEM_BAUDOT_OK) {
		memset(transmit, 0, (size_t)count * sizeof(*transmit));
		return DPSTAT_ERROR;
	}
	collect_terminal_receive(datapump);
	flush_terminal_receive(datapump);
	return datapump->linked ? DPSTAT_OK : enter_link(datapump);
}

int sippymodem_baudot_datapump_register(void)
{
	return modem_dp_register(DP_BAUDOT, &baudot_operations);
}

int sippymodem_baudot_datapump_unregister(void)
{
	return modem_dp_deregister(DP_BAUDOT, &baudot_operations);
}

bool sippymodem_baudot_datapump_is_busy(void)
{
	return baudot_operations.use_count != 0;
}
