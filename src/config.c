// SPDX-License-Identifier: BSD-3-Clause

#include "config.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void clear_error(struct config_error *error)
{
	if (error != NULL)
		memset(error, 0, sizeof(*error));
}

static int set_error(struct config_error *error,
		     const char *setting, const char *message)
{
	if (error != NULL) {
		snprintf(error->setting, sizeof(error->setting), "%s", setting);
		snprintf(error->message, sizeof(error->message), "%s", message);
	}
	return -1;
}

static const char *environment_value(const char *name)
{
	const char *value = getenv(name);

	return value != NULL && value[0] != '\0' ? value : NULL;
}

static int apply_string(char *destination, size_t capacity, const char *name,
			struct config_error *error)
{
	const char *value = environment_value(name);
	size_t length;

	if (value == NULL)
		return 0;
	length = strlen(value);
	if (length >= capacity)
		return set_error(error, name, "value is too long");
	memcpy(destination, value, length + 1);
	return 0;
}

static int parse_boolean(const char *text, bool *result)
{
	if (strcasecmp(text, "1") == 0 || strcasecmp(text, "true") == 0 ||
	    strcasecmp(text, "yes") == 0 || strcasecmp(text, "on") == 0) {
		*result = true;
		return 0;
	}
	if (strcasecmp(text, "0") == 0 || strcasecmp(text, "false") == 0 ||
	    strcasecmp(text, "no") == 0 || strcasecmp(text, "off") == 0) {
		*result = false;
		return 0;
	}
	return -1;
}

static int apply_boolean(bool *destination, const char *name,
			 struct config_error *error)
{
	const char *value = environment_value(name);
	bool parsed;

	if (value == NULL)
		return 0;
	if (parse_boolean(value, &parsed) != 0)
		return set_error(error, name, "expected a boolean value");
	*destination = parsed;
	return 0;
}

static int apply_unsigned(unsigned int *destination, const char *name,
			  unsigned int minimum, unsigned int maximum,
			  struct config_error *error)
{
	const char *value = environment_value(name);
	char *end = NULL;
	unsigned long parsed;

	if (value == NULL)
		return 0;
	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed < minimum ||
	    parsed > maximum)
		return set_error(error, name, "integer is outside the allowed range");
	*destination = (unsigned int)parsed;
	return 0;
}

static int apply_gain(double *destination, const char *name,
		      struct config_error *error)
{
	const char *value = environment_value(name);
	char *end = NULL;
	double parsed;

	if (value == NULL)
		return 0;
	errno = 0;
	parsed = strtod(value, &end);
	if (errno != 0 || end == value || *end != '\0' || !isfinite(parsed) ||
	    parsed < 0.0 || parsed > 4.0)
		return set_error(error, name, "gain must be between 0.0 and 4.0");
	*destination = parsed;
	return 0;
}

static int apply_codec(enum media_codec *destination,
		       const char *name,
		       struct config_error *error)
{
	const char *value = environment_value(name);

	if (value == NULL)
		return 0;
	if (strcasecmp(value, "PCMU") == 0 ||
	    strcasecmp(value, "PCMU/8000/1") == 0)
		*destination = MEDIA_CODEC_PCMU;
	else if (strcasecmp(value, "PCMA") == 0 ||
		 strcasecmp(value, "PCMA/8000/1") == 0)
		*destination = MEDIA_CODEC_PCMA;
	else if (strcasecmp(value, "CLEARMODE") == 0 ||
		 strcasecmp(value, "CLEARMODE/8000/1") == 0)
		*destination = MEDIA_CODEC_CLEARMODE;
	else
		return set_error(error, name, "unsupported media codec");
	return 0;
}

void config_init(struct app_config *config)
{
	if (config == NULL)
		return;
	memset(config, 0, sizeof(*config));
	config->sip.listen_port = 5060;
	config->sip.log_level = 4;
	config->sip.registration_enabled = true;
	config->sip.automatic_answer = false;
	config->media.codec = MEDIA_CODEC_PCMU;
	config->media.rtp_listen_port = 0;
	config->media.jitter_buffer_ms = 60;
	config->media.frame_duration_ms = 10;
	config->media.remote_clock = true;
	config->media.transmit_gain = 1.0;
	config->media.receive_gain = 1.0;
}

