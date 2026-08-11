// SPDX-License-Identifier: BSD-3-Clause

#ifndef DC_FILTER_H
#define DC_FILTER_H

#include <stddef.h>
#include <stdint.h>

enum dc_filter_phase {
	DC_FILTER_WARMUP,
	DC_FILTER_CALIBRATION,
	DC_FILTER_TRACKING,
};

enum dc_filter_result {
	DC_FILTER_OK,
	DC_FILTER_OFFSET_LIMIT,
	DC_FILTER_INVALID_ARGUMENT,
};

struct dc_filter {
	enum dc_filter_phase phase;
	int64_t sample_sum;
	size_t sample_count;
	int16_t offset;
};

void dc_filter_init(struct dc_filter *filter);
enum dc_filter_result dc_filter_process(
	struct dc_filter *filter,
	int16_t *samples,
	size_t count);
int16_t dc_filter_offset(
	const struct dc_filter *filter);
const char *dc_filter_result_name(
	enum dc_filter_result result);

#endif
