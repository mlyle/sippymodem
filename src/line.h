// SPDX-License-Identifier: BSD-3-Clause
#ifndef MODEM_LINE_H
#define MODEM_LINE_H

#include "config.h"
#include "host.h"
#include "recording_files.h"
#include "slmodem_calls.h"

struct modem;
struct modem_line;

/* Own modem/SIP/media/recording state independently of any terminal frontend. */
struct modem_line *modem_line_create(const struct app_config *config,
	const char *name, void (*trace)(void *, const struct sip_trace_record *));
struct modem_line *modem_line_create_shared(struct sip_server *server,
	const struct app_config *config, const char *name,
	const struct sip_signaling_callbacks *callbacks);
void modem_line_destroy(struct modem_line *line);
struct modem *modem_line_modem(struct modem_line *line);
struct modem_host *modem_line_host(struct modem_line *line);
enum sip_signaling_result modem_line_last_signaling_result(const struct modem_line *line);
struct slmodem_calls *modem_line_calls(struct modem_line *line);
const struct recording_files *modem_line_recording(const struct modem_line *line);

/* SDK media jobs never enter shared signaling. The owner flushes any DSP stop
 * only after every line has returned from its bounded media batch. */
enum modem_host_result modem_line_process_media(struct modem_line *line,
	uint64_t now_ns, size_t *datagrams);
enum sip_signaling_result modem_line_flush_signaling(struct modem_line *line);

#endif
