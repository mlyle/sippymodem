// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIP_SIGNALING_H
#define SIP_SIGNALING_H

#include "config.h"

#include <stdbool.h>
#include <stddef.h>

enum sip_signaling_result {
	SIP_SIGNALING_OK = 0,
	SIP_SIGNALING_INVALID_ARGUMENT,
	SIP_SIGNALING_NOT_OPEN,
	SIP_SIGNALING_BUSY,
	SIP_SIGNALING_CALL_ACTIVE,
	SIP_SIGNALING_NO_CALL,
	SIP_SIGNALING_LIBRARY_ERROR,
};

struct sip_signaling_error {
	const char *operation;
	int library_status;
	const char *message;
};

enum sip_trace_origin {
	SIP_TRACE_ADAPTER,
	SIP_TRACE_LIBRARY,
};

enum sip_call_direction {
	SIP_CALL_DIRECTION_INCOMING,
	SIP_CALL_DIRECTION_OUTGOING,
};

enum sip_call_state {
	SIP_CALL_IDLE,
	SIP_CALL_DIALING,
	SIP_CALL_INCOMING_PENDING,
	SIP_CALL_EARLY,
	SIP_CALL_CONNECTED,
};

enum sip_media_state {
	SIP_MEDIA_INACTIVE,
	SIP_MEDIA_CONFIGURED,
};

enum sip_media_address_family {
	SIP_MEDIA_ADDRESS_UNSPECIFIED,
	SIP_MEDIA_ADDRESS_IPV4,
	SIP_MEDIA_ADDRESS_IPV6,
};

/* CONFIGURED supplies the RTP peer; INACTIVE clears it before call idle. */
struct sip_media_event {
	enum sip_media_state state;
	enum sip_media_address_family remote_address_family;
	enum media_codec codec;
	unsigned int payload_type;
	unsigned int sample_rate;
	unsigned int channels;
	unsigned int local_port;
	const char *remote_address;
	unsigned int remote_port;
};

struct sip_call_event {
	enum sip_call_direction direction;
	enum sip_call_state state;
	unsigned int status_code;
	const char *reason;
	const char *remote_identity;
};

enum sip_registration_state {
	SIP_REGISTRATION_DISABLED,
	SIP_REGISTRATION_REGISTERING,
	SIP_REGISTRATION_REGISTERED,
	SIP_REGISTRATION_UNREGISTERING,
	SIP_REGISTRATION_UNREGISTERED,
	SIP_REGISTRATION_FAILED,
	SIP_REGISTRATION_UNREGISTER_FAILED,
};

struct sip_registration_event {
	enum sip_registration_state state;
	unsigned int status_code;
	int library_status;
	unsigned int expires_seconds;
	const char *reason;
};

struct sip_trace_record {
	enum sip_trace_origin origin;
	int level;
	const char *data;
	size_t size;
};

/*
 * Use one owner thread for all API calls. Callback records and their pointed-
 * to data are borrowed synchronously on that thread.
 */
struct sip_signaling_callbacks {
	void *context;
	void (*ready)(void *context);
	void (*error)(void *context,
		      const struct sip_signaling_error *error);
	void (*trace)(void *context,
		      const struct sip_trace_record *record);
	void (*registration_changed)(
		void *context,
		const struct sip_registration_event *event);
	void (*call_changed)(void *context,
			     const struct sip_call_event *event);
	void (*media_changed)(void *context,
			      const struct sip_media_event *event);
};

struct sip_signaling;

struct sip_signaling *sip_signaling_create(
	const struct sippymodem_config *config,
	const struct sip_signaling_callbacks *callbacks);

/*
 * close() is callback-safe and gives confirmed registrations a bounded
 * graceful unregister. Call destroy() again outside a callback to free.
 */
void sip_signaling_destroy(
	struct sip_signaling *signaling);
enum sip_signaling_result sip_signaling_open(
	struct sip_signaling *signaling);
enum sip_signaling_result sip_signaling_close(
	struct sip_signaling *signaling);

/* Poll signaling without worker threads; timeout_ms may be zero. */
enum sip_signaling_result sip_signaling_step(
	struct sip_signaling *signaling, unsigned int timeout_ms);
enum sip_signaling_result sip_signaling_dial(
	struct sip_signaling *signaling, const char *target);
enum sip_signaling_result sip_signaling_answer(
	struct sip_signaling *signaling);
enum sip_signaling_result sip_signaling_hangup(
	struct sip_signaling *signaling);

bool sip_signaling_is_open(
	const struct sip_signaling *signaling);
unsigned int sip_signaling_local_port(
	const struct sip_signaling *signaling);
/* Borrowed normalized identity, or NULL for a transport-local account. */
const char *sip_signaling_identity_uri(
	const struct sip_signaling *signaling);
enum sip_registration_state
sip_signaling_registration_state(
	const struct sip_signaling *signaling);
enum sip_call_state sip_signaling_call_state(
	const struct sip_signaling *signaling);
enum sip_media_state sip_signaling_media_state(
	const struct sip_signaling *signaling);
int sip_signaling_last_library_status(
	const struct sip_signaling *signaling);

const char *sip_signaling_result_name(
	enum sip_signaling_result result);
const char *sip_registration_state_name(
	enum sip_registration_state state);
const char *sip_call_state_name(
	enum sip_call_state state);
const char *sip_media_state_name(
	enum sip_media_state state);

#endif
