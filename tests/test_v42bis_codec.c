// SPDX-License-Identifier: BSD-3-Clause

#include <modem.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

static bool initialize(struct modem *m, int codewords, int string_length)
{
	memset(m, 0, sizeof(*m));
	m->cfg.comp_dict_size = codewords;
	m->cfg.comp_max_string = string_length;
	CHECK(modem_comp_init(m) == 0, "codec allocation failed for %d/%d", codewords, string_length);
	return true;
}

/* V.42bis 7.5 packs the least significant bit of each codeword first. */
static void put_codeword(uint8_t *wire, size_t *bits, unsigned value, unsigned width)
{
	for (unsigned bit = 0; bit < width; ++bit, ++*bits)
		wire[*bits / 8] |= ((value >> bit) & 1U) << (*bits % 8);
}

static size_t compressed_vector(uint8_t wire[32], unsigned width, unsigned codeword)
{
	size_t bits = 16;

	memset(wire, 0, 32); /* Escape 0, ECM 0 precede the packed codewords. */
	for (unsigned current = 9; current < width; ++current)
		put_codeword(wire, &bits, 2, current); /* STEPUP */
	put_codeword(wire, &bits, codeword, width);
	put_codeword(wire, &bits, 1, width); /* FLUSH and zero padding. */
	return (bits + 7) / 8;
}

static bool decode_vector(struct modem *m, const uint8_t *wire, size_t length,
			 char *output, size_t *count, bool *error)
{
	*count = 0;
	*error = false;
	for (size_t index = 0; index < length; ++index) {
		char buffer[512];
		int n = modem_comp_decode(m, wire[index], buffer, sizeof(buffer));

		if (n < 0) {
			*error = true;
			return true;
		}
		CHECK(*count + (size_t)n <= 512, "test vector produced excess output");
		memcpy(output + *count, buffer, (size_t)n);
		*count += (size_t)n;
	}
	return true;
}

static bool test_codeword_width(void)
{
	static const unsigned dictionaries[] = { 512, 513, 768, 1024, 2048, 32768, 32769, 65535 };

	for (size_t index = 0; index < ARRAY_LENGTH(dictionaries); ++index) {
		struct modem m;
		unsigned width = 9;
		uint8_t wire[32];
		char output[512];
		size_t length, count, bits = 0;
		bool error;

		while ((1U << width) < dictionaries[index])
			++width;
		CHECK(initialize(&m, dictionaries[index], 6), "width fixture failed");
		/* Initial root codewords are character ordinal + 3 (6.2). */
		length = compressed_vector(wire, width, 'A' + 3);
		CHECK(decode_vector(&m, wire, length, output, &count, &error) &&
			      !error && count == 1 && output[0] == 'A' &&
			      m.comp.decoder.cw_size == width && m.comp.decoder.threshold == (1U << width),
		      "legal %u-bit codeword failed for dictionary %u", width, dictionaries[index]);
		memset(wire, 0, sizeof(wire));
		put_codeword(wire, &bits, 2, width); /* One STEPUP beyond N1 (5.8). */
		CHECK(decode_vector(&m, wire, (bits + 7) / 8, output, &count, &error) &&
			      error && count == 0 && m.comp.decoder.cw_size == width,
		      "decoder accepted excess codeword growth for dictionary %u", dictionaries[index]);
		modem_comp_exit(&m);
	}
	return true;
}

static bool test_invalid_codewords(void)
{
	static const unsigned dictionaries[] = { 512, 513, 768, 2048, 65535 };

	for (size_t index = 0; index < ARRAY_LENGTH(dictionaries); ++index) {
		unsigned width = 9;

		while ((1U << width) < dictionaries[index])
			++width;
		for (unsigned scenario = 0; scenario < 3; ++scenario) {
			struct modem m;
			uint8_t wire[32];
			char output[512];
			size_t length, count;
			bool error;
			unsigned codeword = scenario == 0 ? 259 : scenario == 1 ? 260 :
				(dictionaries[index] & (dictionaries[index] - 1)) ?
				 dictionaries[index] : dictionaries[index] - 1;

			CHECK(initialize(&m, dictionaries[index], 6), "invalid-codeword fixture failed");
			length = compressed_vector(wire, width, codeword);
			CHECK(decode_vector(&m, wire, length, output, &count, &error) && error && count == 0,
			      "decoder accepted free, empty or out-of-range codeword %u in dictionary %u",
			      codeword, dictionaries[index]);
			modem_comp_exit(&m);
		}
	}
	return true;
}

