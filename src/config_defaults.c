// SPDX-License-Identifier: BSD-3-Clause
#include "config.h"
#include <string.h>

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
