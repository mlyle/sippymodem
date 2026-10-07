// SPDX-License-Identifier: BSD-3-Clause

#include <v90_pcm_frame_decoder.h>
#include <v90_pcm_frame_encoder.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	ROUND_TRIP_FRAMES = 16,
	MAX_RECORDED_RULES = ROUND_TRIP_FRAMES * 3,
};

struct frame_collector {
	struct v90_pcm_frame_decoder decoder;
	enum v90_pcm_mapping_plane plane;
	uint8_t ucodes[V90_CP_FRAME_INTERVALS];
	uint8_t signs[V90_CP_FRAME_INTERVALS];
	size_t symbol_count;
	size_t frame_count;
	size_t rule_count;
};

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static bool make_mapping(
	struct v90_pcm_mapping *mapping, enum v90_cp_kind kind,
	enum g711_law law, uint8_t redundancy, uint8_t lookahead)
{
	uint8_t size = kind == V90_CP_TRAINING ? 4 : 8;
	struct v90_cp_parameters parameters = {
		.kind = kind,
		.law = law,
		.shaping_a1_q1_6 = 17,
		.shaping_a2_q1_6 = -23,
		.shaping_b1_q1_6 = 39,
		.shaping_b2_q1_6 = -7,
		.selected_rate_index = kind == V90_CP_TRAINING ? 4 : 1,
		.sign_redundancy = redundancy,
		.spectral_lookahead = lookahead,
		.constellation_count = 1,
		.codec_constellations_differ = true,
	};

	for (uint8_t ordinal = 0; ordinal < size; ++ordinal) {
		add_ucode(
			&parameters.transmit_constellations[0],
			(uint8_t)(12u + ordinal));
		add_ucode(
			&parameters.codec_constellations[0],
			(uint8_t)(72u + ordinal));
	}
	return v90_pcm_mapping_init(mapping, &parameters) ==
		       V90_PCM_MAPPING_OK &&
	       mapping->codec_plane_differs;
}

static void generate_bits(
	uint8_t *bits, size_t count, size_t frame,
	enum g711_law law, uint8_t redundancy, uint8_t lookahead)
{
	uint32_t state = UINT32_C(0x9e3779b9) ^
			 (uint32_t)(frame * 0x45d9f3bu) ^
			 (uint32_t)(law * 0x101u +
				    redundancy * 0x21u + lookahead);

	for (size_t bit = 0; bit < count; ++bit) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		bits[bit] = (uint8_t)(state & 1u);
	}
}

static bool output_is_canonical(
	const struct v90_pcm_frame_decode_output *output,
	size_t bit_count, size_t rule_count)
{
	if (output->bit_count != bit_count ||
	    output->rule_count != rule_count)
		return false;
	for (size_t bit = bit_count;
	     bit < ARRAY_LENGTH(output->bits); ++bit) {
		if (output->bits[bit] != 0)
			return false;
	}
	for (size_t rule = rule_count;
	     rule < ARRAY_LENGTH(output->rules); ++rule) {
		if (output->rules[rule] != V90_SPECTRAL_RULE_A)
			return false;
	}
	return true;
}

static bool collector_push(
	struct frame_collector *collector,
	const struct v90_pcm_mapping *mapping,
	const struct v90_pcm_symbol *symbol,
	const uint8_t expected_bits[ROUND_TRIP_FRAMES]
		[V90_PCM_FRAME_MAX_BITS],
	size_t expected_frame_count,
	const enum v90_spectral_rule expected_rules[MAX_RECORDED_RULES],
	size_t expected_rule_count)
{
	struct v90_pcm_symbol decision;
	struct v90_pcm_symbol parsed;
	struct v90_pcm_frame_decode_output decoded;
	size_t interval = collector->symbol_count;
	uint8_t label;
	uint8_t ucode;

	if (interval >= V90_CP_FRAME_INTERVALS ||
	    v90_pcm_mapping_label(
		    mapping, V90_PCM_MAPPING_TRANSMIT, interval,
		    symbol->ucode, &label) != V90_PCM_MAPPING_OK ||
	    v90_pcm_mapping_ucode(
		    mapping, collector->plane, interval, label,
		    &ucode) != V90_PCM_MAPPING_OK ||
	    !v90_pcm_symbol_make(
		    &decision, mapping->law, ucode, symbol->positive) ||
	    !v90_pcm_symbol_parse(&parsed, mapping->law, decision.octet))
		return false;
	collector->ucodes[interval] = parsed.ucode;
	collector->signs[interval] = parsed.positive ? 1u : 0u;
	collector->symbol_count++;
	if (collector->symbol_count != V90_CP_FRAME_INTERVALS)
		return true;
	if (collector->frame_count >= expected_frame_count ||
	    v90_pcm_frame_decoder_decode(
		    &collector->decoder, collector->ucodes,
		    collector->signs, collector->symbol_count,
		    &decoded) != V90_PCM_FRAME_DECODER_OK ||
	    !output_is_canonical(
		    &decoded, mapping->frame_bits,
		    mapping->sign_redundancy) ||
	    memcmp(decoded.bits, expected_bits[collector->frame_count],
		   mapping->frame_bits) != 0 ||
	    collector->rule_count + decoded.rule_count >
		    expected_rule_count ||
	    memcmp(decoded.rules,
		   expected_rules + collector->rule_count,
		   decoded.rule_count * sizeof(decoded.rules[0])) != 0)
		return false;
	collector->symbol_count = 0;
	collector->frame_count++;
	collector->rule_count += decoded.rule_count;
	return true;
}

