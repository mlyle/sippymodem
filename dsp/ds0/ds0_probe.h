// SPDX-License-Identifier: BSD-3-Clause
#ifndef DS0_PROBE_H
#define DS0_PROBE_H
#include "ds0_codec.h"
#define DS0_PROBE_MIN_CODEWORDS 1200u
#define DS0_PROBE_HISTORY 4096u
#define DS0_PROBE_TAIL_CODEWORDS 64u
struct ds0_probe {
	struct ds0 rx[2];
	bool allowed[2];
	enum ds0_rate selected;
	uint8_t history[DS0_PROBE_HISTORY];
	size_t head, count, retained;
};
int ds0_probe_init(struct ds0_probe *, bool allow56, bool allow64);
/* Exact source codewords only. Gaps retire both candidate runs and history.
 * Selection requires 1200 whole codewords and a complete FCS-valid frame;
 * speculative frame bits and idle flags alone cannot qualify a candidate. */
enum ds0_rate ds0_probe_sample(struct ds0_probe *, uint8_t, bool available);
size_t ds0_probe_copy(const struct ds0_probe *, uint8_t *, size_t capacity);
#endif
