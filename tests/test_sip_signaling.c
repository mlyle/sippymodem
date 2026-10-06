// SPDX-License-Identifier: BSD-3-Clause

#include <sip_signaling.h>

#include <pjsua-lib/pjsua.h>

#include "test.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <time.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
	TEST_CALL_EVENTS = 8,
	TEST_MEDIA_EVENTS = 4,
	TEST_REGISTRATION_EVENTS = 8,
	TEST_SIP_MESSAGE_MAX = 2048,
	TEST_MODEM_RTP_PORT = 41000,
	TEST_REMOTE_RTP_PORT = 40000,
};

static pj_status_t next_ioqueue_error;

pj_status_t __real_pj_ioqueue_register_sock2(
	pj_pool_t *, pj_ioqueue_t *, pj_sock_t, pj_grp_lock_t *, void *,
	const pj_ioqueue_callback *, pj_ioqueue_key_t **);

pj_status_t __wrap_pj_ioqueue_register_sock2(
	pj_pool_t *pool, pj_ioqueue_t *queue, pj_sock_t socket,
	pj_grp_lock_t *lock, void *context, const pj_ioqueue_callback *callbacks,
	pj_ioqueue_key_t **key)
{
	if (next_ioqueue_error != PJ_SUCCESS) {
		pj_status_t error = next_ioqueue_error;
		next_ioqueue_error = PJ_SUCCESS;
		return error;
	}
	return __real_pj_ioqueue_register_sock2(
		pool, queue, socket, lock, context, callbacks, key);
}

struct callback_capture {
	bool deny_incoming;
	struct sip_signaling *signaling;
	unsigned int ready_count;
	unsigned int error_count;
	unsigned int trace_count;
	unsigned int adapter_trace_count;
	unsigned int call_count;
	unsigned int media_count;
	unsigned int registration_count;
	int last_error_status;
	bool close_on_ready;
	char last_error_operation[64];
	enum sip_call_direction
		call_directions[TEST_CALL_EVENTS];
	enum sip_call_state call_states[TEST_CALL_EVENTS];
	unsigned int call_status_codes[TEST_CALL_EVENTS];
	uint64_t call_generations[TEST_CALL_EVENTS];
	unsigned int endpoint_ids[TEST_CALL_EVENTS];
	char called_numbers[TEST_CALL_EVENTS][128];
	char caller_numbers[TEST_CALL_EVENTS][128];
	char call_reasons[TEST_CALL_EVENTS][32];
	char call_remote_identities[TEST_CALL_EVENTS][128];
	enum sip_media_state media_states[TEST_MEDIA_EVENTS];
	enum sip_media_address_family
		media_families[TEST_MEDIA_EVENTS];
	enum media_codec media_codecs[TEST_MEDIA_EVENTS];
	unsigned int media_payload_types[TEST_MEDIA_EVENTS];
	unsigned int media_sample_rates[TEST_MEDIA_EVENTS];
	unsigned int media_channels[TEST_MEDIA_EVENTS];
	unsigned int media_local_ports[TEST_MEDIA_EVENTS];
	unsigned int media_remote_ports[TEST_MEDIA_EVENTS];
	char media_remote_addresses[TEST_MEDIA_EVENTS][64];
	enum sip_registration_state
		registration_states[TEST_REGISTRATION_EVENTS];
	unsigned int registration_status_codes[TEST_REGISTRATION_EVENTS];
	int registration_library_statuses[TEST_REGISTRATION_EVENTS];
	unsigned int registration_expirations[TEST_REGISTRATION_EVENTS];
	char registration_reasons[TEST_REGISTRATION_EVENTS][32];
};

static bool capture_admit(void *context)
{
	return !((struct callback_capture *)context)->deny_incoming;
}

static void capture_ready(void *context)
{
	struct callback_capture *capture = context;

	capture->ready_count++;
	if (capture->close_on_ready)
		(void)sip_signaling_close(capture->signaling);
}

static void capture_error(
	void *context, const struct sip_signaling_error *error)
{
	struct callback_capture *capture = context;

	capture->error_count++;
	capture->last_error_status = error->library_status;
	snprintf(capture->last_error_operation,
		 sizeof(capture->last_error_operation), "%s", error->operation);
}

static void capture_trace(
	void *context, const struct sip_trace_record *record)
{
	struct callback_capture *capture = context;

	if (record->data != NULL && record->size != 0)
		capture->trace_count++;
	if (record->origin == SIP_TRACE_ADAPTER)
		capture->adapter_trace_count++;
}

static void capture_registration(
	void *context,
	const struct sip_registration_event *event)
{
	struct callback_capture *capture = context;
	unsigned int index = capture->registration_count++;

	if (index >= TEST_REGISTRATION_EVENTS)
		return;
	capture->registration_states[index] = event->state;
	capture->registration_status_codes[index] = event->status_code;
	capture->registration_library_statuses[index] = event->library_status;
	capture->registration_expirations[index] = event->expires_seconds;
	(void)snprintf(capture->registration_reasons[index],
		       sizeof(capture->registration_reasons[index]), "%s",
		       event->reason != NULL ? event->reason : "");
}

static void capture_call(
	void *context, const struct sip_call_event *event)
{
	struct callback_capture *capture = context;
	unsigned int index = capture->call_count++;

	if (index >= TEST_CALL_EVENTS)
		return;
	capture->call_directions[index] = event->direction;
	capture->call_states[index] = event->state;
	capture->call_status_codes[index] = event->status_code;
	capture->call_generations[index] = event->generation;
	capture->endpoint_ids[index] = event->endpoint_id;
	(void)snprintf(capture->called_numbers[index], sizeof(capture->called_numbers[index]), "%s", event->called_number != NULL ? event->called_number : "");
	(void)snprintf(capture->caller_numbers[index], sizeof(capture->caller_numbers[index]), "%s", event->caller_number != NULL ? event->caller_number : "");
	(void)snprintf(capture->call_reasons[index],
		       sizeof(capture->call_reasons[index]), "%s",
		       event->reason != NULL ? event->reason : "");
	(void)snprintf(capture->call_remote_identities[index],
		       sizeof(capture->call_remote_identities[index]), "%s",
		       event->remote_identity != NULL ? event->remote_identity : "");
}

static void capture_media(
	void *context, const struct sip_media_event *event)
{
	struct callback_capture *capture = context;
	unsigned int index = capture->media_count++;

	if (index >= TEST_MEDIA_EVENTS)
		return;
	capture->media_states[index] = event->state;
	capture->media_families[index] = event->remote_address_family;
	capture->media_codecs[index] = event->codec;
	capture->media_payload_types[index] = event->payload_type;
	capture->media_sample_rates[index] = event->sample_rate;
	capture->media_channels[index] = event->channels;
	capture->media_local_ports[index] = event->local_port;
	capture->media_remote_ports[index] = event->remote_port;
	(void)snprintf(capture->media_remote_addresses[index],
		       sizeof(capture->media_remote_addresses[index]), "%s",
		       event->remote_address != NULL ? event->remote_address : "");
}

static struct sip_signaling *create_signaling(
	const struct app_config *config,
	struct callback_capture *capture)
{
	struct sip_signaling_callbacks callbacks = {
		.context = capture,
		.ready = capture_ready,
		.error = capture_error,
		.trace = capture_trace,
		.registration_changed = capture_registration,
		.call_changed = capture_call,
		.media_changed = capture_media,
	};

	capture->signaling =
		sip_signaling_create(config, &callbacks);
	return capture->signaling;
}

static struct sip_signaling *create_local_signaling(
	struct callback_capture *capture)
{
	struct app_config config;

	config_init(&config);
	config.sip.listen_port = 0;
	config.sip.log_level = 3;
	config.sip.registration_enabled = false;
	return create_signaling(&config, capture);
}

static bool library_media_transport_is_ephemeral(void)
{
	pjsua_acc_id account_ids[2];
	unsigned int account_count = ARRAY_LENGTH(account_ids);
	pjsua_acc_config account;
	pj_pool_t *pool;
	bool ephemeral = false;

	if (pjsua_enum_accs(account_ids, &account_count) != PJ_SUCCESS ||
	    account_count != 1)
		return false;
	pool = pjsua_pool_create("test-account", 512, 512);
	if (pool == NULL)
		return false;
	if (pjsua_acc_get_config(account_ids[0], pool, &account) == PJ_SUCCESS)
		ephemeral = account.rtp_cfg.port == 0 &&
			    account.rtp_cfg.port_range == 0;
	pj_pool_release(pool);
	return ephemeral;
}

