// SPDX-License-Identifier: BSD-3-Clause

#include "v92_qc_signal.h"
#include "pcm_s_pattern_rx.h"
#include "test.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static uint8_t encode(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMU ? g711_encode_pcmu(sample) :
		g711_encode_pcma(sample);
}

static bool test_qts(void)
{
	static const uint8_t ucodes[] = {
		61, 62, 63, 66, 67, 70, 71, 74, 75, 78, 79, 82, 83, 86, 87,
	};
	const bool signs[] = { true, true, true, false, false, false };

	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		for (unsigned index = 0; index < ARRAY_LENGTH(ucodes); ++index) {
			for (unsigned i = 0; i < V92_QTS_TOTAL_SYMBOLS; ++i) {
				struct v90_pcm_symbol symbol;
				bool positive = signs[i % 6u] ^ (i >= V92_QTS_SYMBOLS);
				unsigned code = i % 6u == 1u || i % 6u == 4u ?
					0 : ucodes[index];

				CHECK(v92_qts_symbol(law, index, i, &symbol) &&
				      v90_pcm_symbol_is_valid(&symbol, law) &&
				      symbol.positive == positive && symbol.ucode == code &&
				      encode(law, symbol.sample) == symbol.octet,
				      "QTS law %u index %u position %u changed codeword",
				      law, index, i);
				if (code == 0 && !positive)
					CHECK(symbol.octet == (law == G711_LAW_PCMU ?
						0x7f : 0x55), "QTS negative zero was lost");
			}
		}
	}
	return true;
}

static bool test_qts_receiver(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned index = 0; index < 15; ++index)
			for (unsigned channel = 0; channel < 3; ++channel)
				for (unsigned omit = 0; omit < 6; ++omit) {
					struct v90_pcm_symbol symbol;
					struct pcm_s_pattern_rx receiver;
					unsigned acquired = 0, reversed = 0, complete = 0;
					int16_t previous = 0;

					CHECK(v92_qts_symbol(law, index, 0, &symbol) &&
					      pcm_s_pattern_rx_init(&receiver, law,
						      symbol.ucode) == PCM_S_PATTERN_RX_OK,
					      "QTS receiver init failed");
					for (unsigned i = 0; i < V92_QTS_TOTAL_SYMBOLS; ++i) {
						enum pcm_s_pattern_rx_result result;
						int16_t sample;
						uint8_t octet;

						CHECK(v92_qts_symbol(law, index, i, &symbol),
						      "QTS receiver source failed");
						sample = symbol.sample;
						/* Exact octets, half gain with a 12.5% relative
						 * postcursor, or reversed line polarity. */
						if (channel == 1)
							sample = sample / 2 + previous / 16;
						else if (channel == 2)
							sample = -sample;
						previous = symbol.sample;
						if (i < omit)
							continue;
						octet = channel ? encode(law, sample) : symbol.octet;
						result = pcm_s_pattern_rx_receive(&receiver, octet);
						if (result == PCM_S_PATTERN_RX_ACQUIRED) {
							CHECK(i + 1 == omit + PCM_S_PATTERN_ACQUISITION_SYMBOLS,
							      "QTS acquisition shifted");
							++acquired;
						} else if (result == PCM_S_PATTERN_RX_REVERSAL_DETECTED) {
							CHECK(i + 1 == V92_QTS_SYMBOLS + PCM_S_PATTERN_BAR_QUALIFY_SYMBOLS,
							      "QTS reversal shifted");
							++reversed;
						} else if (result == PCM_S_PATTERN_RX_SEQUENCE_COMPLETE) {
							CHECK(i + 1 == V92_QTS_TOTAL_SYMBOLS,
							      "QTS consumed beyond the final reversed symbol");
							++complete;
						} else {
							CHECK(result == PCM_S_PATTERN_RX_OK,
							      "QTS receiver returned %s", pcm_s_pattern_rx_result_name(result));
						}
					}
					CHECK(acquired == 1 && reversed == 1 && complete == 1,
					      "QTS law %u index %u channel %u phase %u did not complete",
					      law, index, channel, omit);
					struct pcm_s_pattern_rx saved = receiver;
					CHECK(pcm_s_pattern_rx_receive(&receiver, 0xa5) ==
						      PCM_S_PATTERN_RX_SEQUENCE_COMPLETE &&
					      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
					      "completed QTS receiver consumed the next signal");
				}
	return true;
}

