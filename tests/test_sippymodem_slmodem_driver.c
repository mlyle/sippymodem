// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_slmodem_driver.h>

#include "test.h"

#include <errno.h>
#include <string.h>

struct sippymodem_host {
	unsigned int marker;
};

struct host_capture {
	struct sippymodem_host *expected_host;
	unsigned int dials;
	unsigned int answers;
	unsigned int hangups;
	char target[sizeof(((struct modem *)0)->dial_string)];
	bool target_was_null;
	enum sippymodem_sip_signaling_result dial_result;
	enum sippymodem_sip_signaling_result answer_result;
	enum sippymodem_sip_signaling_result hangup_result;
};

static struct host_capture capture;

enum sippymodem_sip_signaling_result sippymodem_host_dial(
	struct sippymodem_host *host, const char *target)
{
	capture.dials++;
	if (host != capture.expected_host)
		return SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;
	capture.target_was_null = target == NULL;
	if (target != NULL)
		snprintf(capture.target, sizeof(capture.target), "%s", target);
	else
		capture.target[0] = '\0';
	return capture.dial_result;
}

enum sippymodem_sip_signaling_result sippymodem_host_answer(
	struct sippymodem_host *host)
{
	capture.answers++;
	return host == capture.expected_host ?
		       capture.answer_result :
		       SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;
}

enum sippymodem_sip_signaling_result sippymodem_host_hangup(
	struct sippymodem_host *host)
{
	capture.hangups++;
	return host == capture.expected_host ?
		       capture.hangup_result :
		       SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;
}

static struct modem configured_modem(
	struct sippymodem_slmodem_driver *driver)
{
	struct modem modem;

	memset(&modem, 0, sizeof(modem));
	modem.driver = *sippymodem_slmodem_driver_interface(driver);
	modem.srate = MODEM_RATE;
	modem.format = MODEM_FORMAT;
	modem.frag = MODEM_FRAG;
	return modem;
}

static bool attach_fixture(
	struct sippymodem_slmodem_driver *driver,
	struct modem *modem,
	struct sippymodem_host *host)
{
	memset(&capture, 0, sizeof(capture));
	capture.expected_host = host;
	if (!sippymodem_slmodem_driver_init(driver))
		return false;
	*modem = configured_modem(driver);
	return sippymodem_slmodem_driver_attach(driver, modem, host);
}

static bool test_attachment_lifetime(void)
{
	struct sippymodem_slmodem_driver driver = { 0 };
	struct sippymodem_slmodem_driver original = driver;
	struct sippymodem_host host = { .marker = 1 };
	struct modem modem;
	struct modem_driver *interface;

	CHECK(!sippymodem_slmodem_driver_init(NULL) &&
		      sippymodem_slmodem_driver_interface(NULL) == NULL &&
		      !sippymodem_slmodem_driver_attach(NULL, NULL, NULL) &&
		      !sippymodem_slmodem_driver_detach(NULL) &&
		      sippymodem_slmodem_driver_last_signaling_result(NULL) ==
			      SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT &&
		      driver.modem == original.modem &&
		      driver.host == original.host,
	      "driver accepted incomplete ownership");
	CHECK(sippymodem_slmodem_driver_init(&driver),
	      "driver initialization failed");
	interface = sippymodem_slmodem_driver_interface(&driver);
	CHECK(interface != NULL && interface->start != NULL &&
		      interface->stop != NULL && interface->ioctl != NULL,
	      "driver did not expose a complete modem interface");
	modem = configured_modem(&driver);
	modem.srate++;
	CHECK(!sippymodem_slmodem_driver_attach(&driver, &modem, &host) &&
		      modem.dev_data == NULL && driver.modem == NULL &&
		      driver.host == NULL,
	      "driver partially attached incompatible modem media");
	modem.srate = MODEM_RATE;
	CHECK(sippymodem_slmodem_driver_attach(&driver, &modem, &host) &&
		      modem.dev_data == &driver && driver.modem == &modem &&
		      driver.host == &host,
	      "driver did not attach borrowed owners");
	CHECK(!sippymodem_slmodem_driver_attach(&driver, &modem, &host),
	      "driver attached twice");
	modem.started = 1;
	CHECK(!sippymodem_slmodem_driver_detach(&driver),
	      "driver detached a running modem");
	modem.started = 0;
	CHECK(sippymodem_slmodem_driver_detach(&driver) &&
		      modem.dev_data == NULL && driver.modem == NULL &&
		      driver.host == NULL &&
		      !sippymodem_slmodem_driver_detach(&driver),
	      "driver detach did not release borrowed owners exactly once");
	return true;
}