static bool round_trip(int codewords, int string_length, unsigned pattern)
{
	struct modem m;
	uint32_t random = 0x173e19ab;
	uint8_t input[131072];
	char wire[1024], output[131072];
	size_t payload_length = pattern == 2 ? sizeof(input) : sizeof(input) / 2;
	size_t received = 0;
	unsigned longest = 0;

	CHECK(initialize(&m, codewords, string_length), "round-trip fixture failed");
	CHECK(sizeof(m.comp.encoder.str_data) >= (size_t)string_length,
	      "codec accepted a string limit larger than its buffer");
	for (size_t index = 0; index <= payload_length; ++index) {
		int count;

		if (index == payload_length)
			count = modem_comp_flush_encoder(&m, wire, sizeof(wire));
		else {
			random = random * 1664525U + 1013904223U;
			input[index] = pattern == 1 ? 'A' : pattern == 2 ?
				(index < 65536 ? random >> 24 : 'A') :
				index % 1024 < 512 ? (uint8_t)(index % 19) : random >> 24;
			count = modem_comp_encode(&m, input[index], wire, sizeof(wire));
			if ((unsigned)m.comp.encoder.str_len > longest)
				longest = m.comp.encoder.str_len;
		}
		CHECK(count >= 0, "encoder failed at %zu for %d/%d", index, codewords, string_length);
		for (int byte = 0; byte < count; ++byte) {
			char decoded[512];
			int length = modem_comp_decode(&m, (uint8_t)wire[byte], decoded, sizeof(decoded));

			CHECK(length >= 0 && received + (size_t)length <= sizeof(output),
			      "decoder failed at %zu for %d/%d", index, codewords, string_length);
			memcpy(output + received, decoded, (size_t)length);
			received += (size_t)length;
		}
	}
	{
		char decoded[512];
		int length = modem_comp_flush_decoder(&m, decoded, sizeof(decoded));

		CHECK(length >= 0 && received + (size_t)length <= sizeof(output), "decoder flush failed");
		memcpy(output + received, decoded, (size_t)length);
		received += (size_t)length;
	}
	CHECK(received == payload_length && memcmp(input, output, payload_length) == 0,
	      "codec corrupted payload for %d/%d", codewords, string_length);
	if (pattern == 1 && string_length == 250)
		CHECK(longest >= 249, "250-byte-string case did not exercise the full string buffer");
	if (pattern == 2)
		CHECK(m.comp.encoder.cw_size == 16 && m.comp.decoder.cw_size == 16 &&
			      m.comp.encoder.threshold == 65536 && m.comp.decoder.threshold == 65536,
		      "large dictionary ended at widths %u/%u, thresholds %u/%u, next entry %u",
		      m.comp.encoder.cw_size, m.comp.decoder.cw_size,
		      m.comp.encoder.threshold, m.comp.decoder.threshold, m.comp.encoder.next_free);
	modem_comp_exit(&m);
	return true;
}

static bool test_full_string_capacity(void)
{
	CHECK(round_trip(512, 250, 1), "maximum string round trip failed");
	return true;
}

static bool test_payloads(void)
{
	static const int dictionaries[] = { 512, 513, 768, 2048, 32769, 65535 };
	static const int strings[] = { 6, 34, 250 };

	for (size_t dictionary = 0; dictionary < ARRAY_LENGTH(dictionaries); ++dictionary)
		for (size_t string = 0; string < ARRAY_LENGTH(strings); ++string)
			CHECK(round_trip(dictionaries[dictionary], strings[string], 0),
			      "mixed payload round trip failed");
	CHECK(round_trip(65535, 250, 2), "16-bit codeword payload round trip failed");
	return true;
}

