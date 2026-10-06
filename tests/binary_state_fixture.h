// SPDX-License-Identifier: BSD-3-Clause
#ifndef BINARY_STATE_FIXTURE_H
#define BINARY_STATE_FIXTURE_H

#include <binary_equalizer.h>
#include <stddef.h>
#include <string.h>

/* Preserve the complete equalizer and every binary-line field, replacing only
 * the binary owner's C padding. Never normalize live receiver state. */
static inline void binary_equalizer_snapshot(
	struct binary_equalizer *snapshot,
	const struct binary_equalizer *receiver)
{
	unsigned char bytes[sizeof(*receiver)] = { 0 };
#define COPY(field) memcpy(bytes + offsetof(struct binary_equalizer, field), \
	&receiver->field, sizeof(receiver->field))
	COPY(equalizer);
	COPY(magnitude);
	COPY(adaptation);
	COPY(polarity);
#undef COPY
	memcpy(snapshot, bytes, sizeof(bytes));
}

#endif