static bool collect_output(
	struct frame_collector collectors[2],
	const struct v90_pcm_mapping *mapping,
	const struct v90_pcm_frame_output *output,
	const uint8_t expected_bits[ROUND_TRIP_FRAMES]
		[V90_PCM_FRAME_MAX_BITS],
	size_t expected_frame_count,
	enum v90_spectral_rule expected_rules[MAX_RECORDED_RULES],
	size_t *expected_rule_count)
{
	if (*expected_rule_count + output->rule_count >
	    MAX_RECORDED_RULES)
		return false;
	memcpy(expected_rules + *expected_rule_count, output->rules,
	       output->rule_count * sizeof(output->rules[0]));
	*expected_rule_count += output->rule_count;
	for (size_t symbol = 0; symbol < output->symbol_count; ++symbol) {
		for (size_t collector = 0; collector < 2; ++collector) {
			if (!collector_push(
				    &collectors[collector], mapping,
				    &output->symbols[symbol], expected_bits,
				    expected_frame_count, expected_rules,
				    *expected_rule_count))
				return false;
		}
	}
	return true;
}

static bool run_round_trip(
	enum v90_cp_kind kind, enum g711_law law,
	uint8_t redundancy, uint8_t lookahead)
{
	struct v90_pcm_mapping mapping;
	struct v90_pcm_frame_encoder encoder;
	struct frame_collector collectors[2] = {
		{ .plane = V90_PCM_MAPPING_TRANSMIT },
		{ .plane = V90_PCM_MAPPING_CODEC },
	};
	uint8_t expected_bits[ROUND_TRIP_FRAMES]
		[V90_PCM_FRAME_MAX_BITS] = { 0 };
	enum v90_spectral_rule expected_rules[MAX_RECORDED_RULES] = { 0 };
	size_t expected_rule_count = 0;

	CHECK(make_mapping(
		      &mapping, kind, law, redundancy, lookahead) &&
	      v90_pcm_frame_encoder_init(&encoder, &mapping) ==
		      V90_PCM_FRAME_ENCODER_OK,
	      "encoder fixture did not initialize");
	for (size_t index = 0; index < ARRAY_LENGTH(collectors); ++index) {
		CHECK(v90_pcm_frame_decoder_init(
			      &collectors[index].decoder, &mapping,
			      collectors[index].plane) ==
			      V90_PCM_FRAME_DECODER_OK,
		      "decoder plane %zu did not initialize", index);
	}
	for (size_t frame = 0; frame < ROUND_TRIP_FRAMES; ++frame) {
		struct v90_pcm_frame_output output;
		enum v90_pcm_frame_encoder_result result;

		generate_bits(
			expected_bits[frame], mapping.frame_bits, frame,
			law, redundancy, lookahead);
		result = v90_pcm_frame_encoder_process(
			&encoder, expected_bits[frame], mapping.frame_bits,
			&output);
		CHECK(result == V90_PCM_FRAME_ENCODER_BUFFERING ||
		      result == V90_PCM_FRAME_ENCODER_SYMBOLS_READY,
		      "frame %zu returned %d", frame,
		      (int)result);
		if (result == V90_PCM_FRAME_ENCODER_SYMBOLS_READY)
			CHECK(collect_output(
				      collectors, &mapping, &output,
				      expected_bits, frame + 1u,
				      expected_rules, &expected_rule_count),
			      "frame %zu did not round trip", frame);
	}
	for (;;) {
		struct v90_pcm_frame_output output;
		enum v90_pcm_frame_encoder_result result =
			v90_pcm_frame_encoder_finish(&encoder, &output);

		if (result == V90_PCM_FRAME_ENCODER_COMPLETE)
			break;
		CHECK(result == V90_PCM_FRAME_ENCODER_SYMBOLS_READY &&
		      collect_output(
			      collectors, &mapping, &output, expected_bits,
			      ROUND_TRIP_FRAMES, expected_rules,
			      &expected_rule_count),
		      "encoder tail did not round trip");
	}
	for (size_t index = 0; index < ARRAY_LENGTH(collectors); ++index) {
		CHECK(collectors[index].symbol_count == 0 &&
		      collectors[index].frame_count == ROUND_TRIP_FRAMES &&
		      collectors[index].rule_count == expected_rule_count &&
		      v90_pcm_frame_decoder_is_valid(
			      &collectors[index].decoder),
		      "decoder plane %zu lost stream state", index);
	}
	CHECK(expected_rule_count == ROUND_TRIP_FRAMES * redundancy,
	      "rule stream length changed");
	return true;
}