int config_apply_environment(
	struct app_config *config,
	struct config_error *error)
{
	struct app_config updated;
	double shared_gain;

	clear_error(error);
	if (config == NULL)
		return set_error(error, "config", "configuration is required");
	updated = *config;

#define APPLY(operation)                                                     \
	do {                                                                   \
		if ((operation) != 0)                                            \
			return -1;                                                 \
	} while (0)

	APPLY(apply_string(updated.sip.login, sizeof(updated.sip.login),
			   "SIP_LOGIN", error));
	APPLY(apply_string(updated.sip.user, sizeof(updated.sip.user),
			   "SIP_USER", error));
	APPLY(apply_string(updated.sip.password, sizeof(updated.sip.password),
			   "SIP_PASSWORD", error));
	APPLY(apply_string(updated.sip.domain, sizeof(updated.sip.domain),
			   "SIP_DOMAIN", error));
	APPLY(apply_string(updated.sip.server, sizeof(updated.sip.server),
			   "SIP_SERVER", error));
	APPLY(apply_string(updated.sip.public_address,
			   sizeof(updated.sip.public_address),
			   "SIP_PUBLIC_ADDR", error));
	APPLY(apply_string(updated.sip.bound_address,
			   sizeof(updated.sip.bound_address),
			   "SIP_BOUND_ADDR", error));
	APPLY(apply_string(updated.sip.identity_uri,
			   sizeof(updated.sip.identity_uri), "SIP_ID", error));
	APPLY(apply_string(updated.sip.registrar_uri,
			   sizeof(updated.sip.registrar_uri), "SIP_REG_URI", error));
	APPLY(apply_string(updated.sip.proxy_uri, sizeof(updated.sip.proxy_uri),
			   "SIP_PROXY", error));
	APPLY(apply_string(updated.sip.realm, sizeof(updated.sip.realm),
			   "SIP_REALM", error));
	APPLY(apply_string(updated.sip.target, sizeof(updated.sip.target),
			   "SIP_TARGET", error));
	APPLY(apply_unsigned(&updated.sip.listen_port, "SIP_PORT", 0, 65535,
			     error));
	APPLY(apply_unsigned(&updated.sip.log_level, "SIP_LOG_LEVEL", 0, 10,
			     error));
	APPLY(apply_boolean(&updated.sip.trace_enabled, "SIP_TRACE", error));
	APPLY(apply_boolean(&updated.sip.registration_enabled, "SIP_REGISTER",
			    error));
	APPLY(apply_boolean(&updated.sip.automatic_answer, "SIP_AUTO_ANSWER",
			    error));
	APPLY(apply_codec(&updated.media.codec, "SIP_CODEC", error));
	APPLY(apply_unsigned(&updated.media.rtp_listen_port, "SIP_RTP_PORT", 0,
			     65535, error));
	APPLY(apply_unsigned(&updated.media.jitter_buffer_ms, "SIP_JB_MS", 0,
			     10000, error));
	APPLY(apply_unsigned(&updated.media.frame_duration_ms, "SIP_FRAME_MS", 1,
			     1000, error));
	APPLY(apply_boolean(&updated.media.remote_clock, "SIP_REMOTE_CLOCK",
			    error));
	APPLY(apply_boolean(&updated.media.pcm_codec_grid, "SIP_PCM_CODEC_GRID",
			    error));
	APPLY(apply_string(updated.recording.prefix,
			   sizeof(updated.recording.prefix), "SIP_RECORD_PREFIX",
			   error));

	shared_gain = updated.media.transmit_gain;
	APPLY(apply_gain(&shared_gain, "SIP_GAIN", error));
	if (environment_value("SIP_GAIN") != NULL) {
		updated.media.transmit_gain = shared_gain;
		updated.media.receive_gain = shared_gain;
	}
	APPLY(apply_gain(&updated.media.transmit_gain, "SIP_TX_GAIN", error));
	APPLY(apply_gain(&updated.media.receive_gain, "SIP_RX_GAIN", error));

#undef APPLY

	*config = updated;
	return 0;
}
