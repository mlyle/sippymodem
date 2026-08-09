// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_sip_signaling.h>

#include "test.h"

#include <string.h>

struct callback_capture {
	struct sippymodem_sip_signaling *signaling;
	unsigned int ready_count;
	unsigned int error_count;
	unsigned int trace_count;
	unsigned int adapter_trace_count;
	int last_error_status;
	bool close_on_ready;
	char last_error_operation[64];
};

static void capture_ready(void *context)
{
	struct callback_capture *capture = context;

	capture->ready_count++;
	if (capture->close_on_ready)
		(void)sippymodem_sip_signaling_close(capture->signaling);
}

static void capture_error(
	void *context, const struct sippymodem_sip_signaling_error *error)
{
	struct callback_capture *capture = context;

	capture->error_count++;
	capture->last_error_status = error->library_status;
	snprintf(capture->last_error_operation,
		 sizeof(capture->last_error_operation), "%s", error->operation);
}

static void capture_trace(
	void *context, const struct sippymodem_sip_trace_record *record)
{
	struct callback_capture *capture = context;

	if (record->data != NULL && record->size != 0)
		capture->trace_count++;
	if (record->origin == SIPPYMODEM_SIP_TRACE_ADAPTER)
		capture->adapter_trace_count++;
}

static struct sippymodem_sip_signaling *create_local_signaling(
	struct callback_capture *capture)
{
	struct sippymodem_sip_signaling_callbacks callbacks = {
		.context = capture,
		.ready = capture_ready,
		.error = capture_error,
		.trace = capture_trace,
	};
	struct sippymodem_config config;

	sippymodem_config_init(&config);
	config.sip.listen_port = 0;
	config.sip.log_level = 3;
	config.sip.registration_enabled = false;
	capture->signaling =
		sippymodem_sip_signaling_create(&config, &callbacks);
	return capture->signaling;
}

static bool test_local_transport_lifecycle(void)
{
	struct callback_capture capture = { 0 };
	struct sippymodem_sip_signaling *signaling =
		create_local_signaling(&capture);
	enum sippymodem_sip_signaling_result result;

	CHECK(signaling != NULL, "could not allocate signaling context");
	CHECK(!sippymodem_sip_signaling_is_open(signaling) &&
		      sippymodem_sip_signaling_local_port(signaling) == 0,
	      "new signaling context is already open");
	result = sippymodem_sip_signaling_open(signaling);
	CHECK(result == SIPPYMODEM_SIP_SIGNALING_OK, "open failed: %s",
	      sippymodem_sip_signaling_result_name(result));
	CHECK(sippymodem_sip_signaling_is_open(signaling) &&
		      sippymodem_sip_signaling_local_port(signaling) != 0,
	      "signaling transport did not bind a local port");
	CHECK(capture.ready_count == 1 && capture.error_count == 0 &&
		      capture.trace_count != 0 && capture.adapter_trace_count != 0,
	      "open callbacks were wrong");
	CHECK(sippymodem_sip_signaling_open(signaling) ==
		      SIPPYMODEM_SIP_SIGNALING_OK &&
		      capture.ready_count == 1,
	      "idempotent open emitted another ready event");
	CHECK(sippymodem_sip_signaling_step(signaling, 0) ==
		      SIPPYMODEM_SIP_SIGNALING_OK,
	      "nonblocking signaling poll failed");
	CHECK(sippymodem_sip_signaling_close(signaling) ==
		      SIPPYMODEM_SIP_SIGNALING_OK &&
		      !sippymodem_sip_signaling_is_open(signaling),
	      "signaling close failed");
	CHECK(sippymodem_sip_signaling_step(signaling, 0) ==
		      SIPPYMODEM_SIP_SIGNALING_NOT_OPEN,
	      "closed signaling context accepted polling");
	sippymodem_sip_signaling_destroy(signaling);
	return true;
}

static bool test_singleton_and_configuration_boundary(void)
{
	struct callback_capture first_capture = { 0 };
	struct callback_capture second_capture = { 0 };
	struct sippymodem_sip_signaling *first =
		create_local_signaling(&first_capture);
	struct sippymodem_sip_signaling *second =
		create_local_signaling(&second_capture);
	struct sippymodem_config registered_config;
	struct sippymodem_sip_signaling *registered;
	enum sippymodem_sip_signaling_result result;

	CHECK(first != NULL && second != NULL, "could not allocate contexts");
	CHECK(sippymodem_sip_signaling_open(first) ==
		      SIPPYMODEM_SIP_SIGNALING_OK,
	      "first signaling context did not open");
	result = sippymodem_sip_signaling_open(second);
	CHECK(result == SIPPYMODEM_SIP_SIGNALING_BUSY &&
		      second_capture.error_count == 1 &&
		      second_capture.last_error_status != 0 &&
		      strstr(second_capture.last_error_operation, "concurrent") != NULL,
	      "concurrent signaling instance was not rejected cleanly");
	second_capture.close_on_ready = true;
	CHECK(sippymodem_sip_signaling_close(first) ==
		      SIPPYMODEM_SIP_SIGNALING_OK &&
		      sippymodem_sip_signaling_open(second) ==
			      SIPPYMODEM_SIP_SIGNALING_OK &&
		      !sippymodem_sip_signaling_is_open(second),
	      "singleton ownership was not released");
	sippymodem_sip_signaling_destroy(first);
	sippymodem_sip_signaling_destroy(second);

	sippymodem_config_init(&registered_config);
	registered = sippymodem_sip_signaling_create(&registered_config, NULL);
	CHECK(registered != NULL, "could not allocate registration boundary test");
	CHECK(sippymodem_sip_signaling_open(registered) ==
		      SIPPYMODEM_SIP_SIGNALING_UNSUPPORTED_CONFIGURATION,
	      "unimplemented account registration was silently accepted");
	sippymodem_sip_signaling_destroy(registered);
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "local transport lifecycle", test_local_transport_lifecycle },
		{ "singleton and configuration boundary",
		  test_singleton_and_configuration_boundary },
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
