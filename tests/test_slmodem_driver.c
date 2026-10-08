// SPDX-License-Identifier: BSD-3-Clause

#include <slmodem_driver.h>

#include "test.h"

#include <errno.h>
#include <string.h>

struct modem_host {
	unsigned int marker;
};

struct host_capture {
	struct modem_host *expected_host;
	unsigned int dials;
	unsigned int answers;
	unsigned int hangups;
	char target[sizeof(((struct modem *)0)->dial_string)];
	bool target_was_null;
	enum sip_signaling_result dial_result;
	enum sip_signaling_result answer_result;
	enum sip_signaling_result hangup_result;
};

static struct host_capture capture;

enum sip_signaling_result modem_host_dial(
	struct modem_host *host, const char *target)
{
	capture.dials++;
	if (host != capture.expected_host)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	capture.target_was_null = target == NULL;
	if (target != NULL)
		snprintf(capture.target, sizeof(capture.target), "%s", target);
	else
		capture.target[0] = '\0';
	return capture.dial_result;
}

enum sip_signaling_result modem_host_answer(
	struct modem_host *host)
{
	capture.answers++;
	return host == capture.expected_host ?
		       capture.answer_result :
		       SIP_SIGNALING_INVALID_ARGUMENT;
}

enum sip_signaling_result modem_host_hangup(
	struct modem_host *host)
{
	capture.hangups++;
	return host == capture.expected_host ?
		       capture.hangup_result :
		       SIP_SIGNALING_INVALID_ARGUMENT;
}

static struct modem configured_modem(
	struct slmodem_driver *driver)
{
	struct modem modem;

	memset(&modem, 0, sizeof(modem));
	modem.driver = *slmodem_driver_interface(driver);
	modem.srate = MODEM_RATE;
	modem.format = MODEM_FORMAT;
	modem.frag = MODEM_FRAG;
	return modem;
}

static bool attach_fixture(
	struct slmodem_driver *driver,
	struct modem *modem,
	struct modem_host *host)
{
	memset(&capture, 0, sizeof(capture));
	capture.expected_host = host;
	if (!slmodem_driver_init(driver))
		return false;
	*modem = configured_modem(driver);
	return slmodem_driver_attach(driver, modem, host);
}

static bool test_attachment_lifetime(void)
{
	struct slmodem_driver driver = { 0 };
	struct slmodem_driver original = driver;
	struct modem_host host = { .marker = 1 };
	struct modem modem;
	struct modem_driver *interface;

	CHECK(!slmodem_driver_init(NULL) &&
		      slmodem_driver_interface(NULL) == NULL &&
		      !slmodem_driver_attach(NULL, NULL, NULL) &&
		      !slmodem_driver_detach(NULL) &&
		      slmodem_driver_last_signaling_result(NULL) ==
			      SIP_SIGNALING_INVALID_ARGUMENT &&
		      driver.modem == original.modem &&
		      driver.host == original.host,
	      "driver accepted incomplete ownership");
	CHECK(slmodem_driver_init(&driver),
	      "driver initialization failed");
	interface = slmodem_driver_interface(&driver);
	CHECK(interface != NULL && interface->start != NULL &&
		      interface->stop != NULL && interface->ioctl != NULL,
	      "driver did not expose a complete modem interface");
	modem = configured_modem(&driver);
	modem.srate++;
	CHECK(!slmodem_driver_attach(&driver, &modem, &host) &&
		      modem.dev_data == NULL && driver.modem == NULL &&
		      driver.host == NULL,
	      "driver partially attached incompatible modem media");
	modem.srate = MODEM_RATE;
	CHECK(slmodem_driver_attach(&driver, &modem, &host) &&
		      modem.dev_data == &driver && driver.modem == &modem &&
		      driver.host == &host,
	      "driver did not attach borrowed owners");
	CHECK(!slmodem_driver_attach(&driver, &modem, &host),
	      "driver attached twice");
	modem.started = 1;
	CHECK(!slmodem_driver_detach(&driver),
	      "driver detached a running modem");
	modem.started = 0;
	CHECK(slmodem_driver_detach(&driver) &&
		      modem.dev_data == NULL && driver.modem == NULL &&
		      driver.host == NULL &&
		      !slmodem_driver_detach(&driver),
	      "driver detach did not release borrowed owners exactly once");
	return true;
}

