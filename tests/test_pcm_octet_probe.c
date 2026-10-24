// SPDX-License-Identifier: BSD-3-Clause
#include "v91_profile.h"
#include "v90_dil_sequence.h"
#include "v90_pcm_frame_encoder.h"
#include "v90_pcm_frame_decoder.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

static bool learn(const struct pcm_octet_probe *probe, enum g711_law law,
	uint32_t maximum_power, struct v90_pcm_mapping *mapping, struct pcm_octet_channel *channel)
{
	struct v91_profile_limits limits = { .maximum_power = maximum_power, .maximum_frame_bits = 48 };
	return v91_profile_learn(probe, law, &limits, mapping, channel);
}

static uint8_t impairment(unsigned kind, enum g711_law law, unsigned interval,
	unsigned epoch, uint8_t octet)
{
	switch (kind) {
	case 1: return (octet & ~1u) | (epoch & 1u);
	case 2: return interval == 0 ? (octet & ~1u) | (epoch & 1u) : octet;
	case 3: return g711_encode(law, g711_decode(law, octet) / 2);
	case 4: return g711_encode(law == G711_LAW_PCMU ? G711_LAW_PCMA : G711_LAW_PCMU,
		g711_decode(law, octet));
	case 5: return octet ^ 0x80u;
	case 6: return 0xff;
	case 7: return interval == 3 && (epoch % 4 == 1 || epoch % 4 == 2) ?
		(octet & ~1u) | (epoch & 1u) : octet;
	case 8: return (g711_encode(law, g711_decode(law, octet) / 2) & ~1u) |
		(epoch & 1u);
	case 9:
	case 10:
		if (interval == 3) {
			/* Four stationary signaling positions, with a pad between
			 * the input and output robbed-bit operations. */
			if (epoch % 4 != 0)
				octet = (octet & ~1u) | (epoch % 4 == 2);
			octet = g711_encode(law, g711_decode(law, octet) / 2);
			if (kind == 10 && epoch % 4 != 2)
				octet = (octet & ~1u) | (epoch % 4 != 1);
			return octet;
		}
		return g711_encode(law, g711_decode(law, octet) / 2);
	default: return octet;
	}
}

static bool test_training_coverage(enum g711_law law)
{
	struct v90_dil_parameters dil;
	struct v90_dil_sequence sequence;
	uint8_t occurrences[24][256] = { 0 };
	uint32_t length;

	CHECK(v91_profile_dil(&dil) &&
	      v90_dil_training_symbol_count(&dil, &length) && length == 6144 &&
	      v90_dil_sequence_init(&sequence, &dil, law) == V90_DIL_SEQUENCE_OK,
	      "coverage DIL initialization failed");
	for (unsigned n = 0; n < length; ++n) {
		struct v90_pcm_symbol symbol;
		CHECK(v90_dil_sequence_next(&sequence, &symbol) == V90_DIL_SEQUENCE_OK,
		      "coverage DIL generation failed");
		CHECK(occurrences[n % 24][symbol.octet]++ == 0,
		      "DIL repeats an octet before covering the 24-position cycle");
	}
	for (unsigned position = 0; position < 24; ++position)
		for (unsigned octet = 0; octet < 256; ++octet)
			CHECK(occurrences[position][octet] == 1,
			      "DIL misses law=%u position=%u octet=%u", law, position, octet);
	return true;
}

