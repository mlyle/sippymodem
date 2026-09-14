// SPDX-License-Identifier: BSD-3-Clause

#include "v92_qc_signal.h"

#include <math.h>
#include <stddef.h>

bool v92_qts_symbol(enum g711_law law, uint8_t index, unsigned position,
	struct v90_pcm_symbol *symbol)
{
	/* V.92 Table 2. These magnitudes are normative, not a uniform scale. */
	static const uint8_t ucodes[] = {
		61, 62, 63, 66, 67, 70, 71, 74, 75, 78, 79, 82, 83, 86, 87,
	};
	unsigned interval = position % 6u;
	bool positive = interval < 3u;

	if (index >= sizeof(ucodes) || position >= V92_QTS_TOTAL_SYMBOLS)
		return false;
	if (position >= V92_QTS_SYMBOLS)
		positive = !positive;
	/* The two zero positions have different signs, including in PCMU. */
	return v90_pcm_symbol_make(symbol, law,
		interval % 3u == 1u ? 0 : ucodes[index], positive);
}

bool v92_anspcm_init(struct v92_anspcm *signal, enum g711_law law,
	uint8_t level)
{
	static const unsigned scales[][2] = {
		{ 1334, 667 }, { 1000, 500 }, { 708, 354 }, { 500, 250 },
	};
	struct v92_anspcm initialized = { .law = law };
	const double pi = acos(-1.0);

	if (signal == NULL || !g711_law_is_valid(law) || level >= 4u)
		return false;
	/* V.92 8.3.1/Table 6, evaluated once in double precision. The largest
	 * rounded magnitude is 1887, so conversion to our signed 16-bit sample
	 * convention remains bounded. Tests check every normative codeword via
	 * independent Tables 7..10 digests; no approximation is permitted. */
	for (unsigned k = 0; k < V92_ANSPCM_PERIOD; ++k) {
		bool alaw = law == G711_LAW_PCMA;
		double value = scales[level][alaw] * sqrt(2.0) *
			cos(2.0 * pi * k * 79.0 / 301.0 + pi / 1204.0);
		int x = (int)floor(value + 0.5);
		/* Table 6 uses signed 14/13-bit linear values. A-law's negative
		 * magnitude must survive our 16-bit one's-complement encoder. */
		int16_t sample = (int16_t)(x * (alaw ? 8 : 4) -
			(alaw && x < 0 ? 1 : 0));

		initialized.period[k] = alaw ? g711_encode_pcma(sample) :
			g711_encode_pcmu(sample);
	}
	*signal = initialized;
	return true;
}

bool v92_anspcm_sample(struct v92_anspcm *signal,
	struct v90_pcm_symbol *symbol)
{
	uint8_t octet;

	if (signal == NULL || symbol == NULL ||
	    signal->position >= 2u * V92_ANSPCM_REVERSAL_PERIOD ||
	    !g711_law_is_valid(signal->law))
		return false;
	octet = signal->period[signal->position % V92_ANSPCM_PERIOD];
	if (signal->position >= V92_ANSPCM_REVERSAL_PERIOD)
		octet ^= UINT8_C(0x80);
	if (!v90_pcm_symbol_parse(symbol, signal->law, octet))
		return false;
	if (++signal->position == 2u * V92_ANSPCM_REVERSAL_PERIOD)
		signal->position = 0;
	return true;
}