static bool test_routes_call_control(void)
{
	struct slmodem_driver driver;
	struct modem_host host = { .marker = 2 };
	struct modem modem;
	struct modem_driver *interface;

	CHECK(attach_fixture(&driver, &modem, &host),
	      "call-control fixture failed");
	interface = slmodem_driver_interface(&driver);
	capture.dial_result = SIP_SIGNALING_OK;
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
	capture.answer_result = SIP_SIGNALING_OK;
	CHECK(interface->start(&modem) == 0 && capture.answers == 1,
	      "answering modem start did not answer SIP");
	modem.command = 1;
	CHECK(interface->start(&modem) == -EOPNOTSUPP && capture.answers == 1,
	      "non-call audio start reached SIP call control");

	capture.hangup_result = SIP_SIGNALING_NO_CALL;
	CHECK(interface->stop(&modem) == 0 && capture.hangups == 1,
	      "already-ended remote call made modem stop fail");
	capture.hangup_result = SIP_SIGNALING_LIBRARY_ERROR;
	CHECK(interface->stop(&modem) == 0 && capture.hangups == 2 &&
		      slmodem_driver_last_signaling_result(&driver) ==
			      SIP_SIGNALING_LIBRARY_ERROR,
	      "SIP hangup failure was hidden or blocked core teardown");
	return true;
}

static bool test_deferred_hangup(void)
{
	struct slmodem_driver driver;
	struct modem_host host = { .marker = 4 };
	struct modem modem;
	CHECK(attach_fixture(&driver, &modem, &host), "deferred stop fixture failed");
	struct modem_driver *interface = slmodem_driver_interface(&driver);
	modem.caller = 1;
	driver.defer_hangup = true;
	CHECK(interface->start(&modem) == -EBUSY && capture.dials == 0,
	      "media processing entered SIP start");
	CHECK(interface->stop(&modem) == 0 && driver.hangup_pending && capture.hangups == 0,
	      "media processing entered SIP hangup");
	CHECK(slmodem_driver_flush_hangup(&driver) == SIP_SIGNALING_BUSY && capture.hangups == 0,
	      "hangup flushed before ownership returned");
	driver.defer_hangup = false;
	CHECK(!slmodem_driver_detach(&driver), "pending SIP hangup lost its owner");
	CHECK(interface->start(&modem) == -EBUSY && capture.dials == 0,
	      "new call started with stale pending hangup");
	capture.hangup_result = SIP_SIGNALING_LIBRARY_ERROR;
	CHECK(slmodem_driver_flush_hangup(&driver) == SIP_SIGNALING_LIBRARY_ERROR &&
	      !driver.hangup_pending && capture.hangups == 1,
	      "deferred hangup lost its signaling result");
	CHECK(slmodem_driver_flush_hangup(&driver) == SIP_SIGNALING_OK && capture.hangups == 1,
	      "hangup flushed twice");
	CHECK(slmodem_driver_flush_hangup(NULL) == SIP_SIGNALING_INVALID_ARGUMENT,
	      "missing driver accepted");
	return true;
}

static bool test_validates_media_and_controls(void)
{
	struct slmodem_driver driver;
	struct modem_host host = { .marker = 3 };
	struct modem modem;
	struct modem_driver *interface;

	CHECK(attach_fixture(&driver, &modem, &host),
	      "media validation fixture failed");
	interface = slmodem_driver_interface(&driver);
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
		{ "deferred hangup", test_deferred_hangup },
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
