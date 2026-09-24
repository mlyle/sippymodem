// SPDX-License-Identifier: BSD-3-Clause

#ifndef DATAPUMP_LINK_H
#define DATAPUMP_LINK_H

#include "modem_dp.h"
#include "modem.h"
#include "modem_param.h"
#include "packed_bits.h"

#include <limits.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Largest current host mapping burst: V.34 primary frame plus auxiliary bit. */
enum { DATAPUMP_LINK_MAX_BITS = 80 };

/* Set the host-facing link rates before reporting CONNECT.  Validate both
 * values before changing either rate; modem_set_param updates the modem's
 * data-link timer after each change. */
static inline bool datapump_set_link_rates(
	struct dp *dp, uint32_t transmit_rate, uint32_t receive_rate)
{
	if (dp == NULL || dp->modem == NULL || transmit_rate > INT_MAX ||
	    receive_rate > INT_MAX)
		return false;
	return modem_set_param(
		       dp->modem, MDMPRM_TX_RATE, (int)transmit_rate) == 0 &&
	       modem_set_param(
		       dp->modem, MDMPRM_RX_RATE, (int)receive_rate) == 0;
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

/* Deliver one bounded unpacked-bit frame without lending the decoder's
 * storage to a host callback, which may modify its input buffer. */
static inline bool datapump_deliver_bits(
	struct dp *dp, bool online, const uint8_t *bits, size_t bit_count)
{
	uint8_t copied[DATAPUMP_LINK_MAX_BITS];

	if (dp == NULL || dp->modem == NULL || bit_count > sizeof(copied) ||
	    !unpacked_bits_are_binary(bits, bit_count))
		return false;
	if (!online || dp->modem->put_bits == NULL)
		return true;
	memcpy(copied, bits, bit_count);
	return modem_put_bits(dp->modem, 1, copied, (int)bit_count) ==
	       (int)bit_count;
}

#endif