static bool test_local_transport_lifecycle(void)
{
	pj_log_func *previous_log_writer = pj_log_get_log_func();
	int previous_log_level = pj_log_get_level();
	struct callback_capture capture = { 0 };
	struct sip_signaling *signaling =
		create_local_signaling(&capture);
	enum sip_signaling_result result;

	CHECK(signaling != NULL, "could not allocate signaling context");
	CHECK(!sip_signaling_is_open(signaling) &&
		      sip_signaling_local_port(signaling) == 0 &&
		      sip_signaling_call_state(signaling) ==
			      SIP_CALL_IDLE &&
		      sip_signaling_media_state(signaling) ==
			      SIP_MEDIA_INACTIVE &&
		      sip_signaling_registration_state(signaling) ==
			      SIP_REGISTRATION_DISABLED,
	      "new signaling context is already open");
	result = sip_signaling_open(signaling);
	CHECK(result == SIP_SIGNALING_OK, "open failed: %s",
	      sip_signaling_result_name(result));
	CHECK(pj_log_get_log_func() == previous_log_writer && pj_log_get_level() == previous_log_level,
	      "open changed the embedding logger");
	CHECK(sip_signaling_is_open(signaling) &&
		      sip_signaling_local_port(signaling) != 0,
	      "signaling transport did not bind a local port");
	CHECK(library_media_transport_is_ephemeral(),
	      "unused library media transport retained a fixed port");
	CHECK(capture.ready_count == 1 && capture.error_count == 0 &&
		      capture.trace_count > capture.adapter_trace_count &&
		      capture.adapter_trace_count != 0,
	      "open callbacks were wrong");
	CHECK(sip_signaling_open(signaling) ==
		      SIP_SIGNALING_OK &&
		      capture.ready_count == 1,
	      "idempotent open emitted another ready event");
	CHECK(sip_signaling_step(signaling, 0) ==
		      SIP_SIGNALING_OK,
	      "nonblocking signaling poll failed");
	CHECK(pj_log_get_log_func() == previous_log_writer && pj_log_get_level() == previous_log_level,
	      "poll changed the embedding logger");
	CHECK(sip_signaling_close(signaling) ==
		      SIP_SIGNALING_OK &&
		      !sip_signaling_is_open(signaling),
	      "signaling close failed");
	CHECK(pj_log_get_log_func() == previous_log_writer &&
	      pj_log_get_level() == previous_log_level,
	      "signaling did not restore the process log settings");
	CHECK(sip_signaling_step(signaling, 0) ==
		      SIP_SIGNALING_NOT_OPEN,
	      "closed signaling context accepted polling");
	sip_signaling_destroy(signaling);
	return true;
}

