// SPDX-License-Identifier: BSD-3-Clause

#include "carrier.h"

enum v32_carrier_result v32_carrier_channel(
	struct qam_channel *channel)
{
	struct qam_channel configured;

	if (channel == NULL)
		return V32_CARRIER_INVALID_ARGUMENT;
	configured = (struct qam_channel){
		.sample_rate = V32_CARRIER_SAMPLE_RATE,
		.symbol_rate_numerator = V32_CARRIER_SYMBOL_RATE,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator = V32_CARRIER_FREQUENCY_HZ,
		.carrier_frequency_denominator = 1,
		.rolloff = 0.25,
		.pulse_span_symbols = V32_CARRIER_PULSE_SPAN_SYMBOLS,
	};
	*channel = configured;
	return V32_CARRIER_OK;
}
