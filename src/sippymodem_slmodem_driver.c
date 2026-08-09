// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_slmodem_driver.h"

#include "sippymodem_host.h"
#include "sippymodem_media.h"

#include <errno.h>

static int start_modem(struct modem *modem);
static int stop_modem(struct modem *modem);
static int control_modem(
	struct modem *modem, unsigned int command, unsigned long argument);

static struct sippymodem_slmodem_driver *owner_from_modem(
	struct modem *modem)
{
	struct sippymodem_slmodem_driver *driver;

	if (modem == NULL || modem->dev_data == NULL)
		return NULL;
	driver = modem->dev_data;
	if (driver->modem != modem || driver->host == NULL ||
	    driver->interface.start != start_modem ||
	    driver->interface.stop != stop_modem ||
	    driver->interface.ioctl != control_modem)
		return NULL;
	return driver;
}

static int signaling_error(enum sippymodem_sip_signaling_result result)
{
	switch (result) {
	case SIPPYMODEM_SIP_SIGNALING_OK:
		return 0;
	case SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT:
		return -EINVAL;
	case SIPPYMODEM_SIP_SIGNALING_NOT_OPEN:
		return -ENOTCONN;
	case SIPPYMODEM_SIP_SIGNALING_BUSY:
	case SIPPYMODEM_SIP_SIGNALING_CALL_ACTIVE:
		return -EBUSY;
	case SIPPYMODEM_SIP_SIGNALING_NO_CALL:
		return -ENXIO;
	case SIPPYMODEM_SIP_SIGNALING_LIBRARY_ERROR:
		return -EIO;
	}
	return -EIO;
}

static bool modem_media_is_compatible(const struct modem *modem)
{
	return modem != NULL &&
	       modem->srate == SIPPYMODEM_MEDIA_SAMPLE_RATE &&
	       modem->format == MODEM_FORMAT && modem->frag == MODEM_FRAG;
}

static int start_modem(struct modem *modem)
{
	struct sippymodem_slmodem_driver *driver = owner_from_modem(modem);

	if (driver == NULL || !modem_media_is_compatible(modem))
		return -EINVAL;
	if (modem->caller) {
		driver->last_signaling_result = sippymodem_host_dial(
			driver->host,
			modem->dial_string[0] != '\0' ? modem->dial_string : NULL);
	} else if (!modem->command) {
		driver->last_signaling_result =
			sippymodem_host_answer(driver->host);
	} else {
		return -EOPNOTSUPP;
	}
	return signaling_error(driver->last_signaling_result);
}

static int stop_modem(struct modem *modem)
{
	struct sippymodem_slmodem_driver *driver = owner_from_modem(modem);

	if (driver == NULL)
		return -EINVAL;
	driver->last_signaling_result = sippymodem_host_hangup(driver->host);
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
		return argument == SIPPYMODEM_MEDIA_SAMPLE_RATE ? 0 : -EINVAL;
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

bool sippymodem_slmodem_driver_init(
	struct sippymodem_slmodem_driver *driver)
{
	const struct sippymodem_slmodem_driver initialized = {
		.interface = {
			.name = "sippymodem SIP media",
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

struct modem_driver *sippymodem_slmodem_driver_interface(
	struct sippymodem_slmodem_driver *driver)
{
	return driver != NULL && driver->interface.start == start_modem &&
		       driver->interface.stop == stop_modem &&
		       driver->interface.ioctl == control_modem ?
		       &driver->interface :
		       NULL;
}

bool sippymodem_slmodem_driver_attach(
	struct sippymodem_slmodem_driver *driver,
	struct modem *modem,
	struct sippymodem_host *host)
{
	if (sippymodem_slmodem_driver_interface(driver) == NULL ||
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

bool sippymodem_slmodem_driver_detach(
	struct sippymodem_slmodem_driver *driver)
{
	if (driver == NULL || driver->modem == NULL || driver->host == NULL ||
	    driver->modem->dev_data != driver || driver->modem->started)
		return false;
	driver->modem->dev_data = NULL;
	driver->modem = NULL;
	driver->host = NULL;
	return true;
}

enum sippymodem_sip_signaling_result
sippymodem_slmodem_driver_last_signaling_result(
	const struct sippymodem_slmodem_driver *driver)
{
	return driver != NULL ? driver->last_signaling_result :
			      SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;
}