static bool test_generated_round_trips(void)
{
	for (unsigned int kind = V90_CP_TRAINING;
	     kind <= V90_CP_DATA; ++kind) {
		for (unsigned int law = G711_LAW_PCMU;
		     law <= G711_LAW_PCMA; ++law) {
			for (uint8_t redundancy = 0;
			     redundancy <= 3; ++redundancy) {
				for (uint8_t lookahead = 0;
				     lookahead <= 3; ++lookahead) {
					CHECK(run_round_trip(
					      (enum v90_cp_kind)kind,
					      (enum g711_law)law,
					      redundancy, lookahead),
					      "round trip failed for %u/%u/%u/%u",
					      kind, law, redundancy, lookahead);
				}
			}
		}
	}
	return true;
}

static bool labels_to_ucodes(
	const struct v90_pcm_mapping *mapping,
	enum v90_pcm_mapping_plane plane,
	const uint8_t labels[V90_CP_FRAME_INTERVALS],
	uint8_t ucodes[V90_CP_FRAME_INTERVALS])
{
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		if (v90_pcm_mapping_ucode(
			    mapping, plane, interval, labels[interval],
			    &ucodes[interval]) != V90_PCM_MAPPING_OK)
			return false;
	}
	return true;
}

static bool test_contracts(void)
{
	struct v90_pcm_mapping mapping;
	struct v90_pcm_frame_decoder decoder;
	struct v90_pcm_frame_decoder saved_decoder;
	union {
		struct v90_pcm_frame_decoder decoder;
		struct v90_pcm_frame_decode_output output;
	} aliased;
	struct v90_pcm_frame_decoder untouched;
	struct v90_pcm_frame_decoder saved_untouched;
	struct v90_pcm_frame_decode_output output;
	struct v90_pcm_frame_decode_output saved_output;
	uint8_t labels[V90_CP_FRAME_INTERVALS] = { 0 };
	uint8_t ucodes[V90_CP_FRAME_INTERVALS];
	uint8_t signs[V90_CP_FRAME_INTERVALS] = { 0 };

	CHECK(make_mapping(
		      &mapping, V90_CP_DATA, G711_LAW_PCMU, 0, 0),
	      "contract mapping did not initialize");
	memset(&untouched, 0xa5, sizeof(untouched));
	saved_untouched = untouched;
	CHECK(v90_pcm_frame_decoder_init(
		      &untouched, &mapping,
		      (enum v90_pcm_mapping_plane)99) ==
		      V90_PCM_FRAME_DECODER_INVALID_ARGUMENT &&
	      memcmp(&untouched, &saved_untouched,
		     sizeof(untouched)) == 0,
	      "rejected plane changed decoder destination");
	mapping.modulus[0] = 0;
	CHECK(v90_pcm_frame_decoder_init(
		      &untouched, &mapping,
		      V90_PCM_MAPPING_TRANSMIT) ==
		      V90_PCM_FRAME_DECODER_INVALID_PARAMETERS &&
	      memcmp(&untouched, &saved_untouched,
		     sizeof(untouched)) == 0,
	      "rejected mapping changed decoder destination");
	CHECK(make_mapping(
		      &mapping, V90_CP_DATA, G711_LAW_PCMU, 0, 0) &&
	      v90_pcm_frame_decoder_init(
		      &decoder, &mapping, V90_PCM_MAPPING_CODEC) ==
		      V90_PCM_FRAME_DECODER_OK &&
	      labels_to_ucodes(
		      &mapping, V90_PCM_MAPPING_CODEC, labels, ucodes),
	      "contract decoder did not initialize");
	memset(&output, 0x5a, sizeof(output));
	saved_output = output;
	saved_decoder = decoder;
	aliased.decoder = decoder;
	CHECK(v90_pcm_frame_decoder_decode(
		      &aliased.decoder, ucodes, signs, ARRAY_LENGTH(signs),
		      &aliased.output) == V90_PCM_FRAME_DECODER_INVALID_ARGUMENT &&
	      memcmp(&aliased.decoder, &decoder, sizeof(decoder)) == 0,
	      "overlapping output changed frame decoder state");
	signs[2] = 2;
	CHECK(v90_pcm_frame_decoder_decode(
		      &decoder, ucodes, signs, ARRAY_LENGTH(signs),
		      &output) == V90_PCM_FRAME_DECODER_INVALID_SYMBOLS,
	      "non-binary sign was accepted");
	signs[2] = 0;
	ucodes[1] = 0;
	CHECK(v90_pcm_frame_decoder_decode(
		      &decoder, ucodes, signs, ARRAY_LENGTH(signs),
		      &output) == V90_PCM_FRAME_DECODER_INVALID_SYMBOLS &&
	      memcmp(&decoder, &saved_decoder, sizeof(decoder)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0,
	      "absent Ucode was accepted");
	CHECK(labels_to_ucodes(
		      &mapping, V90_PCM_MAPPING_CODEC, labels, ucodes),
	      "valid Ucodes could not be restored");
	CHECK(v90_pcm_frame_decoder_decode(
		      &decoder, ucodes, signs, ARRAY_LENGTH(signs) - 1u,
		      &output) == V90_PCM_FRAME_DECODER_INVALID_SYMBOLS &&
	      memcmp(&decoder, &saved_decoder, sizeof(decoder)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0,
	      "short frame was accepted");
	memset(labels, 7, sizeof(labels));
	CHECK(labels_to_ucodes(
		      &mapping, V90_PCM_MAPPING_CODEC, labels, ucodes) &&
	      v90_pcm_frame_decoder_decode(
		      &decoder, ucodes, signs, ARRAY_LENGTH(signs),
		      &output) == V90_PCM_FRAME_DECODER_OK &&
	      output.bit_count == mapping.frame_bits,
	      "unused received tuple stopped the frame clock");
	for (size_t bit = mapping.sign_bits; bit < output.bit_count; ++bit)
		CHECK(output.bits[bit] == 1u,
		      "unused tuple did not retain its low modulus bits");
	saved_output = output;
	CHECK(v90_pcm_frame_decoder_decode(
		      NULL, ucodes, signs, ARRAY_LENGTH(signs), &output) ==
		      V90_PCM_FRAME_DECODER_INVALID_ARGUMENT &&
	      v90_pcm_frame_decoder_decode(
		      &decoder, NULL, signs, ARRAY_LENGTH(signs), &output) ==
		      V90_PCM_FRAME_DECODER_INVALID_ARGUMENT &&
	      v90_pcm_frame_decoder_decode(
		      &decoder, ucodes, NULL, ARRAY_LENGTH(signs), &output) ==
		      V90_PCM_FRAME_DECODER_INVALID_ARGUMENT &&
	      v90_pcm_frame_decoder_decode(
		      &decoder, ucodes, signs, ARRAY_LENGTH(signs), NULL) ==
		      V90_PCM_FRAME_DECODER_INVALID_ARGUMENT,
	      "null decode argument was accepted");
	decoder.mapping.modulus[0] = 0;
	CHECK(!v90_pcm_frame_decoder_is_valid(&decoder) &&
	      !v90_pcm_frame_decoder_is_valid(NULL) &&
	      v90_pcm_frame_decoder_decode(
		      &decoder, ucodes, signs, ARRAY_LENGTH(signs),
		      &output) == V90_PCM_FRAME_DECODER_INVALID_STATE &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0 &&
	      v90_pcm_frame_decoder_init(
		      NULL, &mapping, V90_PCM_MAPPING_TRANSMIT) ==
		      V90_PCM_FRAME_DECODER_INVALID_ARGUMENT,
	      "state contract changed");
	return true;
}

/* Independent scalar modulus/sign calculation, including both G.711 zeros.
 * V.91 reuses the coder, but cannot relax the V.90 wire-record rate limits. */
static bool test_v91_frames(void)
{
	uint32_t random = 0x12345678u;
	for (unsigned law = 0; law < 2; ++law) {
		for (unsigned bits = 21; bits <= PCM_FRAME_MAX_BITS; ++bits) {
			struct pcm_constellation sets[6];
			struct v90_pcm_mapping mapping, saved;
			struct v90_pcm_frame_encoder encoder;
			struct v90_pcm_frame_decoder decoder;
			bool seen[256] = { false };
			uint8_t previous = 0;
			memset(sets, 0xff, sizeof(sets));
			CHECK(v90_pcm_mapping_init_v91(&mapping, law, bits, sets) ==
			      V90_PCM_MAPPING_OK &&
			      v90_pcm_frame_encoder_init(&encoder, &mapping) ==
			      V90_PCM_FRAME_ENCODER_OK &&
			      v90_pcm_frame_decoder_init(&decoder, &mapping,
			      V90_PCM_MAPPING_TRANSMIT) == V90_PCM_FRAME_DECODER_OK,
			      "V.91 frame initialization failed at %u bits", bits);
			for (unsigned frame = 0; frame < 1024; ++frame) {
				uint8_t source[PCM_FRAME_MAX_BITS], ucodes[6], signs[6];
				struct v90_pcm_frame_output encoded;
				struct v90_pcm_frame_decode_output decoded;
				uint64_t value = 0;
				for (unsigned bit = 0; bit < bits; ++bit) {
					random ^= random << 13;
					random ^= random >> 17;
					random ^= random << 5;
					source[bit] = random & 1u;
					if (bit >= 6) value |= (uint64_t)source[bit] << (bit - 6);
				}
				CHECK(v90_pcm_frame_encoder_process(&encoder, source, bits,
				      &encoded) == V90_PCM_FRAME_ENCODER_SYMBOLS_READY &&
				      encoded.symbol_count == 6 && encoded.rule_count == 0,
				      "V.91 encode failed");
				for (unsigned i = 0; i < 6; ++i) {
					struct v90_pcm_symbol parsed;
					previous ^= source[i];
					CHECK(encoded.symbols[i].ucode == 127u - (value % 128u) &&
					      encoded.symbols[i].positive == (previous != 0),
					      "V.91 differs from scalar modulus/sign mapping");
					value /= 128;
					seen[encoded.symbols[i].octet] = true;
					CHECK(v90_pcm_symbol_parse(&parsed, law, encoded.symbols[i].octet),
					      "network octet rejected");
					ucodes[i] = parsed.ucode;
					signs[i] = parsed.positive;
				}
				CHECK(v90_pcm_frame_decoder_decode(&decoder, ucodes, signs, 6,
				      &decoded) == V90_PCM_FRAME_DECODER_OK &&
				      decoded.bit_count == bits && memcmp(source, decoded.bits, bits) == 0,
				      "V.91 payload differs at %u bits", bits);
			}
			if (bits == PCM_FRAME_MAX_BITS)
				for (unsigned octet = 0; octet < 256; ++octet)
					CHECK(seen[octet], "64k test missed octet %u", octet);
			saved = mapping;
			CHECK(v90_pcm_mapping_init_v91(&mapping, law, 49, sets) ==
			      V90_PCM_MAPPING_INVALID_PARAMETERS &&
			      memcmp(&mapping, &saved, sizeof(mapping)) == 0,
			      "invalid V.91 rate changed destination");
			mapping.format = PCM_MAPPING_V90;
			if (bits > V90_PCM_FRAME_MAX_BITS)
				CHECK(!v90_pcm_mapping_is_valid(&mapping), "V.90 accepted >56k");
			mapping = saved;
			mapping.sign_redundancy = 1;
			mapping.sign_bits = 5;
			mapping.modulus_bits++;
			CHECK(!v90_pcm_mapping_is_valid(&mapping), "V.91 accepted shaping");
			memset(sets, 0, sizeof(sets));
			CHECK(v90_pcm_mapping_init_v91(&mapping, law, bits, sets) ==
			      V90_PCM_MAPPING_INVALID_PARAMETERS, "empty mapping accepted");
		}
	}
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "generated round trips", test_generated_round_trips },
		{ "contracts", test_contracts },
		{ "V.91 shared frames", test_v91_frames },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		printf("[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			printf("[       OK ] %s\n", tests[index].name);
		else {
			printf("[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
