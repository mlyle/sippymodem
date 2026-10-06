// SPDX-License-Identifier: BSD-3-Clause

#ifndef DATAPUMP_LINK_H
#define DATAPUMP_LINK_H

#include "modem_dp.h"
#include "modem.h"
#include "packed_bits.h"
#include "training_session.h"

#include <limits.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Largest current host mapping burst: V.34 primary frame plus auxiliary bit. */
enum { DATAPUMP_LINK_MAX_BITS = 80 };

static inline struct dp_link_rates datapump_get_link_rates(const struct dp *dp)
{
	struct dp_link_rates rates = { 0 };

	if (dp == NULL || dp->modem == NULL || dp->op == NULL)
		return rates;
	if (dp->op->link_rates != NULL)
		return dp->op->link_rates(dp);
	/* Fixed-symbol-rate families publish their bit rates at CONNECT.
	 * Do not expose a previous call's host rates during fresh training. */
	if (dp->status != DPSTAT_CONNECT)
		return rates;
	rates.transmit_bps = dp->modem->tx_rate;
	rates.receive_bps = dp->modem->rx_rate;
	switch (dp->id) {
	case DP_V21:
	case DP_B103:
	case DP_V23:
		rates.transmit_baud = rates.transmit_bps;
		rates.receive_baud = rates.receive_bps;
		break;
	case DP_V22:
	case DP_V22BIS:
	case DP_B212:
		rates.transmit_baud = rates.receive_baud = 600;
		break;
	case DP_V32:
	case DP_V32BIS:
		rates.transmit_baud = rates.receive_baud = 2400;
		break;
	case DP_BAUDOT:
		rates.transmit_baud = rates.receive_baud =
			rates.transmit_bps == 50 ? 50.0 : 500.0 / 11.0;
		break;
	default:
		break;
	}
	return rates;
}

/* Publish a completed physical link before delivering its first data bits.
 * Retraining resumes retained HDLC/LAPM state through the common host path. */
static inline bool datapump_complete_training(
	struct dp *dp, struct training_session *training)
{
	if (dp == NULL || dp->modem == NULL)
		return false;
	bool retrained = training_session_is_retraining(training);
	if (training_session_connect(training) != TRAINING_SESSION_OK)
		return false;
	dp->status = DPSTAT_CONNECT;
	/* A negotiation parent publishes the child after adopting it. */
	if (dp->modem->dp != dp)
		return true;
	if (retrained)
		modem_dp_reconnected(dp->modem);
	else
		modem_dp_connected(dp->modem);
	return true;
}

/* Publish both link rates before CONNECT, rescaling data-link timers once. */
static inline bool datapump_set_link_rates(
	struct dp *dp, uint32_t transmit_rate, uint32_t receive_rate)
{
	return dp != NULL &&
	       modem_set_link_rates(dp->modem, transmit_rate, receive_rate) == 0;
}

/* Supply one-bit host data in the online state. A short or unavailable read
 * completes the frame with mark bits; only the reported prefix is payload. */
static inline bool datapump_source_bits(
	struct dp *dp, bool online, uint8_t *bits, size_t bit_count)
{
	size_t supplied = 0;

	if (dp == NULL || dp->modem == NULL || bits == NULL ||
	    bit_count > INT_MAX)
		return false;
	if (online) {
		int count = modem_get_bits(dp->modem, 1, bits, (int)bit_count);

		if (count > 0)
			supplied = (size_t)count < bit_count ? (size_t)count : bit_count;
		for (size_t index = 0; index < supplied; ++index)
			bits[index] &= 1u;
	}
	memset(bits + supplied, 1, bit_count - supplied);
	return true;
}

/* Read one host frame before separating its optional auxiliary bit. */
static inline bool datapump_source_split_bits(
	struct dp *dp, bool online, uint8_t *primary_bits,
	size_t primary_count, uint8_t *auxiliary_bit)
{
	uint8_t bits[DATAPUMP_LINK_MAX_BITS];
	size_t auxiliary_count = auxiliary_bit != NULL ? 1u : 0u;

	if (primary_bits == NULL ||
	    primary_count > DATAPUMP_LINK_MAX_BITS - auxiliary_count ||
	    !datapump_source_bits(
		    dp, online, bits, primary_count + auxiliary_count))
		return false;
	memcpy(primary_bits, bits, primary_count);
	if (auxiliary_bit != NULL)
		*auxiliary_bit = bits[primary_count];
	return true;
}

/* Deliver one bounded unpacked-bit frame without lending the decoder's
 * storage to a host callback, which may modify its input buffer. */
static inline bool datapump_deliver_bits(
	struct dp *dp, bool online, const uint8_t *bits, size_t bit_count)
{
	uint8_t copied[DATAPUMP_LINK_MAX_BITS];

	if (dp == NULL || dp->modem == NULL || bit_count > sizeof(copied) ||
	    !unpacked_bits_are_binary(bits, bit_count))
		return false;
	if (!online)
		return true;
	memcpy(copied, bits, bit_count);
	return modem_put_bits(dp->modem, 1, copied, (int)bit_count) ==
	       (int)bit_count;
}

#endif
