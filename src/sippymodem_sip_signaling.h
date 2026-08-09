// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_SIP_SIGNALING_H
#define SIPPYMODEM_SIP_SIGNALING_H

#include "sippymodem_config.h"

#include <stdbool.h>
#include <stddef.h>

enum sippymodem_sip_signaling_result {
	SIPPYMODEM_SIP_SIGNALING_OK = 0,
	SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT,
	SIPPYMODEM_SIP_SIGNALING_NOT_OPEN,
	SIPPYMODEM_SIP_SIGNALING_BUSY,
	SIPPYMODEM_SIP_SIGNALING_CALL_ACTIVE,
	SIPPYMODEM_SIP_SIGNALING_NO_CALL,
	SIPPYMODEM_SIP_SIGNALING_LIBRARY_ERROR,
};

struct sippymodem_sip_signaling_error {
	const char *operation;
	int library_status;
	const char *message;
};

enum sippymodem_sip_trace_origin {
	SIPPYMODEM_SIP_TRACE_ADAPTER,
	SIPPYMODEM_SIP_TRACE_LIBRARY,
};

enum sippymodem_sip_call_direction {
	SIPPYMODEM_SIP_CALL_DIRECTION_INCOMING,
	SIPPYMODEM_SIP_CALL_DIRECTION_OUTGOING,
};

enum sippymodem_sip_call_state {
	SIPPYMODEM_SIP_CALL_IDLE,
	SIPPYMODEM_SIP_CALL_DIALING,
	SIPPYMODEM_SIP_CALL_INCOMING_PENDING,
	SIPPYMODEM_SIP_CALL_EARLY,
	SIPPYMODEM_SIP_CALL_CONNECTED,
};

enum sippymodem_sip_media_state {
	SIPPYMODEM_SIP_MEDIA_INACTIVE,
	SIPPYMODEM_SIP_MEDIA_CONFIGURED,
};

enum sippymodem_sip_media_address_family {
	SIPPYMODEM_SIP_MEDIA_ADDRESS_UNSPECIFIED,
	SIPPYMODEM_SIP_MEDIA_ADDRESS_IPV4,
	SIPPYMODEM_SIP_MEDIA_ADDRESS_IPV6,
};

/* CONFIGURED supplies the RTP peer; INACTIVE clears it before call idle. */
struct sippymodem_sip_media_event {
	enum sippymodem_sip_media_state state;
	enum sippymodem_sip_media_address_family remote_address_family;
	enum sippymodem_media_codec codec;
	unsigned int payload_type;
	unsigned int sample_rate;
	unsigned int channels;
	unsigned int local_port;
	const char *remote_address;
	unsigned int remote_port;
};

struct sippymodem_sip_call_event {
	enum sippymodem_sip_call_direction direction;
	enum sippymodem_sip_call_state state;
	unsigned int status_code;
	const char *reason;
	const char *remote_identity;
};

enum sippymodem_sip_registration_state {
	SIPPYMODEM_SIP_REGISTRATION_DISABLED,
	SIPPYMODEM_SIP_REGISTRATION_REGISTERING,
	SIPPYMODEM_SIP_REGISTRATION_REGISTERED,
	SIPPYMODEM_SIP_REGISTRATION_UNREGISTERING,
	SIPPYMODEM_SIP_REGISTRATION_UNREGISTERED,
	SIPPYMODEM_SIP_REGISTRATION_FAILED,
	SIPPYMODEM_SIP_REGISTRATION_UNREGISTER_FAILED,
};

struct sippymodem_sip_registration_event {
	enum sippymodem_sip_registration_state state;
	unsigned int status_code;
	int library_status;
	unsigned int expires_seconds;
	const char *reason;
};

struct sippymodem_sip_trace_record {
	enum sippymodem_sip_trace_origin origin;
	int level;
	const char *data;
	size_t size;
};

/*
 * Use one owner thread for all API calls. Callback records and their pointed-
 * to data are borrowed synchronously on that thread.
 */
struct sippymodem_sip_signaling_callbacks {
	void *context;
	void (*ready)(void *context);
	void (*error)(void *context,
		      const struct sippymodem_sip_signaling_error *error);
	void (*trace)(void *context,
		      const struct sippymodem_sip_trace_record *record);
	void (*registration_changed)(
		void *context,
		const struct sippymodem_sip_registration_event *event);
	void (*call_changed)(void *context,
			     const struct sippymodem_sip_call_event *event);
	void (*media_changed)(void *context,
			      const struct sippymodem_sip_media_event *event);
};

struct sippymodem_sip_signaling;

struct sippymodem_sip_signaling *sippymodem_sip_signaling_create(
	const struct sippymodem_config *config,
	const struct sippymodem_sip_signaling_callbacks *callbacks);

/*
 * close() is callback-safe and gives confirmed registrations a bounded
 * graceful unregister. Call destroy() again outside a callback to free.
 */
void sippymodem_sip_signaling_destroy(
	struct sippymodem_sip_signaling *signaling);
enum sippymodem_sip_signaling_result sippymodem_sip_signaling_open(
	struct sippymodem_sip_signaling *signaling);
enum sippymodem_sip_signaling_result sippymodem_sip_signaling_close(
	struct sippymodem_sip_signaling *signaling);

/* Poll signaling without worker threads; timeout_ms may be zero. */
enum sippymodem_sip_signaling_result sippymodem_sip_signaling_step(
	struct sippymodem_sip_signaling *signaling, unsigned int timeout_ms);
enum sippymodem_sip_signaling_result sippymodem_sip_signaling_dial(
	struct sippymodem_sip_signaling *signaling, const char *target);
enum sippymodem_sip_signaling_result sippymodem_sip_signaling_answer(
	struct sippymodem_sip_signaling *signaling);
enum sippymodem_sip_signaling_result sippymodem_sip_signaling_hangup(
	struct sippymodem_sip_signaling *signaling);

bool sippymodem_sip_signaling_is_open(
	const struct sippymodem_sip_signaling *signaling);
unsigned int sippymodem_sip_signaling_local_port(
	const struct sippymodem_sip_signaling *signaling);
/* Borrowed normalized identity, or NULL for a transport-local account. */
const char *sippymodem_sip_signaling_identity_uri(
	const struct sippymodem_sip_signaling *signaling);
enum sippymodem_sip_registration_state
sippymodem_sip_signaling_registration_state(
	const struct sippymodem_sip_signaling *signaling);
enum sippymodem_sip_call_state sippymodem_sip_signaling_call_state(
	const struct sippymodem_sip_signaling *signaling);
enum sippymodem_sip_media_state sippymodem_sip_signaling_media_state(
	const struct sippymodem_sip_signaling *signaling);
int sippymodem_sip_signaling_last_library_status(
	const struct sippymodem_sip_signaling *signaling);

const char *sippymodem_sip_signaling_result_name(
	enum sippymodem_sip_signaling_result result);
const char *sippymodem_sip_registration_state_name(
	enum sippymodem_sip_registration_state state);
const char *sippymodem_sip_call_state_name(
	enum sippymodem_sip_call_state state);
const char *sippymodem_sip_media_state_name(
	enum sippymodem_sip_media_state state);

#endif