static bool test_singleton_and_account_boundary(void)
{
	struct callback_capture first_capture = { 0 };
	struct callback_capture second_capture = { 0 };
	struct callback_capture account_capture = { 0 };
	struct sip_signaling *first =
		create_local_signaling(&first_capture);
	struct sip_signaling *second =
		create_local_signaling(&second_capture);
	struct app_config account_config;
	struct sip_signaling *account;
	enum sip_signaling_result account_close_result =
		SIP_SIGNALING_NOT_OPEN;
	enum sip_signaling_result account_open_result;
	enum sip_signaling_result result;
	bool identity_normalized = false;

	CHECK(first != NULL && second != NULL, "could not allocate contexts");
	CHECK(sip_signaling_open(first) ==
		      SIP_SIGNALING_OK,
	      "first signaling context did not open");
	result = sip_signaling_open(second);
	CHECK(result == SIP_SIGNALING_BUSY &&
		      second_capture.error_count == 1 &&
		      second_capture.last_error_status != 0 &&
		      strstr(second_capture.last_error_operation, "concurrent") != NULL,
	      "concurrent signaling instance was not rejected cleanly");
	second_capture.close_on_ready = true;
	CHECK(sip_signaling_close(first) ==
		      SIP_SIGNALING_OK &&
		      sip_signaling_open(second) ==
			      SIP_SIGNALING_OK &&
		      !sip_signaling_is_open(second),
	      "singleton ownership was not released");
	sip_signaling_destroy(first);
	sip_signaling_destroy(second);

	config_init(&account_config);
	account = create_signaling(&account_config, &account_capture);
	CHECK(account != NULL, "could not allocate account boundary test");
	account_open_result = sip_signaling_open(account);
	sip_signaling_destroy(account);
	CHECK(account_open_result == SIP_SIGNALING_INVALID_ARGUMENT,
	      "registration without credentials was accepted");

	memset(&account_capture, 0, sizeof(account_capture));
	config_init(&account_config);
	account_config.sip.listen_port = 0;
	account_config.sip.registration_enabled = false;
	(void)snprintf(account_config.sip.login,
		       sizeof(account_config.sip.login),
		       "bob:p@ss:word@example.net");
	account = create_signaling(&account_config, &account_capture);
	CHECK(account != NULL, "could not allocate combined-login account");
	account_open_result = sip_signaling_open(account);
	if (account_open_result == SIP_SIGNALING_OK) {
		const char *identity =
			sip_signaling_identity_uri(account);

		identity_normalized = identity != NULL &&
			strcmp(identity, "sip:bob@example.net") == 0;
		account_close_result =
			sip_signaling_close(account);
	}
	sip_signaling_destroy(account);
	CHECK(account_open_result == SIP_SIGNALING_OK,
	      "combined-login account did not open: %s",
	      sip_signaling_result_name(account_open_result));
	CHECK(identity_normalized,
	      "combined login did not produce the normalized identity");
	CHECK(account_close_result == SIP_SIGNALING_OK,
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

struct loopback_registrar {
	pthread_t worker;
	int socket_fd;
	unsigned int port;
	unsigned int request_count;
	bool registration_received;
	bool unregistration_received;
	bool failed;
};

static const char *find_header(const char *request, const char *name)
{
	const char *line = request;
	size_t name_size = strlen(name);

	while (line[0] != '\0') {
		const char *line_end = strstr(line, "\r\n");

		if (line_end == NULL)
			return NULL;
		if ((size_t)(line_end - line) >= name_size &&
		    memcmp(line, name, name_size) == 0)
			return line;
		line = line_end + 2;
	}
	return NULL;
}

static bool append_bytes(char *output, size_t capacity, size_t *size,
			 const char *data, size_t data_size)
{
	if (*size > capacity || data_size > capacity - *size)
		return false;
	memcpy(output + *size, data, data_size);
	*size += data_size;
	return true;
}

static bool append_response_header(char *response, size_t capacity,
				   size_t *response_size,
				   const char *request, const char *name,
				   const char *suffix)
{
	const char *line = find_header(request, name);
	const char *line_end = line != NULL ? strstr(line, "\r\n") : NULL;

	return line_end != NULL &&
	       append_bytes(response, capacity, response_size, line,
			    (size_t)(line_end - line)) &&
	       append_bytes(response, capacity, response_size, suffix,
			    strlen(suffix)) &&
	       append_bytes(response, capacity, response_size, "\r\n", 2);
}

static bool build_response_head(
	char *response, size_t capacity, size_t *response_size,
	const char *request, unsigned int status_code, const char *reason)
{
	const char *to_header = find_header(request, "To:");
	const char *to_end =
		to_header != NULL ? strstr(to_header, "\r\n") : NULL;
	const char *to_tag =
		to_header != NULL ? strstr(to_header, ";tag=") : NULL;
	const char *to_suffix =
		to_tag != NULL && to_end != NULL && to_tag < to_end
			? ""
			: ";tag=loopback";
	int status_line_size;

	status_line_size = snprintf(response, capacity, "SIP/2.0 %u %s\r\n",
				    status_code, reason);
	if (status_line_size < 0 || (size_t)status_line_size >= capacity)
		return false;
	*response_size = (size_t)status_line_size;
	return append_response_header(response, capacity, response_size, request,
				      "Via:", "") &&
	       append_response_header(response, capacity, response_size, request,
				      "From:", "") &&
	       append_response_header(response, capacity, response_size, request,
				      "To:", to_suffix) &&
	       append_response_header(response, capacity, response_size, request,
				      "Call-ID:", "") &&
	       append_response_header(response, capacity, response_size, request,
				      "CSeq:", "");
}

static bool build_registration_response(
	char *response, size_t capacity, size_t *response_size,
	const char *request, bool unregistration)
{
	int tail_size;

	if (!build_response_head(response, capacity, response_size, request,
				 200, "OK"))
		return false;
	if (!unregistration &&
	    !append_response_header(response, capacity, response_size, request,
				    "Contact:", ""))
		return false;
	tail_size = snprintf(response + *response_size,
			     capacity - *response_size,
			     "Expires: %u\r\nContent-Length: 0\r\n\r\n",
			     unregistration ? 0u : 60u);
	if (tail_size < 0 || (size_t)tail_size >= capacity - *response_size)
		return false;
	*response_size += (size_t)tail_size;
	return true;
}

static bool build_empty_response(
	char *response, size_t capacity, size_t *response_size,
	const char *request, unsigned int status_code, const char *reason)
{
	return build_response_head(response, capacity, response_size, request,
				   status_code, reason) &&
	       append_bytes(response, capacity, response_size,
			    "Content-Length: 0\r\n\r\n",
			    sizeof("Content-Length: 0\r\n\r\n") - 1);
}

static void *serve_loopback_registrar(void *context)
{
	struct loopback_registrar *registrar = context;

	while (registrar->request_count < 2) {
		struct sockaddr_in remote;
		struct pollfd descriptor = {
			.fd = registrar->socket_fd,
			.events = POLLIN,
		};
		char request[TEST_SIP_MESSAGE_MAX];
		char response[TEST_SIP_MESSAGE_MAX];
		socklen_t remote_size = sizeof(remote);
		size_t response_size;
		ssize_t request_size;
		bool unregistration;

		if (poll(&descriptor, 1, 3000) != 1 ||
		    (descriptor.revents & POLLIN) == 0) {
			registrar->failed = true;
			break;
		}
		request_size = recvfrom(
			registrar->socket_fd, request, sizeof(request) - 1, 0,
			(struct sockaddr *)&remote, &remote_size);
		if (request_size <= 0) {
			registrar->failed = true;
			break;
		}
		request[request_size] = '\0';
		unregistration =
			strstr(request, "\r\nExpires: 0\r\n") != NULL ||
			strstr(request, ";expires=0") != NULL;
		if (strncmp(request, "REGISTER ", sizeof("REGISTER ") - 1) != 0 ||
		    !build_registration_response(response, sizeof(response),
						 &response_size, request,
						 unregistration) ||
		    sendto(registrar->socket_fd, response, response_size, 0,
			   (const struct sockaddr *)&remote, remote_size) !=
			    (ssize_t)response_size) {
			registrar->failed = true;
			break;
		}
		registrar->request_count++;
		if (unregistration)
			registrar->unregistration_received = true;
		else
			registrar->registration_received = true;
	}
	return NULL;
}

static bool start_loopback_registrar(struct loopback_registrar *registrar)
{
	memset(registrar, 0, sizeof(*registrar));
	registrar->socket_fd = open_loopback_listener(&registrar->port);
	if (registrar->socket_fd < 0)
		return false;
	if (pthread_create(&registrar->worker, NULL, serve_loopback_registrar,
			   registrar) != 0) {
		(void)close(registrar->socket_fd);
		registrar->socket_fd = -1;
		return false;
	}
	return true;
}

static bool finish_loopback_registrar(struct loopback_registrar *registrar)
{
	bool succeeded = pthread_join(registrar->worker, NULL) == 0 &&
			 !registrar->failed;

	(void)close(registrar->socket_fd);
	registrar->socket_fd = -1;
	return succeeded;
}

struct loopback_call_peer {
	pthread_t worker;
	int socket_fd;
	unsigned int port;
	bool invite_received;
	bool target_received;
	bool modem_sdp_received;
	bool cancel_received;
	bool failed;
};

static bool receive_datagram(int socket_fd, char *message, size_t capacity,
			     struct sockaddr_in *remote,
			     socklen_t *remote_size)
{
	struct pollfd descriptor = {
		.fd = socket_fd,
		.events = POLLIN,
	};
	ssize_t message_size;

	if (capacity == 0 || poll(&descriptor, 1, 3000) != 1 ||
	    (descriptor.revents & POLLIN) == 0)
		return false;
	message_size = recvfrom(socket_fd, message, capacity - 1, 0,
				(struct sockaddr *)remote, remote_size);
	if (message_size <= 0)
		return false;
	message[message_size] = '\0';
	return true;
}

static bool send_empty_response(
	int socket_fd, const struct sockaddr_in *remote, socklen_t remote_size,
	const char *request, unsigned int status_code, const char *reason)
{
	char response[TEST_SIP_MESSAGE_MAX];
	size_t response_size;

	return build_empty_response(response, sizeof(response), &response_size,
				    request, status_code, reason) &&
	       sendto(socket_fd, response, response_size, 0,
		      (const struct sockaddr *)remote, remote_size) ==
		       (ssize_t)response_size;
}

static bool sdp_has_unsigned_line(const char *sdp, const char *prefix,
				  unsigned int value, const char *suffix)
{
	char line[96];
	int size = snprintf(line, sizeof(line), "%s%u%s\r\n", prefix, value,
			    suffix);

	return size > 0 && (size_t)size < sizeof(line) &&
	       strstr(sdp, line) != NULL;
}

static bool has_modem_audio_sdp(const char *message, unsigned int rtp_port,
				unsigned int frame_duration_ms)
{
	const char *sdp = strstr(message, "\r\n\r\n");

	if (sdp == NULL)
		return false;
	sdp += 4;
	return strstr(sdp, "c=IN IP4 127.0.0.1\r\n") != NULL &&
	       sdp_has_unsigned_line(sdp, "m=audio ", rtp_port,
				     " RTP/AVP 0") &&
	       strstr(sdp, "a=rtpmap:0 PCMU/8000\r\n") != NULL &&
	       sdp_has_unsigned_line(sdp, "a=ptime:", frame_duration_ms, "") &&
	       sdp_has_unsigned_line(sdp, "a=maxptime:", frame_duration_ms,
				     "") &&
	       sdp_has_unsigned_line(sdp, "a=rtcp:", rtp_port, "") &&
	       strstr(sdp, "a=silenceSupp:off - - - -\r\n") != NULL &&
	       strstr(sdp, "a=gpmd:0 vbd=yes\r\n") != NULL;
}

static void *serve_loopback_call(void *context)
{
	struct loopback_call_peer *peer = context;
	struct sockaddr_in invite_remote;
	struct sockaddr_in cancel_remote;
	socklen_t invite_remote_size = sizeof(invite_remote);
	socklen_t cancel_remote_size = sizeof(cancel_remote);
	char invite[TEST_SIP_MESSAGE_MAX];
	char cancel[TEST_SIP_MESSAGE_MAX];

	if (!receive_datagram(peer->socket_fd, invite, sizeof(invite),
			     &invite_remote, &invite_remote_size) ||
	    strncmp(invite, "INVITE ", sizeof("INVITE ") - 1) != 0 ||
	    !send_empty_response(peer->socket_fd, &invite_remote,
				 invite_remote_size, invite, 180, "Ringing")) {
		peer->failed = true;
		return NULL;
	}
	peer->invite_received = true;
	peer->target_received =
		strstr(invite, "INVITE sip:bob@127.0.0.1:") == invite;
	peer->modem_sdp_received = has_modem_audio_sdp(
		invite, TEST_MODEM_RTP_PORT, 20);
	if (!receive_datagram(peer->socket_fd, cancel, sizeof(cancel),
			     &cancel_remote, &cancel_remote_size) ||
	    strncmp(cancel, "CANCEL ", sizeof("CANCEL ") - 1) != 0 ||
	    !send_empty_response(peer->socket_fd, &cancel_remote,
				 cancel_remote_size, cancel, 200, "OK") ||
	    !send_empty_response(peer->socket_fd, &invite_remote,
				 invite_remote_size, invite, 487,
				 "Request Terminated")) {
		peer->failed = true;
		return NULL;
	}
	peer->cancel_received = true;
	return NULL;
}

static bool start_loopback_call(struct loopback_call_peer *peer)
{
	memset(peer, 0, sizeof(*peer));
	peer->socket_fd = open_loopback_listener(&peer->port);
	if (peer->socket_fd < 0)
		return false;
	if (pthread_create(&peer->worker, NULL, serve_loopback_call, peer) != 0) {
		(void)close(peer->socket_fd);
		peer->socket_fd = -1;
		return false;
	}
	return true;
}

static bool finish_loopback_call(struct loopback_call_peer *peer)
{
	bool succeeded = pthread_join(peer->worker, NULL) == 0 && !peer->failed;

	(void)close(peer->socket_fd);
	peer->socket_fd = -1;
	return succeeded;
}

struct loopback_incoming_peer {
	pthread_t worker;
	int socket_fd;
	unsigned int port;
	unsigned int destination_port;
	const char *caller;
	const char *called;
	const char *privacy;
	bool invite_sent;
	bool ringing_received;
	bool answer_received;
	bool modem_sdp_received;
	bool ack_sent;
	bool failed;
};

static bool copy_header_line(char *output, size_t capacity,
			     const char *message, const char *name)
{
	const char *line = find_header(message, name);
	const char *line_end = line != NULL ? strstr(line, "\r\n") : NULL;
	size_t size;

	if (line_end == NULL)
		return false;
	size = (size_t)(line_end - line);
	if (size >= capacity)
		return false;
	memcpy(output, line, size);
	output[size] = '\0';
	return true;
}

static void *serve_incoming_call(void *context)
{
	static const char offer[] =
		"v=0\r\n"
		"o=- 1 1 IN IP4 127.0.0.1\r\n"
		"s=-\r\n"
		"c=IN IP4 127.0.0.1\r\n"
		"t=0 0\r\n"
		"m=audio 40000 RTP/AVP 0\r\n"
		"a=rtpmap:0 PCMU/8000\r\n";
	struct loopback_incoming_peer *peer = context;
	struct sockaddr_in destination = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	struct sockaddr_in response_remote;
	char invite[TEST_SIP_MESSAGE_MAX];
	char response[TEST_SIP_MESSAGE_MAX];
	char to_header[256];
	char ack[1024];
	int invite_size;
	int ack_size;

	destination.sin_port = htons((uint16_t)peer->destination_port);
	invite_size = snprintf(
		invite, sizeof(invite),
		"INVITE sip:%s@127.0.0.1:%u SIP/2.0\r\n"
		"Via: SIP/2.0/UDP 127.0.0.1:%u;branch=z9hG4bK-inbound-%u\r\n"
		"Max-Forwards: 70\r\n"
		"From: <sip:%s@127.0.0.1>;tag=caller\r\n"
		"To: <sip:modem@127.0.0.1:%u>\r\n"
		"Call-ID: incoming-loopback-%u\r\n"
		"CSeq: 1 INVITE\r\n"
		"%s"
		"Contact: <sip:caller@127.0.0.1:%u>\r\n"
		"Content-Type: application/sdp\r\n"
		"Content-Length: %zu\r\n\r\n%s",
		peer->called, peer->destination_port, peer->port, peer->port, peer->caller,
		peer->destination_port, peer->port, peer->privacy, peer->port, sizeof(offer) - 1, offer);
	if (invite_size < 0 || (size_t)invite_size >= sizeof(invite) ||
	    sendto(peer->socket_fd, invite, (size_t)invite_size, 0,
		   (const struct sockaddr *)&destination, sizeof(destination)) !=
		    invite_size) {
		peer->failed = true;
		return NULL;
	}
	peer->invite_sent = true;
	for (unsigned int attempt = 0; attempt < 4 && !peer->answer_received;
	     ++attempt) {
		socklen_t response_remote_size = sizeof(response_remote);

		if (!receive_datagram(peer->socket_fd, response, sizeof(response),
				      &response_remote,
				      &response_remote_size)) {
			peer->failed = true;
			return NULL;
		}
		if (strncmp(response, "SIP/2.0 100 ",
			    sizeof("SIP/2.0 100 ") - 1) == 0)
			continue;
		if (strncmp(response, "SIP/2.0 180 ",
			    sizeof("SIP/2.0 180 ") - 1) == 0) {
			peer->ringing_received = true;
			continue;
		}
		if (strncmp(response, "SIP/2.0 200 ",
			    sizeof("SIP/2.0 200 ") - 1) != 0 ||
		    !copy_header_line(to_header, sizeof(to_header), response,
				      "To:")) {
			peer->failed = true;
			return NULL;
		}
		peer->answer_received = true;
		peer->modem_sdp_received = has_modem_audio_sdp(
			response, TEST_MODEM_RTP_PORT, 10);
	}
	if (!peer->answer_received) {
		peer->failed = true;
		return NULL;
	}
	ack_size = snprintf(
		ack, sizeof(ack),
		"ACK sip:modem@127.0.0.1:%u SIP/2.0\r\n"
		"Via: SIP/2.0/UDP 127.0.0.1:%u;branch=z9hG4bK-ack\r\n"
		"Max-Forwards: 70\r\n"
		"From: <sip:%s@127.0.0.1>;tag=caller\r\n"
		"%s\r\n"
		"Call-ID: incoming-loopback-%u\r\n"
		"CSeq: 1 ACK\r\n"
		"Content-Length: 0\r\n\r\n",
		peer->destination_port, peer->port, peer->caller, to_header, peer->port);
	if (ack_size < 0 || (size_t)ack_size >= sizeof(ack) ||
	    sendto(peer->socket_fd, ack, (size_t)ack_size, 0,
		   (const struct sockaddr *)&destination, sizeof(destination)) !=
		    ack_size) {
		peer->failed = true;
		return NULL;
	}
	peer->ack_sent = true;
	return NULL;
}

static bool start_incoming_identity(struct loopback_incoming_peer *peer,
	unsigned int destination_port, const char *caller, const char *called, const char *privacy)
{
	memset(peer, 0, sizeof(*peer));
	peer->destination_port = destination_port;
	peer->caller = caller;
	peer->called = called;
	peer->privacy = privacy;
	peer->socket_fd = open_loopback_listener(&peer->port);
	if (peer->socket_fd < 0)
		return false;
	if (pthread_create(&peer->worker, NULL, serve_incoming_call, peer) != 0) {
		(void)close(peer->socket_fd);
		peer->socket_fd = -1;
		return false;
	}
	return true;
}

static bool start_incoming_call(struct loopback_incoming_peer *peer, unsigned int destination_port)
{
	return start_incoming_identity(peer, destination_port, "caller", "modem", "");
}

static bool finish_incoming_call(struct loopback_incoming_peer *peer)
{
	bool succeeded = pthread_join(peer->worker, NULL) == 0 && !peer->failed;

	(void)close(peer->socket_fd);
	peer->socket_fd = -1;
	return succeeded;
}

static bool test_loopback_registration_lifecycle(void)
{
	struct callback_capture capture = { 0 };
	struct loopback_registrar registrar;
	struct app_config config;
	struct sip_signaling *signaling = NULL;
	enum sip_signaling_result open_result =
		SIP_SIGNALING_INVALID_ARGUMENT;
	enum sip_signaling_result step_result =
		SIP_SIGNALING_NOT_OPEN;
	enum sip_signaling_result close_result =
		SIP_SIGNALING_NOT_OPEN;
	bool identity_normalized = false;
	bool media_transport_ephemeral = false;
	bool final_state_unregistered = false;
	bool registrar_succeeded;

	CHECK(start_loopback_registrar(&registrar),
	      "could not start loopback SIP registrar");
	config_init(&config);
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
		       registrar.port);
	signaling = create_signaling(&config, &capture);
	if (signaling != NULL) {
		open_result = sip_signaling_open(signaling);
		if (open_result == SIP_SIGNALING_OK) {
			const char *identity =
				sip_signaling_identity_uri(signaling);

			identity_normalized = identity != NULL &&
				strcmp(identity, "sip:alice@127.0.0.1") == 0;
			media_transport_ephemeral =
				library_media_transport_is_ephemeral();
			step_result = SIP_SIGNALING_OK;
			for (unsigned int attempt = 0;
			     attempt < 50 &&
			     sip_signaling_registration_state(signaling) !=
				     SIP_REGISTRATION_REGISTERED;
			     ++attempt) {
				step_result =
					sip_signaling_step(signaling, 20);
				if (step_result != SIP_SIGNALING_OK)
					break;
			}
			close_result =
				sip_signaling_close(signaling);
			final_state_unregistered =
				sip_signaling_registration_state(
					signaling) ==
				SIP_REGISTRATION_UNREGISTERED;
		}
		sip_signaling_destroy(signaling);
	}
	registrar_succeeded = finish_loopback_registrar(&registrar);

	CHECK(signaling != NULL, "could not allocate registered account");
	CHECK(open_result == SIP_SIGNALING_OK,
	      "registered account did not open: %s",
	      sip_signaling_result_name(open_result));
	CHECK(identity_normalized, "registered identity was not normalized");
	CHECK(media_transport_ephemeral,
	      "registered account retained a fixed library media port");
	CHECK(step_result == SIP_SIGNALING_OK,
	      "registration event polling failed");
	CHECK(registrar_succeeded && registrar.request_count == 2 &&
		      registrar.registration_received &&
		      registrar.unregistration_received,
	      "registrar did not complete registration and unregistration");
	CHECK(capture.registration_count == 4,
	      "registration lifecycle emitted %u events",
	      capture.registration_count);
	CHECK(capture.registration_states[0] ==
		      SIP_REGISTRATION_REGISTERING &&
		      capture.registration_states[1] ==
			      SIP_REGISTRATION_REGISTERED &&
		      capture.registration_states[2] ==
			      SIP_REGISTRATION_UNREGISTERING &&
		      capture.registration_states[3] ==
			      SIP_REGISTRATION_UNREGISTERED,
	      "registration lifecycle event order was wrong");
	CHECK(capture.registration_status_codes[1] == 200 &&
		      capture.registration_status_codes[3] == 0 &&
		      capture.registration_library_statuses[1] == 0 &&
		      capture.registration_library_statuses[3] == 0 &&
		      capture.registration_expirations[1] == 60 &&
		      capture.registration_expirations[3] == 0 &&
		      strcmp(capture.registration_reasons[1], "OK") == 0,
	      "registration result details were wrong");
	CHECK(close_result == SIP_SIGNALING_OK,
	      "registered account did not close");
	CHECK(final_state_unregistered,
	      "local shutdown did not retain the unregistered state");
	CHECK(strcmp(sip_registration_state_name(
			     SIP_REGISTRATION_REGISTERED),
		     "registered") == 0,
	      "registration state name was wrong");
	return true;
}

static bool test_outgoing_call_lifecycle(void)
{
	struct callback_capture capture = { 0 };
	struct loopback_call_peer peer;
	struct app_config config;
	struct sip_signaling *signaling = NULL;
	enum sip_signaling_result open_result =
		SIP_SIGNALING_INVALID_ARGUMENT;
	enum sip_signaling_result invalid_dial_result =
		SIP_SIGNALING_OK;
	enum sip_signaling_result no_call_result =
		SIP_SIGNALING_OK;
	enum sip_signaling_result dial_result =
		SIP_SIGNALING_INVALID_ARGUMENT;
	enum sip_signaling_result duplicate_dial_result =
		SIP_SIGNALING_OK;
	enum sip_signaling_result hangup_result =
		SIP_SIGNALING_NO_CALL;
	enum sip_signaling_result close_result =
		SIP_SIGNALING_NOT_OPEN;
	bool peer_succeeded;
	bool final_state_idle = false;

	CHECK(start_loopback_call(&peer),
	      "could not start loopback SIP call peer");
	config_init(&config);
	config.sip.listen_port = 0;
	config.sip.log_level = 3;
	config.sip.registration_enabled = false;
	(void)snprintf(config.sip.public_address,
		       sizeof(config.sip.public_address), "127.0.0.1");
	(void)snprintf(config.sip.domain, sizeof(config.sip.domain),
		       "127.0.0.1:%u", peer.port);
	(void)snprintf(config.sip.target, sizeof(config.sip.target), "bob");
	config.media.rtp_listen_port = TEST_MODEM_RTP_PORT;
	config.media.frame_duration_ms = 20;
	signaling = create_signaling(&config, &capture);
	if (signaling != NULL) {
		open_result = sip_signaling_open(signaling);
		if (open_result == SIP_SIGNALING_OK) {
			no_call_result =
				sip_signaling_hangup(signaling);
			invalid_dial_result = sip_signaling_dial(
				signaling, "bad\r\ntarget");
			dial_result =
				sip_signaling_dial(signaling, NULL);
			if (dial_result == SIP_SIGNALING_OK) {
				duplicate_dial_result =
					sip_signaling_dial(
						signaling, "ignored");
				for (unsigned int attempt = 0;
				     attempt < 50 &&
				     sip_signaling_call_state(
					     signaling) !=
					     SIP_CALL_EARLY;
				     ++attempt)
					(void)sip_signaling_step(
						signaling, 20);
				hangup_result =
					sip_signaling_hangup(signaling);
			}
		}
	}
	peer_succeeded = finish_loopback_call(&peer);
	if (signaling != NULL) {
		if (sip_signaling_is_open(signaling)) {
			(void)sip_signaling_step(signaling, 0);
			final_state_idle =
				sip_signaling_call_state(signaling) ==
				SIP_CALL_IDLE;
			close_result =
				sip_signaling_close(signaling);
		}
		sip_signaling_destroy(signaling);
	}

	CHECK(signaling != NULL, "could not allocate outgoing call context");
	CHECK(open_result == SIP_SIGNALING_OK,
	      "outgoing call context did not open: %s",
	      sip_signaling_result_name(open_result));
	CHECK(invalid_dial_result == SIP_SIGNALING_INVALID_ARGUMENT,
	      "unsafe call target was accepted");
	CHECK(no_call_result == SIP_SIGNALING_NO_CALL,
	      "idle signaling context accepted hangup");
	CHECK(dial_result == SIP_SIGNALING_OK,
	      "configured call target was not dialed: %s",
	      sip_signaling_result_name(dial_result));
	CHECK(duplicate_dial_result == SIP_SIGNALING_CALL_ACTIVE,
	      "a second simultaneous call was accepted");
	CHECK(hangup_result == SIP_SIGNALING_OK,
	      "outgoing call did not hang up");
	CHECK(peer_succeeded && peer.invite_received && peer.target_received &&
		      peer.modem_sdp_received && peer.cancel_received,
	      "loopback peer did not complete INVITE and CANCEL");
	CHECK(capture.media_count == 0,
	      "provisional outgoing call configured media prematurely");
	CHECK(capture.call_count == 3 &&
		      capture.call_states[0] == SIP_CALL_DIALING &&
		      capture.call_states[1] == SIP_CALL_EARLY &&
		      capture.call_states[2] == SIP_CALL_IDLE,
	      "outgoing call lifecycle event order was wrong");
	CHECK(capture.call_directions[0] ==
		      SIP_CALL_DIRECTION_OUTGOING &&
		      capture.call_directions[1] ==
			      SIP_CALL_DIRECTION_OUTGOING &&
		      capture.call_directions[2] ==
			      SIP_CALL_DIRECTION_OUTGOING &&
		      capture.call_status_codes[1] == 180 &&
		      strcmp(capture.call_reasons[1], "Ringing") == 0 &&
		      strstr(capture.call_remote_identities[1],
			     "sip:bob@127.0.0.1:") != NULL,
	      "outgoing call event details were wrong");
	CHECK(final_state_idle && close_result == SIP_SIGNALING_OK,
	      "outgoing call context did not return to idle and close");
	CHECK(strcmp(sip_call_state_name(
			     SIP_CALL_DIALING),
		     "dialing") == 0,
	      "call state name was wrong");
	return true;
}

static bool test_incoming_call_lifecycle(void)
{
	struct callback_capture capture = { 0 };
	struct loopback_incoming_peer peer;
	struct app_config config;
	struct sip_signaling *signaling;
	enum sip_signaling_result open_result;
	enum sip_signaling_result answer_result =
		SIP_SIGNALING_NO_CALL;
	enum sip_signaling_result second_answer_result =
		SIP_SIGNALING_NO_CALL;
	enum sip_signaling_result hangup_result =
		SIP_SIGNALING_NO_CALL;
	enum sip_signaling_result close_result =
		SIP_SIGNALING_NOT_OPEN;
	bool peer_started = false;
	bool peer_succeeded = false;
	bool connected = false;
	bool final_state_idle = false;
	bool final_media_inactive = false;

	config_init(&config);
	config.sip.listen_port = 0;
	config.sip.log_level = 3;
	config.sip.registration_enabled = false;
	(void)snprintf(config.sip.public_address,
		       sizeof(config.sip.public_address), "127.0.0.1");
	config.media.rtp_listen_port = TEST_MODEM_RTP_PORT;
	signaling = create_signaling(&config, &capture);
	CHECK(signaling != NULL, "could not allocate incoming call context");
	open_result = sip_signaling_open(signaling);
	if (open_result == SIP_SIGNALING_OK) {
		peer_started = start_incoming_call(
			&peer, sip_signaling_local_port(signaling));
		if (peer_started) {
			for (unsigned int attempt = 0;
			     attempt < 50 &&
			     sip_signaling_call_state(signaling) !=
				     SIP_CALL_EARLY;
			     ++attempt)
				(void)sip_signaling_step(signaling, 20);
			answer_result =
				sip_signaling_answer(signaling);
			if (answer_result == SIP_SIGNALING_OK)
				second_answer_result =
					sip_signaling_answer(signaling);
			for (unsigned int attempt = 0;
			     attempt < 50 &&
			     sip_signaling_call_state(signaling) !=
				     SIP_CALL_CONNECTED;
			     ++attempt)
				(void)sip_signaling_step(signaling, 20);
			connected =
				sip_signaling_call_state(signaling) ==
				SIP_CALL_CONNECTED;
			peer_succeeded = finish_incoming_call(&peer);
			if (sip_signaling_call_state(signaling) !=
			    SIP_CALL_IDLE)
				hangup_result =
					sip_signaling_hangup(signaling);
			final_state_idle =
				sip_signaling_call_state(signaling) ==
				SIP_CALL_IDLE;
			final_media_inactive =
				sip_signaling_media_state(signaling) ==
				SIP_MEDIA_INACTIVE;
		}
		close_result = sip_signaling_close(signaling);
	}
	sip_signaling_destroy(signaling);

	CHECK(open_result == SIP_SIGNALING_OK,
	      "incoming call context did not open: %s",
	      sip_signaling_result_name(open_result));
	CHECK(peer_started && peer_succeeded && peer.invite_sent &&
		      peer.ringing_received && peer.answer_received &&
		      peer.modem_sdp_received && peer.ack_sent,
	      "loopback peer did not complete the incoming dialog");
	CHECK(answer_result == SIP_SIGNALING_OK &&
		      second_answer_result == SIP_SIGNALING_OK &&
		      connected,
	      "incoming call did not connect after answer");
	CHECK(hangup_result == SIP_SIGNALING_OK && final_state_idle,
	      "incoming call did not return to idle after hangup");
	CHECK(capture.media_count == 2 &&
		      capture.media_states[0] ==
			      SIP_MEDIA_CONFIGURED &&
		      capture.media_states[1] == SIP_MEDIA_INACTIVE &&
		      capture.media_families[0] ==
			      SIP_MEDIA_ADDRESS_IPV4 &&
		      capture.media_families[1] ==
			      SIP_MEDIA_ADDRESS_UNSPECIFIED &&
		      capture.media_codecs[0] == MEDIA_CODEC_PCMU &&
		      capture.media_payload_types[0] == 0 &&
		      capture.media_sample_rates[0] ==
			      MEDIA_SAMPLE_RATE &&
		      capture.media_channels[0] == 1 &&
		      capture.media_local_ports[0] == TEST_MODEM_RTP_PORT &&
		      capture.media_remote_ports[0] == TEST_REMOTE_RTP_PORT &&
		      strcmp(capture.media_remote_addresses[0], "127.0.0.1") ==
			      0 &&
		      capture.media_remote_ports[1] == 0 &&
		      capture.media_remote_addresses[1][0] == '\0' &&
		      final_media_inactive,
	      "incoming media endpoint lifecycle was wrong");
	CHECK(capture.call_count == 4 &&
		      capture.call_states[0] ==
			      SIP_CALL_INCOMING_PENDING &&
		      capture.call_states[1] == SIP_CALL_EARLY &&
		      capture.call_states[2] == SIP_CALL_CONNECTED &&
		      capture.call_states[3] == SIP_CALL_IDLE,
	      "incoming call lifecycle event order was wrong");
	CHECK(capture.call_directions[0] ==
		      SIP_CALL_DIRECTION_INCOMING &&
		      capture.call_directions[1] ==
			      SIP_CALL_DIRECTION_INCOMING &&
		      capture.call_directions[2] ==
			      SIP_CALL_DIRECTION_INCOMING &&
		      capture.call_directions[3] ==
			      SIP_CALL_DIRECTION_INCOMING &&
		      strstr(capture.call_remote_identities[0],
			     "sip:caller@127.0.0.1") != NULL,
	      "incoming call event details were wrong");
	CHECK(close_result == SIP_SIGNALING_OK,
	      "incoming call context did not close");
	CHECK(strcmp(sip_media_state_name(
			     SIP_MEDIA_CONFIGURED),
		     "configured") == 0,
	      "media state name was wrong");
	return true;
}

static bool test_incoming_caller_facts(void)
{
	char oversized[129];
	memset(oversized, '1', sizeof(oversized) - 1);
	oversized[sizeof(oversized) - 1] = '\0';
	const struct { const char *caller, *privacy, *expected; } cases[] = {
		{ "+12025550123", "", "+12025550123" },
		{ "12025550124", "", "12025550124" },
		{ "202-555-0123", "", "2025550123" },
		{ "+1(202)555.0123", "", "+12025550123" },
		{ "anonymous", "", "" }, { "+", "", "" }, { "().-", "", "" },
		{ "123%1B%5B2J", "", "" }, { oversized, "", "" },
		{ "1234", "Privacy: none\r\n", "1234" },
		{ "1234", "Privacy: uSeR;critical\r\n", "" },
		{ "1234", "Privacy: id\r\n", "" },
		{ "1234", "Privacy: header\r\n", "" },
		{ "1234", "Privacy: none\r\nPrivacy: user\r\n", "" },
		{ "+1-202-555-0125", "", "+12025550125" },
	};
	struct callback_capture capture = { 0 };
	struct app_config config;
	config_init(&config);
	config.sip.listen_port = 0;
	config.sip.registration_enabled = false;
	config.sip.log_level = 0;
	strcpy(config.sip.public_address, "127.0.0.1");
	config.media.rtp_listen_port = TEST_MODEM_RTP_PORT;
	struct sip_signaling *signaling = create_signaling(&config, &capture);
	CHECK(signaling != NULL && sip_signaling_open(signaling) == SIP_SIGNALING_OK,
	      "caller identity server did not open");
	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		memset(&capture, 0, sizeof(capture));
		struct loopback_incoming_peer peer;
		CHECK(start_incoming_identity(&peer, sip_signaling_local_port(signaling),
		      cases[index].caller, "5678", cases[index].privacy), "caller fixture did not start");
		for (unsigned int attempt = 0; attempt < 100 && sip_signaling_call_state(signaling) != SIP_CALL_EARLY; ++attempt)
			CHECK(sip_signaling_step(signaling, 10) == SIP_SIGNALING_OK, "caller fixture poll failed");
		CHECK(capture.call_count > 0 && capture.call_generations[0] == index + 1 &&
		      strcmp(capture.caller_numbers[0], cases[index].expected) == 0 &&
		      strcmp(capture.called_numbers[0], "5678") == 0,
		      "caller/called facts wrong for case %zu", index);
		CHECK(sip_signaling_answer(signaling) == SIP_SIGNALING_OK, "caller fixture answer failed");
		for (unsigned int attempt = 0; attempt < 100 && sip_signaling_call_state(signaling) != SIP_CALL_CONNECTED; ++attempt)
			CHECK(sip_signaling_step(signaling, 10) == SIP_SIGNALING_OK, "caller connect poll failed");
		CHECK(sip_signaling_call_state(signaling) == SIP_CALL_CONNECTED && finish_incoming_call(&peer),
		      "caller fixture failed to connect");
		CHECK(sip_signaling_hangup(signaling) == SIP_SIGNALING_OK, "caller fixture hangup failed");
		for (unsigned int event = 0; event < capture.call_count && event < TEST_CALL_EVENTS; ++event)
			CHECK(strcmp(capture.caller_numbers[event], cases[index].expected) == 0,
			      "caller changed across one dialog");
	}
	memset(&capture, 0, sizeof(capture));
	unsigned int unused_port;
	int unused = open_loopback_listener(&unused_port);
	CHECK(unused >= 0, "outgoing identity fixture failed");
	close(unused);
	char target[128];
	snprintf(target, sizeof(target), "sip:1234@127.0.0.1:%u", unused_port);
	CHECK(sip_signaling_dial(signaling, target) == SIP_SIGNALING_OK,
	      "outgoing identity reset failed");
	CHECK(capture.call_count > 0 && !capture.caller_numbers[0][0],
	      "outgoing call inherited incoming caller identity");
	CHECK(sip_signaling_hangup(signaling) == SIP_SIGNALING_OK, "outgoing identity fixture cleanup failed");
	sip_signaling_destroy(signaling);
	return true;
}

