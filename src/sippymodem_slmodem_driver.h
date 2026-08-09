// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_SLMODEM_DRIVER_H
#define SIPPYMODEM_SLMODEM_DRIVER_H

#include "modem.h"
#include "sippymodem_sip_signaling.h"

#include <stdbool.h>

struct sippymodem_host;

struct sippymodem_slmodem_driver {
	struct modem_driver interface;
	struct modem *modem;
	struct sippymodem_host *host;
	enum sippymodem_sip_signaling_result last_signaling_result;
};

bool sippymodem_slmodem_driver_init(
	struct sippymodem_slmodem_driver *driver);
/* Borrowed interface copied by modem_create(). */
struct modem_driver *sippymodem_slmodem_driver_interface(
	struct sippymodem_slmodem_driver *driver);

/* All three objects must remain alive until detach; stop the modem first. */
bool sippymodem_slmodem_driver_attach(
	struct sippymodem_slmodem_driver *driver,
	struct modem *modem,
	struct sippymodem_host *host);
/* Detach is allowed only after the modem has stopped. */
bool sippymodem_slmodem_driver_detach(
	struct sippymodem_slmodem_driver *driver);

enum sippymodem_sip_signaling_result
sippymodem_slmodem_driver_last_signaling_result(
	const struct sippymodem_slmodem_driver *driver);

#endif