static bool test_anspcm(void)
{
	/* FNV-1a of the 301 octets in each column of V.92 Tables 7..10.
	 * Independently transcribed normative output, not generator output. */
	static const uint64_t hashes[4][2] = {
		{ UINT64_C(0x838c7f5c4d5bdd2c), UINT64_C(0x43849647409cb897) },
		{ UINT64_C(0xd5319c5f5212a295), UINT64_C(0x581e59d42b047dd3) },
		{ UINT64_C(0xf52d6032bf975249), UINT64_C(0xec8737d87f300c41) },
		{ UINT64_C(0x94f8c56db48526f5), UINT64_C(0x02161d163fe970d3) },
	};

	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		for (unsigned level = 0; level < 4; ++level) {
			struct v92_anspcm signal;
			uint64_t hash = UINT64_C(14695981039346656037);

			CHECK(v92_anspcm_init(&signal, law, level), "ANSpcm init failed");
			for (unsigned k = 0; k < V92_ANSPCM_PERIOD; ++k)
				hash = (hash ^ signal.period[k]) * UINT64_C(1099511628211);
			CHECK(hash == hashes[level][law],
			      "ANSpcm Table %u law %u does not match", level + 7, law);
			for (unsigned k = 0; k < 6u * V92_ANSPCM_REVERSAL_PERIOD; ++k) {
				struct v90_pcm_symbol symbol;
				uint8_t expected = signal.period[k % V92_ANSPCM_PERIOD] ^
					((k / V92_ANSPCM_REVERSAL_PERIOD) % 2u ? 0x80 : 0);

				CHECK(v92_anspcm_sample(&signal, &symbol) &&
				      v90_pcm_symbol_is_valid(&symbol, law) &&
				      symbol.octet == expected &&
				      encode(law, symbol.sample) == expected,
				      "ANSpcm law %u level %u position %u lost reversal",
				      law, level, k);
			}
			CHECK(signal.position == 0, "ANSpcm position grew without bound");
		}
	}
	return true;
}

static bool test_validation(void)
{
	struct v92_anspcm signal = { 0 }, before;
	struct v90_pcm_symbol symbol = { 0 }, saved = symbol;

	CHECK(v92_anspcm_init(&signal, G711_LAW_PCMA, 0), "init failed");
	before = signal;
	CHECK(!v92_anspcm_init(&signal, G711_LAW_PCMA, 4) &&
	      !v92_anspcm_init(&signal, (enum g711_law)2, 0) &&
	      !v92_anspcm_init(NULL, G711_LAW_PCMU, 0) &&
	      memcmp(&signal, &before, sizeof(signal)) == 0,
	      "invalid ANSpcm input changed output");
	CHECK(!v92_qts_symbol(G711_LAW_PCMU, 15, 0, &symbol) &&
	      !v92_qts_symbol(G711_LAW_PCMU, 0, 816, &symbol) &&
	      !v92_qts_symbol(G711_LAW_PCMU, UINT8_MAX, 0, &symbol) &&
	      !v92_qts_symbol(G711_LAW_PCMU, 0, UINT_MAX, &symbol) &&
	      !v92_qts_symbol((enum g711_law)2, 0, 0, &symbol) &&
	      !v92_qts_symbol(G711_LAW_PCMU, 0, 0, NULL) &&
	      memcmp(&symbol, &saved, sizeof(symbol)) == 0,
	      "invalid QTS input changed output");
	CHECK(!v92_anspcm_sample(NULL, &symbol) &&
	      !v92_anspcm_sample(&signal, NULL) &&
	      memcmp(&signal, &before, sizeof(signal)) == 0,
	      "null ANSpcm output consumed a sample");
	signal.position = 2u * V92_ANSPCM_REVERSAL_PERIOD;
	before = signal;
	CHECK(!v92_anspcm_sample(&signal, &symbol) &&
	      memcmp(&signal, &before, sizeof(signal)) == 0 &&
	      memcmp(&symbol, &saved, sizeof(symbol)) == 0,
	      "bad ANSpcm position changed output");
	signal.position = 0;
	signal.law = (enum g711_law)2;
	before = signal;
	CHECK(!v92_anspcm_sample(&signal, &symbol) &&
	      memcmp(&signal, &before, sizeof(signal)) == 0 &&
	      memcmp(&symbol, &saved, sizeof(symbol)) == 0,
	      "bad ANSpcm law changed output");
	return true;
}