static bool test_routes_call_control(void)
{
	struct sippymodem_slmodem_driver driver;
	struct sippymodem_host host = { .marker = 2 };
	struct modem modem;
	struct modem_driver *interface;

	CHECK(attach_fixture(&driver, &modem, &host),
	      "call-control fixture failed");
	interface = sippymodem_slmodem_driver_interface(&driver);
	capture.dial_result = SIPPYMODEM_SIP_SIGNALING_OK;
	modem.caller = 1;
	snprintf(modem.dial_string, sizeof(modem.dial_string), "%s",
		 "peer@example.test");
	CHECK(interface->start(&modem) == 0 && capture.dials == 1 &&
		      !capture.target_was_null &&
		      strcmp(capture.target, modem.dial_string) == 0,
	      "outgoing modem start did not dial its exact target");
	modem.dial_string[0] = '\0';
	CHECK(interface->start(&modem) == 0 && capture.dials == 2 &&
		      capture.target_was_null,
	      "configured target fallback was not preserved");

	modem.caller = 0;
	modem.command = 0;
	capture.answer_result = SIPPYMODEM_SIP_SIGNALING_OK;
	CHECK(interface->start(&modem) == 0 && capture.answers == 1,
	      "answering modem start did not answer SIP");
	modem.command = 1;
	CHECK(interface->start(&modem) == -EOPNOTSUPP && capture.answers == 1,
	      "non-call audio start reached SIP call control");

	capture.hangup_result = SIPPYMODEM_SIP_SIGNALING_NO_CALL;
	CHECK(interface->stop(&modem) == 0 && capture.hangups == 1,
	      "already-ended remote call made modem stop fail");
	capture.hangup_result = SIPPYMODEM_SIP_SIGNALING_LIBRARY_ERROR;
	CHECK(interface->stop(&modem) == 0 && capture.hangups == 2 &&
		      sippymodem_slmodem_driver_last_signaling_result(&driver) ==
			      SIPPYMODEM_SIP_SIGNALING_LIBRARY_ERROR,
	      "SIP hangup failure was hidden or blocked core teardown");
	return true;
}

static bool test_validates_media_and_controls(void)
{
	struct sippymodem_slmodem_driver driver;
	struct sippymodem_host host = { .marker = 3 };
	struct modem modem;
	struct modem_driver *interface;

	CHECK(attach_fixture(&driver, &modem, &host),
	      "media validation fixture failed");
	interface = sippymodem_slmodem_driver_interface(&driver);
	modem.caller = 1;
	modem.srate++;
	CHECK(interface->start(&modem) == -EINVAL && capture.dials == 0,
	      "driver accepted an incompatible sample clock");
	modem.srate = MODEM_RATE;
	modem.format++;
	CHECK(interface->start(&modem) == -EINVAL && capture.dials == 0,
	      "driver accepted an incompatible sample format");
	modem.format = MODEM_FORMAT;
	modem.frag++;
	CHECK(interface->start(&modem) == -EINVAL && capture.dials == 0,
	      "driver accepted an incompatible fragment");
	modem.frag = MODEM_FRAG;

	CHECK(interface->ioctl(&modem, MDMCTL_SPEED, MODEM_RATE) == 0 &&
		      interface->ioctl(&modem, MDMCTL_SPEED, MODEM_RATE + 1) ==
			      -EINVAL &&
		      interface->ioctl(&modem, MDMCTL_SETFMT, MODEM_FORMAT) == 0 &&
		      interface->ioctl(&modem, MDMCTL_SETFMT, MODEM_FORMAT + 1) ==
			      -EINVAL &&
		      interface->ioctl(&modem, MDMCTL_SETFRAG, MODEM_FRAG) == 0 &&
		      interface->ioctl(&modem, MDMCTL_SETFRAG, MODEM_FRAG + 1) ==
			      -EINVAL,
	      "driver control accepted incompatible media parameters");
	CHECK(interface->ioctl(&modem, MDMCTL_GETFMTS, 0) == MODEM_FORMAT &&
		      interface->ioctl(&modem, MDMCTL_CODECTYPE, 0) ==
			      CODEC_UNKNOWN &&
		      interface->ioctl(&modem, MDMCTL_IODELAY, 0) == 0 &&
		      interface->ioctl(&modem, MDMCTL_CAPABILITIES, 0) ==
			      -EOPNOTSUPP &&
		      interface->ioctl(&modem, 0xffff, 0) == -ENOTTY,
	      "driver control diagnostics were inconsistent");
	CHECK(interface->start(NULL) == -EINVAL &&
		      interface->stop(NULL) == -EINVAL &&
		      interface->ioctl(NULL, MDMCTL_SPEED, MODEM_RATE) == -EINVAL,
	      "driver callbacks accepted a missing modem");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "attachment lifetime", test_attachment_lifetime },
		{ "call control", test_routes_call_control },
		{ "media controls", test_validates_media_and_controls },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		printf("[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			printf("[       OK ] %s\n", tests[index].name);
		else {
			printf("[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
