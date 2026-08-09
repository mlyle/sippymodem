// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_CONFIG_H
#define SIPPYMODEM_CONFIG_H

#include <stdbool.h>

#define SIPPYMODEM_CONFIG_TEXT_MAX 256
#define SIPPYMODEM_CONFIG_ERROR_MAX 128

enum sippymodem_media_codec {
	SIPPYMODEM_CODEC_PCMU,
	SIPPYMODEM_CODEC_PCMA,
	SIPPYMODEM_CODEC_CLEARMODE,
};

struct sippymodem_sip_config {
	char login[SIPPYMODEM_CONFIG_TEXT_MAX];
	char user[SIPPYMODEM_CONFIG_TEXT_MAX];
	char password[SIPPYMODEM_CONFIG_TEXT_MAX];
	char domain[SIPPYMODEM_CONFIG_TEXT_MAX];
	char server[SIPPYMODEM_CONFIG_TEXT_MAX];
	char public_address[SIPPYMODEM_CONFIG_TEXT_MAX];
	char bound_address[SIPPYMODEM_CONFIG_TEXT_MAX];
	char identity_uri[SIPPYMODEM_CONFIG_TEXT_MAX];
	char registrar_uri[SIPPYMODEM_CONFIG_TEXT_MAX];
	char proxy_uri[SIPPYMODEM_CONFIG_TEXT_MAX];
	char realm[SIPPYMODEM_CONFIG_TEXT_MAX];
	char target[SIPPYMODEM_CONFIG_TEXT_MAX];
	unsigned int listen_port;
	unsigned int log_level;
	bool registration_enabled;
	bool automatic_answer;
};

struct sippymodem_media_config {
	enum sippymodem_media_codec codec;
	unsigned int rtp_listen_port;
	unsigned int jitter_buffer_ms;
	unsigned int frame_duration_ms;
	bool remote_clock;
	double transmit_gain;
	double receive_gain;
};

struct sippymodem_recording_config {
	char prefix[SIPPYMODEM_CONFIG_TEXT_MAX];
};

struct sippymodem_config {
	struct sippymodem_sip_config sip;
	struct sippymodem_media_config media;
	struct sippymodem_recording_config recording;
};

struct sippymodem_config_error {
	char setting[32];
	char message[SIPPYMODEM_CONFIG_ERROR_MAX];
};

void sippymodem_config_init(struct sippymodem_config *config);
int sippymodem_config_apply_environment(
	struct sippymodem_config *config,
	struct sippymodem_config_error *error);
const char *sippymodem_codec_name(enum sippymodem_media_codec codec);

#endif
