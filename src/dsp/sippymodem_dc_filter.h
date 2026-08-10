// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_DC_FILTER_H
#define SIPPYMODEM_DC_FILTER_H

#include <stddef.h>
#include <stdint.h>

enum sippymodem_dc_filter_phase {
	SIPPYMODEM_DC_FILTER_WARMUP,
	SIPPYMODEM_DC_FILTER_CALIBRATION,
	SIPPYMODEM_DC_FILTER_TRACKING,
};

enum sippymodem_dc_filter_result {
	SIPPYMODEM_DC_FILTER_OK,
	SIPPYMODEM_DC_FILTER_OFFSET_LIMIT,
	SIPPYMODEM_DC_FILTER_INVALID_ARGUMENT,
};

struct sippymodem_dc_filter {
	enum sippymodem_dc_filter_phase phase;
	int64_t sample_sum;
	size_t sample_count;
	int16_t offset;
};

void sippymodem_dc_filter_init(struct sippymodem_dc_filter *filter);
enum sippymodem_dc_filter_result sippymodem_dc_filter_process(
	struct sippymodem_dc_filter *filter,
	int16_t *samples,
	size_t count);
int16_t sippymodem_dc_filter_offset(
	const struct sippymodem_dc_filter *filter);
const char *sippymodem_dc_filter_result_name(
	enum sippymodem_dc_filter_result result);

#endif
