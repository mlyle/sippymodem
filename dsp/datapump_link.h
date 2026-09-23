// SPDX-License-Identifier: BSD-3-Clause

#ifndef DATAPUMP_LINK_H
#define DATAPUMP_LINK_H

#include "modem_dp.h"
#include "modem_param.h"

#include <limits.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

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

#endif