static bool test_channel(unsigned kind, enum g711_law law)
{
	struct pcm_octet_probe *probe = malloc(sizeof(*probe));
	struct pcm_octet_channel channel;
	struct pcm_constellation sets[6];
	struct v90_pcm_mapping mapping;
	struct v90_pcm_frame_encoder tx;
	struct v90_pcm_frame_decoder rx;
	CHECK(probe != NULL, "allocation failed");
	pcm_octet_probe_init(probe);
	CHECK(!pcm_octet_probe_finish(probe, law, sets, &channel), "empty probe qualified");
	struct v90_dil_parameters dil;
	struct v90_dil_sequence sequence;
	uint32_t length;
	CHECK(v91_profile_dil(&dil) && v90_dil_training_symbol_count(&dil, &length) &&
	      length == 6144 && v90_dil_sequence_init(&sequence, &dil, law) ==
	      V90_DIL_SEQUENCE_OK, "DIL initialization failed");
	for (unsigned n = 0; n < length; ++n) {
		struct v90_pcm_symbol symbol;
		CHECK(v90_dil_sequence_next(&sequence, &symbol) == V90_DIL_SEQUENCE_OK &&
		      pcm_octet_probe_observe(probe, n % 6, symbol.octet,
			impairment(kind, law, n % 6, n / 6, symbol.octet)),
		      "DIL observation failed");
	}
	if (kind == 6) {
		CHECK(!pcm_octet_probe_finish(probe, law, sets, &channel),
		      "constant channel invented distinguishable symbols");
		free(probe);
		return true;
	}
	CHECK(learn(probe, law, UINT32_MAX, &mapping, &channel),
	      "no usable inferred rate");
	free(probe);
	unsigned bits = mapping.frame_bits;
	if (kind == 0 || kind == 5) CHECK(bits == 48, "clear channel lost capacity");
	if (kind == 1) CHECK(bits == 42, "robbed-bit channel did not yield 56k");
	if (kind == 2) CHECK(bits == 47, "periodic robbed bits did not yield 62.667k");
	/* Check every retained code, both signs, at every signaling position.
	 * Random payloads alone need not exercise the rare output of a pad
	 * combined with input and output robbed bits. */
	for (unsigned interval = 0; interval < 6; ++interval)
		for (unsigned code = 0; code < 128; ++code) {
			if (!pcm_constellation_contains(&mapping.transmit[interval], code))
				continue;
			for (unsigned sign = 0; sign < 2; ++sign)
				for (unsigned epoch = 0; epoch < 4; ++epoch) {
					uint8_t sent, normalized;
					CHECK(g711_ucode_octet(law, code, sign != 0, &sent) &&
					      pcm_octet_channel_normalize(&channel, interval,
						impairment(kind, law, interval, epoch, sent), &normalized) &&
					      normalized == sent,
					      "uncovered output kind=%u law=%u interval=%u code=%u sign=%u epoch=%u",
					      kind, law, interval, code, sign, epoch);
				}
		}
	CHECK(v90_pcm_frame_encoder_init(&tx, &mapping) == V90_PCM_FRAME_ENCODER_OK &&
	      v90_pcm_frame_decoder_init(&rx, &mapping, V90_PCM_MAPPING_TRANSMIT) ==
	      V90_PCM_FRAME_DECODER_OK, "coder initialization failed");
	uint32_t random = 0x18238517;
	for (unsigned frame = 0; frame < 512; ++frame) {
		uint8_t input[48], ucodes[6], signs[6];
		struct v90_pcm_frame_output encoded;
		struct v90_pcm_frame_decode_output decoded;
		for (unsigned bit = 0; bit < bits; ++bit) {
			random ^= random << 13; random ^= random >> 17; random ^= random << 5;
			input[bit] = random & 1;
		}
		CHECK(v90_pcm_frame_encoder_process(&tx, input, bits, &encoded) ==
		      V90_PCM_FRAME_ENCODER_SYMBOLS_READY, "encode failed");
		for (unsigned i = 0; i < 6; ++i) {
			uint8_t normalized;
			struct v90_pcm_symbol symbol;
			CHECK(pcm_octet_channel_normalize(&channel, i,
			      impairment(kind, law, i, frame, encoded.symbols[i].octet), &normalized) &&
			      v90_pcm_symbol_parse(&symbol, law, normalized), "normalization failed");
			ucodes[i] = symbol.ucode; signs[i] = symbol.positive;
		}
		CHECK(v90_pcm_frame_decoder_decode(&rx, ucodes, signs, 6, &decoded) ==
		      V90_PCM_FRAME_DECODER_OK && memcmp(input, decoded.bits, bits) == 0,
		      "impaired payload differs kind=%u law=%u frame=%u", kind, law, frame);
	}
	printf("channel=%u law=%u frame_bits=%u passed\n", kind, law, bits);
	return true;
}
static bool test_contracts(void)
{
	struct pcm_octet_probe *probe = calloc(1, sizeof(*probe));
	struct pcm_octet_channel channel, saved_channel;
	struct v90_pcm_mapping mapping, saved_mapping;
	struct pcm_constellation sets[6];
	uint8_t result = 37;

	CHECK(probe != NULL, "allocation failed");
	memset(&channel, 0x51, sizeof(channel));
	memset(&mapping, 0x52, sizeof(mapping));
	saved_channel = channel;
	saved_mapping = mapping;
	CHECK(!pcm_octet_probe_observe(NULL, 0, 0, 0) &&
	      !pcm_octet_probe_observe(probe, 6, 0, 0) &&
	      !learn(probe, G711_LAW_PCMU, UINT32_MAX, &mapping, &channel) &&
	      memcmp(&mapping, &saved_mapping, sizeof(mapping)) == 0 &&
	      memcmp(&channel, &saved_channel, sizeof(channel)) == 0,
	      "invalid observation or failed learning changed state");
	for (unsigned i = 0; i < 6; ++i)
		for (unsigned octet = 0; octet < 256; ++octet)
			CHECK(pcm_octet_probe_observe(probe, i, octet, octet), "observation failed");
	CHECK(!pcm_octet_probe_finish(probe, G711_LAW_PCMU, sets, &channel),
	      "single observation selected unproven codes");
	for (unsigned i = 0; i < 6; ++i)
		for (unsigned octet = 0; octet < 256; ++octet)
			CHECK(pcm_octet_probe_observe(probe, i, octet, octet), "observation failed");
	CHECK(learn(probe, G711_LAW_PCMU, 1000000, &mapping, &channel),
	      "power-limited rate was not inferred");
	uint32_t floor, ceiling;
	CHECK(mapping.frame_bits < 48 &&
	      v90_pcm_mapping_power(&mapping, V90_PCM_MAPPING_TRANSMIT,
		G711_LAW_PCMU, &floor, &ceiling) == V90_PCM_MAPPING_OK && ceiling <= 1000000,
	      "selected profile exceeds requested power");
	struct v91_profile_limits limits = {
		.maximum_power = UINT32_MAX, .maximum_frame_bits = 41,
		.receive_law = G711_LAW_PCMU,
	};
	CHECK(v91_profile_learn(probe, G711_LAW_PCMU, &limits, &mapping, &channel) &&
	      mapping.frame_bits == 41, "receive rate cap was ignored");
	limits.maximum_frame_bits = 48;
	limits.maximum_power = 1000000;
	limits.receiver_power = true;
	CHECK(v91_profile_learn(probe, G711_LAW_PCMU, &limits, &mapping, &channel),
	      "receiver power bound rejected usable channel");
	for (unsigned interval = 0; interval < 6; ++interval)
		for (unsigned code = 0; code < 128; ++code) {
			if (!pcm_constellation_contains(&mapping.transmit[interval], code))
				continue;
			int16_t sample;
			CHECK(g711_ucode_sample(G711_LAW_PCMU, code, true, &sample) &&
			      (uint32_t)((int32_t)sample * sample) <= limits.maximum_power,
			      "receiver power bound admitted an excessive codeword");
		}
	saved_channel = channel;
	saved_mapping = mapping;
	CHECK(!learn(probe, G711_LAW_PCMU, 1, &mapping, &channel) &&
	      memcmp(&mapping, &saved_mapping, sizeof(mapping)) == 0 &&
	      memcmp(&channel, &saved_channel, sizeof(channel)) == 0,
	      "unusable channel changed accepted profile");
	limits.maximum_frame_bits = 20;
	CHECK(!v91_profile_learn(probe, G711_LAW_PCMU, &limits, &mapping, &channel),
	      "invalid rate limit accepted");
	pcm_octet_probe_init(probe);
	CHECK(!pcm_octet_probe_finish(probe, G711_LAW_PCMU, sets, &channel),
	      "previous call observations survived reset");
	memset(&channel, 0, sizeof(channel));
	CHECK(!pcm_octet_channel_normalize(&channel, 0, 0, &result) && result == 37 &&
	      !pcm_octet_channel_normalize(&channel, 6, 0, &result),
	      "unknown codeword silently normalized");
	free(probe);
	return true;
}

int main(void)
{
	if (!test_contracts())
		return 1;
	for (unsigned law = 0; law < 2; ++law) {
		if (!test_training_coverage(law))
			return 1;
		for (unsigned kind = 0; kind <= 10; ++kind)
			if (!test_channel(kind, law))
				return 1;
	}
	return 0;
}