static unsigned int open_fd_count(void)
{
	DIR *directory = opendir("/proc/self/fd");
	if (directory == NULL)
		return UINT32_MAX;
	unsigned int count = 0;
	struct dirent *entry;
	while ((entry = readdir(directory)) != NULL)
		if (entry->d_name[0] != '.')
			count++;
	closedir(directory);
	return count;
}

static uint64_t monotonic_ms(void)
{
	struct timespec time;
	if (clock_gettime(CLOCK_MONOTONIC, &time) != 0)
		return 0;
	return (uint64_t)time.tv_sec * 1000 + (unsigned int)time.tv_nsec / 1000000;
}

static bool test_external_endpoint_owner(void)
{
	pj_log_func *writer = pj_log_get_log_func();
	int level = pj_log_get_level();
	pj_log_set_level(0);
	CHECK(pjsua_create() == PJ_SUCCESS, "external SDK endpoint create failed");
	struct app_config config;
	config_init(&config);
	config.sip.listen_port = 0;
	config.sip.registration_enabled = false;
	config.sip.log_level = 0;
	struct sip_server *server = sip_server_create(&config, 1);
	CHECK(server != NULL && sip_server_open(server) == SIP_SIGNALING_BUSY, "external SDK endpoint was overwritten");
	CHECK(pjsua_get_state() == PJSUA_STATE_CREATED, "external SDK endpoint state changed");
	sip_server_destroy(server);
	CHECK(pjsua_get_state() == PJSUA_STATE_CREATED, "unstarted server destroyed external SDK endpoint");
	CHECK(pjsua_destroy2(PJSUA_DESTROY_NO_NETWORK) == PJ_SUCCESS, "external endpoint cleanup failed");
	pj_log_set_log_func(writer);
	pj_log_set_level(level);
	return true;
}

