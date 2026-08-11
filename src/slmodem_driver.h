// SPDX-License-Identifier: BSD-3-Clause

#ifndef SLMODEM_DRIVER_H
#define SLMODEM_DRIVER_H

#include "modem.h"
#include "sip_signaling.h"

#include <stdbool.h>

struct modem_host;

struct slmodem_driver {
	struct modem_driver interface;
	struct modem *modem;
	struct modem_host *host;
	enum sip_signaling_result last_signaling_result;
};

bool slmodem_driver_init(
	struct slmodem_driver *driver);
/* Borrowed interface copied by modem_create(). */
struct modem_driver *slmodem_driver_interface(
	struct slmodem_driver *driver);

/* All three objects must remain alive until detach; stop the modem first. */
bool slmodem_driver_attach(
	struct slmodem_driver *driver,
	struct modem *modem,
	struct modem_host *host);
/* Detach is allowed only after the modem has stopped. */
bool slmodem_driver_detach(
	struct slmodem_driver *driver);

enum sip_signaling_result
slmodem_driver_last_signaling_result(
	const struct slmodem_driver *driver);

#endif
