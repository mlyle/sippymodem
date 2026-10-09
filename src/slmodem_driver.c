// SPDX-License-Identifier: BSD-3-Clause

#include "slmodem_driver.h"

#include "host.h"
#include "media.h"

#include <errno.h>

static int start_modem(struct modem *modem);
static int stop_modem(struct modem *modem);
static int control_modem(
	struct modem *modem, unsigned int command, unsigned long argument);

static struct slmodem_driver *owner_from_modem(
	struct modem *modem)
{
	struct slmodem_driver *driver;

	if (modem == NULL || modem->dev_data == NULL)
		return NULL;
	driver = modem->dev_data;
	if (driver->modem != modem || driver->host == NULL ||
	    driver->operations.start != start_modem ||
	    driver->operations.stop != stop_modem ||
	    driver->operations.ioctl != control_modem)
		return NULL;
	return driver;
}

static int signaling_error(enum sip_signaling_result result)
{
	switch (result) {
	case SIP_SIGNALING_OK:
		return 0;
	case SIP_SIGNALING_INVALID_ARGUMENT:
		return -EINVAL;
	case SIP_SIGNALING_NOT_OPEN:
		return -ENOTCONN;
	case SIP_SIGNALING_BUSY:
	case SIP_SIGNALING_CALL_ACTIVE:
		return -EBUSY;
	case SIP_SIGNALING_NO_CALL:
		return -ENXIO;
	case SIP_SIGNALING_LIBRARY_ERROR:
		return -EIO;
	}
	return -EIO;
}

static bool modem_media_is_compatible(const struct modem *modem)
{
	return modem != NULL &&
	       modem->srate == MEDIA_SAMPLE_RATE &&
	       modem->format == MODEM_FORMAT && modem->frag == MODEM_FRAG;
}

static int start_modem(struct modem *modem)
{
	struct slmodem_driver *driver = owner_from_modem(modem);

	if (driver == NULL || !modem_media_is_compatible(modem))
		return -EINVAL;
	if (driver->defer_hangup || driver->hangup_pending)
		return -EBUSY;
	if (modem->caller) {
		driver->last_signaling_result = modem_host_dial(
			driver->host,
			modem->dial_string[0] != '\0' ? modem->dial_string : NULL);
	} else if (!modem->command) {
		driver->last_signaling_result =
			modem_host_answer(driver->host);
	} else {
		return -EOPNOTSUPP;
	}
	return signaling_error(driver->last_signaling_result);
}

static int stop_modem(struct modem *modem)
{
	struct slmodem_driver *driver = owner_from_modem(modem);

	if (driver == NULL)
		return -EINVAL;
	if (driver->defer_hangup) {
		driver->hangup_pending = true;
		return 0;
	}
	driver->hangup_pending = false;
	driver->last_signaling_result = modem_host_hangup(driver->host);
	/* The legacy core treats any stop error as "still started" after it has
	 * already released its DSP state. Preserve the signaling result for the
	 * owner, but always let core teardown finish in the stopped state. */
	return 0;
}

static int control_modem(
	struct modem *modem, unsigned int command, unsigned long argument)
{
	if (owner_from_modem(modem) == NULL)
		return -EINVAL;
	switch (command) {
	case MDMCTL_SPEED:
		return argument == MEDIA_SAMPLE_RATE ? 0 : -EINVAL;
	case MDMCTL_SETFMT:
		return argument == MODEM_FORMAT ? 0 : -EINVAL;
	case MDMCTL_SETFRAG:
		return argument == MODEM_FRAG ? 0 : -EINVAL;
	case MDMCTL_GETFMTS:
		return MODEM_FORMAT;
	case MDMCTL_CODECTYPE:
		return CODEC_UNKNOWN;
	case MDMCTL_IODELAY:
	case MDMCTL_HOOKSTATE:
	case MDMCTL_SPEAKERVOL:
	case MDMCTL_GETSTAT:
	case MDMCTL_START:
	case MDMCTL_STOP:
		return 0;
	case MDMCTL_CAPABILITIES:
		return -EOPNOTSUPP;
	default:
		return -ENOTTY;
	}
}

bool slmodem_driver_init(
	struct slmodem_driver *driver)
{
	const struct slmodem_driver initialized = {
		.operations = {
			.name = "SIP media",
			.start = start_modem,
			.stop = stop_modem,
			.ioctl = control_modem,
		},
	};

	if (driver == NULL)
		return false;
	*driver = initialized;
	return true;
}

struct modem_driver *slmodem_driver_interface(
	struct slmodem_driver *driver)
{
	return driver != NULL && driver->operations.start == start_modem &&
		       driver->operations.stop == stop_modem &&
		       driver->operations.ioctl == control_modem ?
		       &driver->operations :
		       NULL;
}

bool slmodem_driver_attach(
	struct slmodem_driver *driver,
	struct modem *modem,
	struct modem_host *host)
{
	if (slmodem_driver_interface(driver) == NULL ||
	    modem == NULL || host == NULL || driver->modem != NULL ||
	    driver->host != NULL || modem->dev_data != NULL ||
	    !modem_media_is_compatible(modem) ||
	    modem->driver.start != start_modem ||
	    modem->driver.stop != stop_modem ||
	    modem->driver.ioctl != control_modem)
		return false;
	driver->modem = modem;
	driver->host = host;
	modem->dev_data = driver;
	return true;
}

bool slmodem_driver_detach(
	struct slmodem_driver *driver)
{
	if (driver == NULL || driver->modem == NULL || driver->host == NULL ||
	    driver->modem->dev_data != driver || driver->modem->started ||
	    driver->defer_hangup || driver->hangup_pending)
		return false;
	driver->modem->dev_data = NULL;
	driver->modem = NULL;
	driver->host = NULL;
	return true;
}

enum sip_signaling_result
slmodem_driver_last_signaling_result(
	const struct slmodem_driver *driver)
{
	return driver != NULL ? driver->last_signaling_result :
			      SIP_SIGNALING_INVALID_ARGUMENT;
}


enum sip_signaling_result slmodem_driver_flush_hangup(struct slmodem_driver *driver)
{
	if (driver == NULL || driver->modem == NULL || driver->host == NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	if (driver->defer_hangup)
		return SIP_SIGNALING_BUSY;
	if (!driver->hangup_pending)
		return SIP_SIGNALING_OK;
	driver->hangup_pending = false;
	driver->last_signaling_result = modem_host_hangup(driver->host);
	return driver->last_signaling_result;
}
