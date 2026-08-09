// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_sip_signaling.h>

#include "test.h"

#include <arpa/inet.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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

static struct sippymodem_sip_signaling *create_signaling(
	const struct sippymodem_config *config,
	struct callback_capture *capture)
{
	struct sippymodem_sip_signaling_callbacks callbacks = {
		.context = capture,
		.ready = capture_ready,
		.error = capture_error,
		.trace = capture_trace,
	};

	capture->signaling =
		sippymodem_sip_signaling_create(config, &callbacks);
	return capture->signaling;
}

static struct sippymodem_sip_signaling *create_local_signaling(
	struct callback_capture *capture)
{
	struct sippymodem_config config;

	sippymodem_config_init(&config);
	config.sip.listen_port = 0;
	config.sip.log_level = 3;
	config.sip.registration_enabled = false;
	return create_signaling(&config, capture);
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

static bool test_singleton_and_account_boundary(void)
{
	struct callback_capture first_capture = { 0 };
	struct callback_capture second_capture = { 0 };
	struct callback_capture account_capture = { 0 };
	struct sippymodem_sip_signaling *first =
		create_local_signaling(&first_capture);
	struct sippymodem_sip_signaling *second =
		create_local_signaling(&second_capture);
	struct sippymodem_config account_config;
	struct sippymodem_sip_signaling *account;
	enum sippymodem_sip_signaling_result account_close_result =
		SIPPYMODEM_SIP_SIGNALING_NOT_OPEN;
	enum sippymodem_sip_signaling_result account_open_result;
	enum sippymodem_sip_signaling_result result;
	bool identity_normalized = false;

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

	sippymodem_config_init(&account_config);
	account = create_signaling(&account_config, &account_capture);
	CHECK(account != NULL, "could not allocate account boundary test");
	account_open_result = sippymodem_sip_signaling_open(account);
	sippymodem_sip_signaling_destroy(account);
	CHECK(account_open_result == SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT,
	      "registration without credentials was accepted");

	memset(&account_capture, 0, sizeof(account_capture));
	sippymodem_config_init(&account_config);
	account_config.sip.listen_port = 0;
	account_config.sip.registration_enabled = false;
	(void)snprintf(account_config.sip.login,
		       sizeof(account_config.sip.login),
		       "bob:p@ss:word@example.net");
	account = create_signaling(&account_config, &account_capture);
	CHECK(account != NULL, "could not allocate combined-login account");
	account_open_result = sippymodem_sip_signaling_open(account);
	if (account_open_result == SIPPYMODEM_SIP_SIGNALING_OK) {
		const char *identity =
			sippymodem_sip_signaling_identity_uri(account);

		identity_normalized = identity != NULL &&
			strcmp(identity, "sip:bob@example.net") == 0;
		account_close_result =
			sippymodem_sip_signaling_close(account);
	}
	sippymodem_sip_signaling_destroy(account);
	CHECK(account_open_result == SIPPYMODEM_SIP_SIGNALING_OK,
	      "combined-login account did not open: %s",
	      sippymodem_sip_signaling_result_name(account_open_result));
	CHECK(identity_normalized,
	      "combined login did not produce the normalized identity");
	CHECK(account_close_result == SIPPYMODEM_SIP_SIGNALING_OK,
	      "combined-login account did not close");
	return true;
}

static int open_loopback_listener(unsigned int *port)
{
	struct sockaddr_in address = {
		.sin_family = AF_INET,
		.sin_port = 0,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	socklen_t address_size = sizeof(address);
	int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);

	if (socket_fd < 0 ||
	    bind(socket_fd, (const struct sockaddr *)&address,
		 sizeof(address)) != 0 ||
	    getsockname(socket_fd, (struct sockaddr *)&address,
			&address_size) != 0) {
		if (socket_fd >= 0)
			(void)close(socket_fd);
		return -1;
	}
	*port = ntohs(address.sin_port);
	return socket_fd;
}

static bool test_loopback_registration_request(void)
{
	struct callback_capture capture = { 0 };
	struct sippymodem_config config;
	struct sippymodem_sip_signaling *signaling = NULL;
	struct pollfd descriptor;
	enum sippymodem_sip_signaling_result open_result =
		SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;
	enum sippymodem_sip_signaling_result step_result =
		SIPPYMODEM_SIP_SIGNALING_NOT_OPEN;
	enum sippymodem_sip_signaling_result close_result =
		SIPPYMODEM_SIP_SIGNALING_NOT_OPEN;
	char packet[2048] = { 0 };
	unsigned int registrar_port = 0;
	ssize_t packet_size = -1;
	bool identity_normalized = false;
	int listener = open_loopback_listener(&registrar_port);
	int poll_result = -1;

	CHECK(listener >= 0, "could not open loopback SIP registrar");
	sippymodem_config_init(&config);
	config.sip.listen_port = 0;
	config.sip.log_level = 3;
	config.sip.registration_enabled = true;
	(void)snprintf(config.sip.user, sizeof(config.sip.user), "alice");
	(void)snprintf(config.sip.password, sizeof(config.sip.password), "secret");
	(void)snprintf(config.sip.domain, sizeof(config.sip.domain),
		       "127.0.0.1");
	(void)snprintf(config.sip.public_address,
		       sizeof(config.sip.public_address), "127.0.0.1");
	(void)snprintf(config.sip.registrar_uri,
		       sizeof(config.sip.registrar_uri), "sip:127.0.0.1:%u",
		       registrar_port);
	signaling = create_signaling(&config, &capture);
	if (signaling != NULL) {
		open_result = sippymodem_sip_signaling_open(signaling);
		if (open_result == SIPPYMODEM_SIP_SIGNALING_OK) {
			const char *identity =
				sippymodem_sip_signaling_identity_uri(signaling);

			identity_normalized = identity != NULL &&
				strcmp(identity, "sip:alice@127.0.0.1") == 0;
			step_result = sippymodem_sip_signaling_step(signaling, 0);
			descriptor.fd = listener;
			descriptor.events = POLLIN;
			descriptor.revents = 0;
			poll_result = poll(&descriptor, 1, 1000);
			if (poll_result == 1 && (descriptor.revents & POLLIN) != 0)
				packet_size = recv(listener, packet,
						   sizeof(packet) - 1, 0);
			close_result =
				sippymodem_sip_signaling_close(signaling);
		}
		sippymodem_sip_signaling_destroy(signaling);
	}
	(void)close(listener);

	CHECK(signaling != NULL, "could not allocate registered account");
	CHECK(open_result == SIPPYMODEM_SIP_SIGNALING_OK,
	      "registered account did not open: %s",
	      sippymodem_sip_signaling_result_name(open_result));
	CHECK(identity_normalized, "registered identity was not normalized");
	CHECK(step_result == SIPPYMODEM_SIP_SIGNALING_OK,
	      "registration event polling failed");
	CHECK(packet_size > 0, "registrar did not receive a SIP request");
	CHECK(strstr(packet, "REGISTER sip:127.0.0.1:") != NULL &&
		      strstr(packet, ";ob") != NULL,
	      "registration request or forced contact was wrong");
	CHECK(close_result == SIPPYMODEM_SIP_SIGNALING_OK,
	      "registered account did not close");
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
		{ "singleton and account boundary",
		  test_singleton_and_account_boundary },
		{ "loopback registration request",
		  test_loopback_registration_request },
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