static bool test_shared_two_calls(void)
{
	struct app_config config;
	config_init(&config);
	config.sip.listen_port = 0;
	config.sip.registration_enabled = false;
	config.sip.log_level = 0;
	strcpy(config.sip.public_address, "127.0.0.1");
	strcpy(config.sip.bound_address, "127.0.0.1");
	config.media.frame_duration_ms = 20;
	struct sip_server *server = sip_server_create(&config, 4);
	CHECK(server != NULL && sip_server_open(server) == SIP_SIGNALING_OK, "shared server open failed");
	struct callback_capture captures[4] = { 0 };
	struct sip_signaling *lines[4];
	for (unsigned int i = 0; i < 4; ++i) {
		config.media.rtp_listen_port = TEST_MODEM_RTP_PORT + 2 * i;
		struct sip_signaling_callbacks callbacks = {
			.context = &captures[i], .call_changed = capture_call,
			.media_changed = capture_media, .error = capture_error,
		};
		lines[i] = sip_signaling_create_shared(server, &config, &callbacks);
		CHECK(lines[i] != NULL && sip_signaling_open(lines[i]) == SIP_SIGNALING_OK, "shared line %u open failed", i);
	}
	char target[128];
	snprintf(target, sizeof(target), "sip:1234@127.0.0.1:%u", sip_server_local_port(server));
	CHECK(sip_signaling_dial(lines[0], target) == SIP_SIGNALING_OK, "first shared dial failed");
	for (unsigned int i = 0; i < 100 && sip_signaling_call_state(lines[1]) == SIP_CALL_IDLE; ++i)
		CHECK(sip_server_step(server, 2) == SIP_SIGNALING_OK, "shared poll failed");
	CHECK((sip_signaling_call_state(lines[1]) == SIP_CALL_INCOMING_PENDING || sip_signaling_call_state(lines[1]) == SIP_CALL_EARLY), "incoming call routed to wrong line");
	CHECK(strcmp(captures[1].called_numbers[0], "1234") == 0 && captures[1].call_generations[0] == 1, "incoming routing facts/generation missing");
	CHECK(sip_signaling_answer(lines[1]) == SIP_SIGNALING_OK, "first shared answer failed");
	for (unsigned int i = 0; i < 100 && (sip_signaling_call_state(lines[0]) != SIP_CALL_CONNECTED || sip_signaling_call_state(lines[1]) != SIP_CALL_CONNECTED); ++i)
		CHECK(sip_server_step(server, 2) == SIP_SIGNALING_OK, "shared poll failed");
	CHECK(sip_signaling_call_state(lines[0]) == SIP_CALL_CONNECTED && sip_signaling_call_state(lines[1]) == SIP_CALL_CONNECTED, "first shared call did not connect");
	snprintf(target, sizeof(target), "sip:5678@127.0.0.1:%u", sip_server_local_port(server));
	CHECK(sip_signaling_dial(lines[2], target) == SIP_SIGNALING_OK, "second shared dial failed");
	for (unsigned int i = 0; i < 100 && sip_signaling_call_state(lines[3]) == SIP_CALL_IDLE; ++i)
		CHECK(sip_server_step(server, 2) == SIP_SIGNALING_OK, "shared poll failed");
	CHECK((sip_signaling_call_state(lines[3]) == SIP_CALL_INCOMING_PENDING || sip_signaling_call_state(lines[3]) == SIP_CALL_EARLY), "second incoming call routed to wrong line");
	CHECK(sip_signaling_answer(lines[3]) == SIP_SIGNALING_OK, "second shared answer failed");
	for (unsigned int i = 0; i < 100 && (sip_signaling_call_state(lines[2]) != SIP_CALL_CONNECTED || sip_signaling_call_state(lines[3]) != SIP_CALL_CONNECTED); ++i)
		CHECK(sip_server_step(server, 2) == SIP_SIGNALING_OK, "shared poll failed");
	for (unsigned int i = 0; i < 4; ++i)
		CHECK(sip_signaling_call_state(lines[i]) == SIP_CALL_CONNECTED && captures[i].media_count == 1 && captures[i].media_local_ports[0] == TEST_MODEM_RTP_PORT + 2 * i, "shared call %u mixed early SDP/media ownership", i);
	CHECK(sip_signaling_hangup(lines[0]) == SIP_SIGNALING_OK, "first shared hangup failed");
	sip_signaling_destroy(lines[0]);
	lines[0] = NULL;
	for (unsigned int i = 0; i < 50; ++i)
		CHECK(sip_server_step(server, 1) == SIP_SIGNALING_OK, "shared poll after line destroy failed");
	CHECK(sip_signaling_call_state(lines[2]) == SIP_CALL_CONNECTED && sip_signaling_call_state(lines[3]) == SIP_CALL_CONNECTED, "one line destroy ended another call");
	CHECK(sip_signaling_call_state(lines[1]) == SIP_CALL_IDLE, "peer call did not release");
	/* Delayed SDK callbacks must not end the next call on a reused line. */
	snprintf(target, sizeof(target), "sip:4321@127.0.0.1:%u", sip_server_local_port(server));
	CHECK(sip_signaling_dial(lines[1], target) == SIP_SIGNALING_OK, "reused line dial failed");
	CHECK(sip_signaling_hangup(lines[1]) == SIP_SIGNALING_OK, "reused line cancellation failed");
	for (unsigned int i = 0; i < 25; ++i)
		CHECK(sip_server_step(server, 1) == SIP_SIGNALING_OK, "shared poll after reused line failed");
	CHECK(sip_signaling_call_state(lines[2]) == SIP_CALL_CONNECTED && sip_signaling_call_state(lines[3]) == SIP_CALL_CONNECTED, "retired SDK call affected another line");
	config.media.rtp_listen_port = TEST_MODEM_RTP_PORT;
	struct sip_signaling_callbacks callbacks = {
		.context = &captures[0], .call_changed = capture_call,
		.media_changed = capture_media, .error = capture_error,
	};
	lines[0] = sip_signaling_create_shared(server, &config, &callbacks);
	CHECK(lines[0] != NULL && sip_signaling_open(lines[0]) == SIP_SIGNALING_OK, "recreated line failed");
	unsigned int baseline_fds = open_fd_count(), peak_fds = baseline_fds;
	CHECK(baseline_fds != UINT32_MAX, "could not inspect owned descriptor count");
	/* Instrumentation may give retired SDK keys time to become reusable.
	 * Inject a real socket-registration failure independently of call speed. */
	unsigned int errors_before = captures[1].error_count;
	next_ioqueue_error = PJ_ETOOMANY;
	CHECK(sip_signaling_dial(lines[1], target) == SIP_SIGNALING_LIBRARY_ERROR &&
	      next_ioqueue_error == PJ_SUCCESS &&
	      sip_signaling_call_state(lines[1]) == SIP_CALL_IDLE &&
	      captures[1].error_count == errors_before + 1 &&
	      captures[1].last_error_status == PJ_ETOOMANY,
	      "socket capacity failure did not release the outgoing call");
	CHECK(open_fd_count() <= baseline_fds &&
	      sip_signaling_call_state(lines[2]) == SIP_CALL_CONNECTED &&
	      sip_signaling_call_state(lines[3]) == SIP_CALL_CONNECTED,
	      "socket capacity failure leaked descriptors or ended another call");
	bool capacity_seen = false;
	unsigned int completed = 0;
	for (unsigned int cycle = 0; cycle < 160; ++cycle) {
		memset(&captures[0], 0, sizeof(captures[0]));
		memset(&captures[1], 0, sizeof(captures[1]));
		enum sip_signaling_result rc = sip_signaling_dial(lines[1], target);
		if (rc != SIP_SIGNALING_OK) {
			CHECK(rc == SIP_SIGNALING_LIBRARY_ERROR || rc == SIP_SIGNALING_BUSY, "capacity returned unexpected result");
			capacity_seen = true;
			break;
		}
		for (unsigned int i = 0; i < 100 && sip_signaling_call_state(lines[0]) == SIP_CALL_IDLE && sip_signaling_call_state(lines[1]) != SIP_CALL_IDLE; ++i)
			CHECK(sip_server_step(server, 0) == SIP_SIGNALING_OK, "capacity incoming poll failed");
		if (sip_signaling_call_state(lines[0]) == SIP_CALL_IDLE) {
			CHECK(sip_signaling_call_state(lines[1]) == SIP_CALL_IDLE, "capacity did not release failed call");
			capacity_seen = true;
			break;
		}
		rc = sip_signaling_answer(lines[0]);
		if (rc != SIP_SIGNALING_OK) {
			CHECK(rc == SIP_SIGNALING_LIBRARY_ERROR, "capacity answer returned unexpected result");
			capacity_seen = true;
			break;
		}
		for (unsigned int i = 0; i < 100 && (sip_signaling_call_state(lines[0]) != SIP_CALL_CONNECTED || sip_signaling_call_state(lines[1]) != SIP_CALL_CONNECTED); ++i)
			CHECK(sip_server_step(server, 0) == SIP_SIGNALING_OK, "capacity connect poll failed");
		CHECK(sip_signaling_call_state(lines[0]) == SIP_CALL_CONNECTED && sip_signaling_call_state(lines[1]) == SIP_CALL_CONNECTED, "capacity successful call failed to connect");
		unsigned int fds = open_fd_count();
		if (fds > peak_fds)
			peak_fds = fds;
		CHECK(sip_signaling_hangup(lines[0]) == SIP_SIGNALING_OK, "capacity hangup failed");
		for (unsigned int i = 0; i < 100 && sip_signaling_call_state(lines[1]) != SIP_CALL_IDLE; ++i)
			CHECK(sip_server_step(server, 0) == SIP_SIGNALING_OK, "capacity release poll failed");
		CHECK(sip_signaling_call_state(lines[1]) == SIP_CALL_IDLE, "capacity peer release failed");
		completed++;
	}
	(void)sip_signaling_hangup(lines[0]);
	(void)sip_signaling_hangup(lines[1]);
	CHECK(sip_signaling_call_state(lines[2]) == SIP_CALL_CONNECTED && sip_signaling_call_state(lines[3]) == SIP_CALL_CONNECTED, "capacity failure ended an established call");
	/* Keys close synchronously, then have a fixed 500ms safe reuse delay. */
	uint64_t start = monotonic_ms();
	for (unsigned int i = 0; i < 1200 && monotonic_ms() - start < 600; ++i) {
		CHECK(sip_server_step(server, 0) == SIP_SIGNALING_OK, "capacity recovery poll failed");
		usleep(1000);
	}
	CHECK(open_fd_count() <= baseline_fds, "closed SDK transports leaked descriptors");
	CHECK(sip_signaling_dial(lines[1], target) == SIP_SIGNALING_OK, "capacity failed to recover after key retirement");
	for (unsigned int i = 0; i < 100 && sip_signaling_call_state(lines[0]) == SIP_CALL_IDLE; ++i)
		CHECK(sip_server_step(server, 1) == SIP_SIGNALING_OK, "recovered incoming poll failed");
	CHECK(sip_signaling_call_state(lines[0]) != SIP_CALL_IDLE && sip_signaling_answer(lines[0]) == SIP_SIGNALING_OK, "recovered incoming admission failed");
	for (unsigned int i = 0; i < 100 && (sip_signaling_call_state(lines[0]) != SIP_CALL_CONNECTED || sip_signaling_call_state(lines[1]) != SIP_CALL_CONNECTED); ++i)
		CHECK(sip_server_step(server, 1) == SIP_SIGNALING_OK, "recovered call poll failed");
	CHECK(sip_signaling_call_state(lines[0]) == SIP_CALL_CONNECTED && sip_signaling_call_state(lines[1]) == SIP_CALL_CONNECTED, "recovered call failed to connect");
	printf("SDK recycle: completed=%u fd-baseline=%u fd-peak=%u retirement-ms=500 capacity-exhausted=%s\n",
	       completed, baseline_fds, peak_fds, capacity_seen ? "yes" : "no");
	for (unsigned int i = 0; i < 4; ++i)
		sip_signaling_destroy(lines[i]);
	CHECK(sip_server_close(server) == SIP_SIGNALING_OK && sip_server_close(server) == SIP_SIGNALING_OK, "shared server close not idempotent");
	sip_server_destroy(server);
	return true;
}