static bool test_independent_signals(void)
{
	struct v92_anspcm signals[3];
	unsigned positions[3] = { 0 };

	for (unsigned i = 0; i < ARRAY_LENGTH(signals); ++i)
		CHECK(v92_anspcm_init(&signals[i], i == 1 ? G711_LAW_PCMA :
			      G711_LAW_PCMU, i), "interleaved init failed");
	for (unsigned tick = 0; tick < 24000; ++tick) {
		for (unsigned i = 0; i < ARRAY_LENGTH(signals); ++i) {
			struct v90_pcm_symbol symbol;
			unsigned pos = positions[i];
			uint8_t expected = signals[i].period[pos % V92_ANSPCM_PERIOD] ^
				((pos / V92_ANSPCM_REVERSAL_PERIOD) % 2u ? 0x80 : 0);

			if (tick % (i + 2u) != 0)
				continue;
			CHECK(v92_anspcm_sample(&signals[i], &symbol) &&
			      symbol.octet == expected,
			      "interleaved signal %u changed at %u", i, pos);
			positions[i]++;
		}
	}
	return true;
}

static bool test_copied_signal(void)
{
	static const unsigned positions[] = { 0, 300, 301, 3611, 3612, 7223 };

	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned level = 0; level < 4; ++level)
			for (size_t i = 0; i < ARRAY_LENGTH(positions); ++i) {
				struct v92_anspcm source, copy, control;
				struct v90_pcm_symbol output, expected;

				CHECK(v92_anspcm_init(&source, law, level) &&
				      v92_anspcm_init(&control, law, level), "copy init");
				for (unsigned k = 0; k < positions[i]; ++k)
					CHECK(v92_anspcm_sample(&source, &output) &&
					      v92_anspcm_sample(&control, &expected), "copy advance");
				copy = source;
				memset(&source, 0, sizeof(source));
				for (unsigned k = 0; k < 2 * V92_ANSPCM_REVERSAL_PERIOD; ++k)
					CHECK(v92_anspcm_sample(&copy, &output) &&
					      v92_anspcm_sample(&control, &expected) &&
					      output.octet == expected.octet &&
					      output.sample == expected.sample,
					      "copied ANSpcm changed at %u", k);
				CHECK(copy.position == positions[i], "copy lost bounded phase");
			}
	return true;
}

int main(void)
{
	const struct { const char *name; bool (*run)(void); } tests[] = {
		{ "QTS codewords and signed zeros", test_qts },
		{ "shared QTS acquisition and reversal", test_qts_receiver },
		{ "ANSpcm normative digests and reversals", test_anspcm },
		{ "signal validation", test_validation },
		{ "independent signal instances", test_independent_signals },
		{ "copied signal ownership", test_copied_signal },
	};

	for (size_t i = 0; i < ARRAY_LENGTH(tests); ++i) {
		printf("[ RUN      ] %s\n", tests[i].name);
		if (!tests[i].run())
			return EXIT_FAILURE;
		printf("[       OK ] %s\n", tests[i].name);
	}
	return EXIT_SUCCESS;
}