static bool test_configuration_limits(void)
{
	struct modem m;

	CHECK(initialize(&m, 1024, 34), "configuration fixture failed");
	/* Distinct owned capacities must be checked before either side changes. */
	m.comp.decoder.dict_size = 512;
	CHECK(modem_comp_config(&m, 768, 12) < 0 &&
		      m.comp.encoder.dict_size == 1024 && m.comp.encoder.max_string == 34 &&
		      m.comp.decoder.dict_size == 512 && m.comp.decoder.max_string == 34,
	      "failed configuration partially changed the codec");
	CHECK(modem_comp_config(&m, 512, 250) == 0 &&
		      m.comp.encoder.dict_size == 512 && m.comp.decoder.dict_size == 512 &&
		      m.comp.encoder.max_string == 250 && m.comp.decoder.max_string == 250,
	      "codec could not select valid smaller limits");
	CHECK(modem_comp_config(&m, 512, 251) < 0 && m.comp.encoder.max_string == 250 &&
		      m.comp.decoder.max_string == 250, "invalid string limit changed the codec");
	modem_comp_exit(&m);
	memset(&m, 0, sizeof(m));
	m.cfg.comp_dict_size = 65536;
	m.cfg.comp_max_string = 6;
	CHECK(modem_comp_init(&m) < 0 && m.comp.encoder.dict == NULL && m.comp.decoder.dict == NULL,
	      "codec accepted a dictionary larger than its index type");
	return true;
}

static bool test_encoder_input_ownership(void)
{
	struct modem m;
	char output[4];

	CHECK(initialize(&m, 512, 34), "encoder ownership fixture failed");
	CHECK(modem_comp_read_encoder(&m, output, -1) < 0 &&
		      modem_comp_read_encoder(&m, NULL, 1) < 0 &&
		      modem_comp_read_encoder(&m, NULL, 0) == 0,
	      "encoder drain accepted an invalid output buffer");
	CHECK(modem_comp_encode(&m, 0, output, 0) < 0 && m.comp.encoder.str_len == 0,
	      "encoder consumed input without output space");
	CHECK(modem_comp_encode(&m, 0, output, 1) == 0 &&
		      modem_comp_encode(&m, 'B', output, 1) == 1 && output[0] == 0,
	      "encoder ownership escape fixture failed");
	CHECK(modem_comp_encode(&m, 'C', output, 1) < 0 &&
		      m.comp.encoder.str_len == 1 && m.comp.encoder.str_data[0] == 'B',
	      "encoder accepted input while earlier output remained queued");
	CHECK(modem_comp_read_encoder(&m, NULL, 0) == 0,
	      "zero-capacity encoder drain changed pending output");
	CHECK(modem_comp_read_encoder(&m, output, 1) == 1 && output[0] == 1 &&
		      modem_comp_encode(&m, 'C', output, 1) == 1 && output[0] == 'B' &&
		      modem_comp_flush_encoder(&m, output, sizeof(output)) == 1 && output[0] == 'C',
	      "encoder failed to resume after draining its output");
	modem_comp_exit(&m);
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} cases[] = {
		{ "V.42bis codeword width bounds", test_codeword_width },
		{ "V.42bis invalid dictionary codewords", test_invalid_codewords },
		{ "V.42bis full string capacity", test_full_string_capacity },
		{ "V.42bis mixed payloads", test_payloads },
		{ "V.42bis transactional configuration", test_configuration_limits },
		{ "V.42bis encoder input ownership", test_encoder_input_ownership },
	};
	unsigned failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", cases[index].name);
		if (cases[index].run())
			fprintf(stderr, "[       OK ] %s\n", cases[index].name);
		else {
			fprintf(stderr, "[  FAILED  ] %s\n", cases[index].name);
			++failures;
		}
	}
	return failures ? 1 : 0;
}