static bool test_incoming_slot_reuse(void)
{
	struct app_config config;
	config_init(&config);
	config.sip.listen_port = 0;
	config.sip.registration_enabled = false;
	config.sip.log_level = 0;
	strcpy(config.sip.public_address, "127.0.0.1");
	strcpy(config.sip.bound_address, "127.0.0.1");
	struct sip_server *server = sip_server_create(&config, 2);
	CHECK(server != NULL && sip_server_open(server) == SIP_SIGNALING_OK, "reuse server open failed");
	struct callback_capture captures[2] = { 0 };
	struct sip_signaling *lines[2];
	for (unsigned int i = 0; i < 2; ++i) {
		config.media.rtp_listen_port = TEST_MODEM_RTP_PORT + 2 * i;
		struct sip_signaling_callbacks callbacks = {
			.context = &captures[i], .admit = capture_admit, .call_changed = capture_call,
			.media_changed = capture_media, .error = capture_error,
		};
		lines[i] = sip_signaling_create_shared(server, &config, &callbacks);
		CHECK(lines[i] != NULL && sip_signaling_open(lines[i]) == SIP_SIGNALING_OK, "reuse line open failed");
	}
	char target[128];
	snprintf(target, sizeof(target), "sip:1234@127.0.0.1:%u", sip_server_local_port(server));
	/* A rejected call must not attach later when admission becomes available. */
	captures[1].deny_incoming = true;
	CHECK(sip_signaling_dial(lines[0], target) == SIP_SIGNALING_OK, "rejected dial failed");
	for (unsigned int i = 0; i < 100 && sip_signaling_call_state(lines[0]) != SIP_CALL_IDLE; ++i)
		CHECK(sip_server_step(server, 1) == SIP_SIGNALING_OK, "rejected call poll failed");
	CHECK(sip_signaling_call_state(lines[0]) == SIP_CALL_IDLE, "rejected call did not end");
	captures[1].deny_incoming = false;
	for (unsigned int i = 0; i < 20; ++i)
		CHECK(sip_server_step(server, 1) == SIP_SIGNALING_OK, "late rejected callback poll failed");
	CHECK(sip_signaling_call_state(lines[1]) == SIP_CALL_IDLE && captures[1].call_count == 0, "rejected call claimed an idle line");
	/* More than the complete SDK slot count, without restarting its server. */
	for (unsigned int cycle = 0; cycle < PJSUA_MAX_CALLS + 4; ++cycle) {
		memset(captures, 0, sizeof(captures));
		const char *user = cycle == 0 ? "+" : cycle == 1 ? "+1234" : "1234";
		snprintf(target, sizeof(target), "sip:%s@127.0.0.1:%u", user, sip_server_local_port(server));
		CHECK(sip_signaling_dial(lines[0], target) == SIP_SIGNALING_OK, "reuse dial %u failed", cycle);
		for (unsigned int i = 0; i < 100 && sip_signaling_call_state(lines[1]) == SIP_CALL_IDLE; ++i)
			CHECK(sip_server_step(server, 1) == SIP_SIGNALING_OK, "reuse poll failed");
		CHECK(sip_signaling_call_state(lines[1]) == SIP_CALL_EARLY || sip_signaling_call_state(lines[1]) == SIP_CALL_INCOMING_PENDING, "incoming SDK slot reuse failed at %u", cycle);
		CHECK(captures[1].call_generations[0] == cycle + 1, "reused call generation wrong");
		CHECK(strcmp(captures[1].called_numbers[0], cycle == 0 ? "" : user) == 0, "invalid or numeric called-number policy wrong");
		CHECK(sip_signaling_answer(lines[1]) == SIP_SIGNALING_OK, "reuse answer failed");
		for (unsigned int i = 0; i < 100 && (sip_signaling_call_state(lines[0]) != SIP_CALL_CONNECTED || sip_signaling_call_state(lines[1]) != SIP_CALL_CONNECTED); ++i)
			CHECK(sip_server_step(server, 1) == SIP_SIGNALING_OK, "reuse connect poll failed");
		for (unsigned int i = 0; i < 2; ++i)
			CHECK(sip_signaling_call_state(lines[i]) == SIP_CALL_CONNECTED && captures[i].media_count == 1 && captures[i].media_local_ports[0] == TEST_MODEM_RTP_PORT + 2 * i, "reuse SDP ownership failed at %u/%u", cycle, i);
		CHECK(sip_signaling_hangup(lines[1]) == SIP_SIGNALING_OK, "local incoming hangup failed");
		for (unsigned int i = 0; i < 100 && sip_signaling_call_state(lines[0]) != SIP_CALL_IDLE; ++i)
			CHECK(sip_server_step(server, 1) == SIP_SIGNALING_OK, "reuse release poll failed");
		CHECK(sip_signaling_call_state(lines[0]) == SIP_CALL_IDLE && sip_signaling_call_state(lines[1]) == SIP_CALL_IDLE, "reuse release failed");
	}
	for (unsigned int i = 0; i < 2; ++i)
		sip_signaling_destroy(lines[i]);
	sip_server_destroy(server);
	return true;
}

