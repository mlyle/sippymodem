// SPDX-License-Identifier: BSD-3-Clause

#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include "media.h"
#include "log_target.h"

#include <stdbool.h>

#define APP_CONFIG_TEXT_MAX 256
#define CONFIG_ERROR_MAX 128

struct sip_config {
	char login[APP_CONFIG_TEXT_MAX];
	char user[APP_CONFIG_TEXT_MAX];
	char password[APP_CONFIG_TEXT_MAX];
	char domain[APP_CONFIG_TEXT_MAX];
	char server[APP_CONFIG_TEXT_MAX];
	char public_address[APP_CONFIG_TEXT_MAX];
	char bound_address[APP_CONFIG_TEXT_MAX];
	char identity_uri[APP_CONFIG_TEXT_MAX];
	char registrar_uri[APP_CONFIG_TEXT_MAX];
	char proxy_uri[APP_CONFIG_TEXT_MAX];
	char realm[APP_CONFIG_TEXT_MAX];
	char target[APP_CONFIG_TEXT_MAX];
	unsigned int listen_port;
	unsigned int log_level;
	bool trace_enabled;
	bool registration_enabled;
	bool automatic_answer;
};

struct media_config {
	enum media_codec codec;
	unsigned int rtp_listen_port;
	/* Remote clock: maximum hold for a real RTP gap; local clock: prefill. */
	unsigned int jitter_buffer_ms;
	unsigned int frame_duration_ms;
	bool remote_clock;
	bool pcm_codec_grid;
	double transmit_gain;
	double receive_gain;
};

struct recording_config {
	char prefix[APP_CONFIG_TEXT_MAX];
};

struct app_config {
	enum log_target log_target; /* Compatibility executable output only. */
	struct sip_config sip;
	struct media_config media;
	struct recording_config recording;
};

struct config_error {
	char setting[32];
	char message[CONFIG_ERROR_MAX];
};

void config_init(struct app_config *config);
int config_apply_environment(
	struct app_config *config,
	struct config_error *error);

#endif
