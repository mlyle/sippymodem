// SPDX-License-Identifier: BSD-3-Clause
#ifndef DATAPUMP_TRANSMIT_H
#define DATAPUMP_TRANSMIT_H

#include "qam.h"
#include <limits.h>
#include <stdint.h>

/* Convert a bounded encoder/filter tail using the actual rational clock. */
static inline int datapump_symbol_tail(uint32_t sample_rate,
	uint32_t numerator, uint32_t denominator, unsigned symbols)
{
	if (!numerator || !denominator || !sample_rate ||
	    (uint64_t)sample_rate * denominator > UINT64_MAX / (symbols ? symbols : 1))
		return -1;
	uint64_t units = (uint64_t)sample_rate * denominator * symbols;
	uint64_t samples = units / numerator + (units % numerator != 0);
	return samples > INT_MAX ? -1 : (int)samples;
}

/* These modulators pull one symbol at a time; accepted points remain in the
 * configured finite pulse history, including the current symbol interval. */
static inline int datapump_qam_tail(const struct qam_modulator *modulator)
{
	if (modulator == NULL || modulator->channel.pulse_span_symbols == UINT_MAX)
		return -1;
	const struct qam_channel *c = &modulator->channel;
	return datapump_symbol_tail(c->sample_rate, c->symbol_rate_numerator,
		c->symbol_rate_denominator, c->pulse_span_symbols + 1u);
}
#endif
