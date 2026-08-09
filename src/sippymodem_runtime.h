// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_RUNTIME_H
#define SIPPYMODEM_RUNTIME_H

#include "sippymodem_config.h"
#include "sippymodem_recording_files.h"

enum { SIPPYMODEM_RUNTIME_NAME_MAX = 64 };

struct modem;
struct sippymodem_runtime;

/* Owns the modem, closed SIP host, optional recording files, and every adapter
 * between them. Construction does not open a transport. */
struct sippymodem_runtime *sippymodem_runtime_create(
	const struct sippymodem_config *config, const char *modem_name);
void sippymodem_runtime_destroy(struct sippymodem_runtime *runtime);

struct modem *sippymodem_runtime_modem(struct sippymodem_runtime *runtime);
const struct sippymodem_recording_files *sippymodem_runtime_recording_files(
	const struct sippymodem_runtime *runtime);

#endif
