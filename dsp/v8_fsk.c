// SPDX-License-Identifier: BSD-3-Clause

#include "v8_fsk.h"

#include "v21.h"

enum {
	/* A quarter-scale peak leaves headroom for companding and media gain. */
	V8_FSK_TRANSMIT_AMPLITUDE = 8192,
};

static bool role_channels(
	enum sippymodem_v8_station_role role,
	const struct sippymodem_fsk_channel **transmit,
	const struct sippymodem_fsk_channel **receive)
{
	if (role == SIPPYMODEM_V8_CALLING_STATION) {
		*transmit = &sippymodem_v21_channel_1;
		*receive = &sippymodem_v21_channel_2;
		return true;
	}
	if (role == SIPPYMODEM_V8_ANSWERING_STATION) {
		*transmit = &sippymodem_v21_channel_2;
		*receive = &sippymodem_v21_channel_1;
		return true;
	}
	return false;
}

static bool channels_equal(
	const struct sippymodem_fsk_channel *left,
	const struct sippymodem_fsk_channel *right)
{
	return left->sample_rate == right->sample_rate &&
	       left->symbol_rate_numerator == right->symbol_rate_numerator &&
	       left->symbol_rate_denominator == right->symbol_rate_denominator &&
	       left->mark_frequency_hz == right->mark_frequency_hz &&
	       left->space_frequency_hz == right->space_frequency_hz &&
	       left->detector_window == right->detector_window &&
	       left->metric_smoothing_shift == right->metric_smoothing_shift;
}

static bool fsk_is_configured(const struct sippymodem_v8_fsk *fsk)
{
	const struct sippymodem_fsk_channel *transmit;
	const struct sippymodem_fsk_channel *receive;

	return fsk != NULL && role_channels(fsk->role, &transmit, &receive) &&
	       channels_equal(&fsk->transmitter.channel, transmit) &&
	       channels_equal(&fsk->receiver.channel, receive) &&
	       fsk->transmitter.tones.amplitude == V8_FSK_TRANSMIT_AMPLITUDE;
}

enum sippymodem_v8_fsk_result sippymodem_v8_fsk_init(
	struct sippymodem_v8_fsk *fsk,
	enum sippymodem_v8_station_role role)
{
	const struct sippymodem_fsk_channel *transmit;
	const struct sippymodem_fsk_channel *receive;
	struct sippymodem_v8_fsk initialized = { 0 };

	if (fsk == NULL || !role_channels(role, &transmit, &receive))
		return SIPPYMODEM_V8_FSK_INVALID_ARGUMENT;
	if (sippymodem_fsk_transmitter_init(
		    &initialized.transmitter, transmit,
		    V8_FSK_TRANSMIT_AMPLITUDE) != SIPPYMODEM_FSK_OK ||
	    sippymodem_fsk_receiver_init(&initialized.receiver, receive) !=
		    SIPPYMODEM_FSK_OK)
		return SIPPYMODEM_V8_FSK_INVALID_ARGUMENT;
	initialized.role = role;
	*fsk = initialized;
	return SIPPYMODEM_V8_FSK_OK;
}

enum sippymodem_v8_fsk_result sippymodem_v8_fsk_reset(
	struct sippymodem_v8_fsk *fsk)
{
	if (fsk == NULL ||
	    (fsk->role != SIPPYMODEM_V8_CALLING_STATION &&
	     fsk->role != SIPPYMODEM_V8_ANSWERING_STATION))
		return SIPPYMODEM_V8_FSK_INVALID_ARGUMENT;
	return sippymodem_v8_fsk_init(fsk, fsk->role);
}

bool sippymodem_v8_fsk_can_accept_bit(
	const struct sippymodem_v8_fsk *fsk)
{
	return fsk_is_configured(fsk) &&
	       sippymodem_fsk_transmitter_can_accept_bit(&fsk->transmitter);
}

enum sippymodem_v8_fsk_result sippymodem_v8_fsk_transmit_sample(
	struct sippymodem_v8_fsk *fsk, uint8_t bit, int16_t *sample,
	bool *bit_consumed)
{
	int16_t generated;
	bool consumed;

	if (!fsk_is_configured(fsk) || bit > 1 || sample == NULL ||
	    bit_consumed == NULL ||
	    sippymodem_fsk_transmitter_sample(
		    &fsk->transmitter, bit, &generated, &consumed) !=
		    SIPPYMODEM_FSK_OK)
		return SIPPYMODEM_V8_FSK_INVALID_ARGUMENT;
	*sample = generated;
	*bit_consumed = consumed;
	return SIPPYMODEM_V8_FSK_OK;
}

enum sippymodem_v8_fsk_result sippymodem_v8_fsk_receive_sample(
	struct sippymodem_v8_fsk *fsk, int16_t sample, uint8_t *bit,
	bool *bit_ready)
{
	uint8_t received;
	bool ready;

	if (!fsk_is_configured(fsk) || bit == NULL || bit_ready == NULL ||
	    sippymodem_fsk_receiver_sample(
		    &fsk->receiver, sample, &received, &ready) !=
		    SIPPYMODEM_FSK_OK)
		return SIPPYMODEM_V8_FSK_INVALID_ARGUMENT;
	*bit = received;
	*bit_ready = ready;
	return SIPPYMODEM_V8_FSK_OK;
}

const char *sippymodem_v8_fsk_result_name(
	enum sippymodem_v8_fsk_result result)
{
	switch (result) {
	case SIPPYMODEM_V8_FSK_OK:
		return "ok";
	case SIPPYMODEM_V8_FSK_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown V.8 FSK result";
}