static bool accepts_owner(void *context, void *line_owner)
{
	return context != NULL && context == line_owner;
}

static bool test_endpoint_routes(void)
{
	struct app_config config;
	config_init(&config);
	config.sip.listen_port = 0;
	config.sip.registration_enabled = false;
	config.sip.log_level = 0;
	strcpy(config.sip.public_address, "127.0.0.1");
	strcpy(config.sip.bound_address, "127.0.0.1");
	struct sip_server *server = sip_server_create(&config, 4);
	CHECK(server != NULL && sip_server_disable_default(server) == SIP_SIGNALING_OK,
		"explicit signaling owner create failed");
	struct callback_capture captures[4] = { 0 };
	struct sip_signaling *lines[4];
	for (unsigned int index = 0; index < 4; ++index) {
		config.media.rtp_listen_port = TEST_MODEM_RTP_PORT + 2 * index;
		struct sip_signaling_callbacks callbacks = {
			.context = &captures[index], .line_owner = &captures[index],
			.call_changed = capture_call, .media_changed = capture_media, .error = capture_error,
		};
		lines[index] = sip_signaling_create_shared(server, &config, &callbacks);
		CHECK(lines[index] != NULL, "explicit line allocation failed");
	}
	struct sip_endpoint *endpoints[4];
	const char *users[] = { "out", "111", "222", "333" };
	for (unsigned int index = 0; index < 4; ++index) {
		snprintf(config.sip.identity_uri, sizeof(config.sip.identity_uri), "sip:%s@127.0.0.1", users[index]);
		endpoints[index] = sip_endpoint_create(server, &config, users[index], "127.0.0.1",
			accepts_owner, index == 0 ? NULL : &captures[index == 2 ? 3 : 2]);
		CHECK(endpoints[index] != NULL, "explicit endpoint %u allocation failed", index);
	}
	CHECK(sip_server_open(server) == SIP_SIGNALING_OK, "explicit server open failed");
	for (unsigned int index = 0; index < 4; ++index) {
		CHECK(sip_endpoint_open(endpoints[index]) == SIP_SIGNALING_OK, "explicit endpoint open failed");
		CHECK(sip_signaling_open(lines[index]) == SIP_SIGNALING_OK, "explicit line open failed");
	}
	CHECK(sip_signaling_set_endpoint(lines[0], endpoints[0]) == SIP_SIGNALING_OK &&
		sip_signaling_set_endpoint(lines[1], endpoints[0]) == SIP_SIGNALING_OK, "originating endpoint assignment failed");
	unsigned int port = sip_endpoint_local_port(endpoints[1]);
	CHECK(port != 0 && port == sip_endpoint_local_port(endpoints[2]), "shared transport ownership failed");
	char target[128];
	for (unsigned int call = 0; call < 2; ++call) {
		snprintf(target, sizeof(target), "sip:%s@127.0.0.1:%u", users[call + 1], port);
		CHECK(sip_signaling_dial(lines[call], target) == SIP_SIGNALING_OK, "endpoint dial failed");
		for (unsigned int tick = 0; tick < 100 && sip_signaling_call_state(lines[call + 2]) == SIP_CALL_IDLE; ++tick)
			CHECK(sip_server_step(server, 1) == SIP_SIGNALING_OK, "endpoint poll failed");
		CHECK(sip_signaling_call_state(lines[call + 2]) == SIP_CALL_INCOMING_PENDING ||
			sip_signaling_call_state(lines[call + 2]) == SIP_CALL_EARLY, "account spilled into the wrong pool");
		CHECK(captures[call + 2].endpoint_ids[0] == sip_endpoint_id(endpoints[call + 1]),
			"native endpoint owner fact missing");
		CHECK(sip_signaling_answer(lines[call + 2]) == SIP_SIGNALING_OK, "endpoint answer failed");
		for (unsigned int tick = 0; tick < 100 && (sip_signaling_call_state(lines[call]) != SIP_CALL_CONNECTED ||
		    sip_signaling_call_state(lines[call + 2]) != SIP_CALL_CONNECTED); ++tick)
			CHECK(sip_server_step(server, 1) == SIP_SIGNALING_OK, "endpoint connect poll failed");
		CHECK(sip_signaling_call_state(lines[call]) == SIP_CALL_CONNECTED &&
			sip_signaling_call_state(lines[call + 2]) == SIP_CALL_CONNECTED, "endpoint call failed to connect");
	}
	CHECK(sip_endpoint_close(endpoints[1]) == SIP_SIGNALING_OK, "own endpoint stop failed");
	for (unsigned int tick = 0; tick < 100; ++tick)
		CHECK(sip_server_step(server, 1) == SIP_SIGNALING_OK, "endpoint shutdown poll failed");
	CHECK(sip_signaling_call_state(lines[0]) == SIP_CALL_IDLE && sip_signaling_call_state(lines[2]) == SIP_CALL_IDLE,
		"endpoint stop retained its call");
	CHECK(sip_signaling_call_state(lines[1]) == SIP_CALL_CONNECTED && sip_signaling_call_state(lines[3]) == SIP_CALL_CONNECTED,
		"endpoint stop affected another account");
	/* CANCEL of a pending call must still reach the owning transaction. */
	snprintf(target, sizeof(target), "sip:333@127.0.0.1:%u", port);
	CHECK(sip_signaling_dial(lines[0], target) == SIP_SIGNALING_OK, "shared pool retry failed");
	for (unsigned int tick = 0; tick < 100 && sip_signaling_call_state(lines[2]) == SIP_CALL_IDLE; ++tick)
		CHECK(sip_server_step(server, 1) == SIP_SIGNALING_OK, "pending retry poll failed");
	CHECK(sip_signaling_call_state(lines[2]) != SIP_CALL_IDLE, "shared pool reservation missing");
	CHECK(sip_signaling_hangup(lines[0]) == SIP_SIGNALING_OK, "pending call cancellation failed");
	for (unsigned int tick = 0; tick < 100 && sip_signaling_call_state(lines[2]) != SIP_CALL_IDLE; ++tick)
		CHECK(sip_server_step(server, 1) == SIP_SIGNALING_OK, "CANCEL poll failed");
	CHECK(sip_signaling_call_state(lines[2]) == SIP_CALL_IDLE, "initial INVITE filter blocked CANCEL");
	const char *unmatched[] = { "sip:999@127.0.0.1", "sip:333@localhost" };
	for (unsigned int route = 0; route < 2; ++route) {
		snprintf(target, sizeof(target), "%s:%u", unmatched[route], port);
		CHECK(sip_signaling_dial(lines[0], target) == SIP_SIGNALING_OK, "unmatched fixture dial failed");
		for (unsigned int tick = 0; tick < 100 && sip_signaling_call_state(lines[0]) != SIP_CALL_IDLE; ++tick)
			CHECK(sip_server_step(server, 1) == SIP_SIGNALING_OK, "unmatched route poll failed");
		CHECK(sip_signaling_call_state(lines[0]) == SIP_CALL_IDLE && sip_signaling_call_state(lines[2]) == SIP_CALL_IDLE,
			"unmatched Request-URI reached a pool");
	}
	for (unsigned int index = 0; index < 4; ++index)
		sip_signaling_destroy(lines[index]);
	sip_server_destroy(server);
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
		{ "loopback registration lifecycle",
		  test_loopback_registration_lifecycle },
		{ "outgoing call lifecycle", test_outgoing_call_lifecycle },
		{ "incoming call lifecycle", test_incoming_call_lifecycle },
		{ "bounded incoming caller facts", test_incoming_caller_facts },
		{ "external SDK owner", test_external_endpoint_owner },
		{ "two calls on shared server", test_shared_two_calls },
		{ "incoming SDK slot reuse", test_incoming_slot_reuse },
		{ "endpoint routing and isolated stop", test_endpoint_routes },
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
