// SPDX-License-Identifier: BSD-3-Clause

#include <v90_pcm_stream_rx.h>
#include <pcm_downstream_tx.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

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
		.shaping_a1_q1_6 = 29,
		.shaping_a2_q1_6 = -13,
		.shaping_b1_q1_6 = 43,
		.shaping_b2_q1_6 = -5,
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

static bool make_plane_octet(
	const struct v90_pcm_mapping *mapping,
	const struct pcm_downstream_symbol *source,
	enum v90_pcm_mapping_plane plane, size_t interval,
	uint8_t *octet)
{
	struct v90_pcm_symbol decision;
	uint8_t label;
	uint8_t ucode;

	if (v90_pcm_mapping_label(
		    mapping, V90_PCM_MAPPING_TRANSMIT, interval,
		    source->pcm.ucode, &label) != V90_PCM_MAPPING_OK ||
	    v90_pcm_mapping_ucode(
		    mapping, plane, interval, label, &ucode) !=
		    V90_PCM_MAPPING_OK ||
	    !v90_pcm_symbol_make(
		    &decision, mapping->law, ucode, source->pcm.positive))
		return false;
	*octet = decision.octet;
	return true;
}

static bool frame_is_expected(
	const struct v90_pcm_stream_frame *frame,
	const struct v90_pcm_mapping *mapping,
	const enum v90_spectral_rule expected_rules[3])
{
	if (frame->bit_count != mapping->frame_bits ||
	    frame->rule_count != mapping->sign_redundancy ||
	    (expected_rules != NULL && memcmp(frame->rules, expected_rules,
		   frame->rule_count * sizeof(frame->rules[0])) != 0))
		return false;
	for (size_t bit = 0; bit < frame->bit_count; ++bit) {
		if (frame->bits[bit] != 1)
			return false;
	}
	for (size_t bit = frame->bit_count;
	     bit < ARRAY_LENGTH(frame->bits); ++bit) {
		if (frame->bits[bit] != 0)
			return false;
	}
	for (size_t rule = frame->rule_count;
	     rule < ARRAY_LENGTH(frame->rules); ++rule) {
		if (frame->rules[rule] != V90_SPECTRAL_RULE_A)
			return false;
	}
	return true;
}

static bool run_sequence(
	enum g711_law law, uint8_t redundancy,
	uint8_t lookahead, size_t symbol_limit, bool erase)
{
	struct v90_pcm_mapping mapping;
	struct pcm_downstream_tx transmitter;
	struct v90_pcm_stream_rx receivers[2];
	enum v90_spectral_rule expected_rules[3] = { 0 };
	size_t expected_rule_count = 0;

	CHECK(make_mapping(
		      &mapping, V90_CP_TRAINING, law,
		      redundancy, lookahead) &&
	      pcm_downstream_tx_init(
		      &transmitter, PCM_PROTOCOL_V90, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      v90_pcm_stream_rx_init(
		      &receivers[0], &mapping,
		      V90_PCM_MAPPING_TRANSMIT) == V90_PCM_STREAM_RX_OK &&
	      v90_pcm_stream_rx_init(
		      &receivers[1], &mapping,
		      V90_PCM_MAPPING_CODEC) == V90_PCM_STREAM_RX_OK,
	      "sequence fixture did not initialize");
	for (size_t index = 0; index < symbol_limit; ++index) {
		struct pcm_downstream_symbol symbol;
		enum v90_pcm_stream_rx_result expected_result =
			index % V90_CP_FRAME_INTERVALS ==
				V90_CP_FRAME_INTERVALS - 1u ?
				V90_PCM_STREAM_RX_FRAME_READY :
				V90_PCM_STREAM_RX_BUFFERING;

		CHECK(pcm_downstream_tx_next(&transmitter, &symbol) ==
			      PCM_DOWNSTREAM_TX_OK,
		      "transmitter stopped at symbol %zu", index);
		if (symbol.shaping_frame_start) {
			CHECK(expected_rule_count < ARRAY_LENGTH(expected_rules),
			      "frame reported too many shaping rules");
			expected_rules[expected_rule_count++] =
				symbol.shaping_rule;
		}
		for (size_t plane = 0; plane < ARRAY_LENGTH(receivers); ++plane) {
			struct v90_pcm_stream_frame frame;
			struct v90_pcm_stream_frame sentinel;
			uint8_t octet;
			enum v90_pcm_stream_rx_result result;

			CHECK(make_plane_octet(
				      &mapping, &symbol,
				      (enum v90_pcm_mapping_plane)plane,
				      index % V90_CP_FRAME_INTERVALS,
				      &octet),
			      "plane %zu symbol %zu did not map", plane, index);
			if (erase && index >= 65u && index < 82u)
				CHECK(g711_ucode_octet(law, 0u, true, &octet),
				      "erased octet did not encode");
			memset(&frame, 0xa5, sizeof(frame));
			sentinel = frame;
			result = v90_pcm_stream_rx_receive_octet(
				&receivers[plane], octet, &frame);
			CHECK(result == expected_result,
			      "plane %zu symbol %zu returned %d", plane, index,
			      (int)result);
			if (result == V90_PCM_STREAM_RX_FRAME_READY) {
				/* The shared GPC descrambler and sign histories must
				 * recover the data after damaged decisions, without an
				 * epoch reset. Absolute shaping-rule labels can differ
				 * after sign errors; the differential data is unambiguous. */
				if (!erase || index < 65u || index >= 130u)
					CHECK(frame_is_expected(
					      &frame, &mapping,
					      erase && index >= 65u ? NULL : expected_rules),
				      "plane %zu frame %llu did not decode",
				      plane,
				      (unsigned long long)
					      receivers[plane].received_frames);
			} else
				CHECK(memcmp(&frame, &sentinel, sizeof(frame)) == 0,
				      "buffering changed frame output");
		}
		if (expected_result == V90_PCM_STREAM_RX_FRAME_READY) {
			CHECK(expected_rule_count == redundancy,
			      "frame reported %zu rules instead of %u",
			      expected_rule_count, redundancy);
			memset(expected_rules, 0, sizeof(expected_rules));
			expected_rule_count = 0;
		}
	}
	for (size_t plane = 0; plane < ARRAY_LENGTH(receivers); ++plane) {
		CHECK(receivers[plane].received_symbols == symbol_limit &&
		      receivers[plane].received_frames ==
			      symbol_limit / V90_CP_FRAME_INTERVALS &&
		      v90_pcm_stream_rx_is_valid(&receivers[plane]),
		      "plane %zu accounting changed", plane);
	}
	return true;
}

static bool test_generated_sequences(void)
{
	for (unsigned int law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law) {
		for (uint8_t redundancy = 0;
		     redundancy <= 3; ++redundancy) {
			for (uint8_t lookahead = 0;
			     lookahead <= 3; ++lookahead) {
				CHECK(run_sequence(
				      (enum g711_law)law, redundancy,
				      lookahead, 48, false) &&
				      run_sequence((enum g711_law)law, redundancy,
						   lookahead, 180, true),
				      "sequence failed for %u/%u/%u",
				      law, redundancy, lookahead);
			}
		}
	}
	return true;
}

static bool test_descrambled_bits(void)
{
	struct v90_pcm_mapping mapping;
	struct pcm_downstream_tx transmitter;
	struct v90_pcm_stream_rx receiver;
	struct v90_pcm_stream_frame frame;

	CHECK(make_mapping(
		      &mapping, V90_CP_TRAINING, G711_LAW_PCMA, 0, 0) &&
	      pcm_downstream_tx_init(
		      &transmitter, PCM_PROTOCOL_V90, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      v90_pcm_stream_rx_init(
		      &receiver, &mapping, V90_PCM_MAPPING_TRANSMIT) ==
		      V90_PCM_STREAM_RX_OK,
	      "error fixture did not initialize");
	for (size_t index = 0; index < V90_CP_FRAME_INTERVALS; ++index) {
		struct pcm_downstream_symbol symbol;
		struct v90_pcm_symbol altered;
		enum v90_pcm_stream_rx_result result;

		CHECK(pcm_downstream_tx_next(&transmitter, &symbol) ==
			      PCM_DOWNSTREAM_TX_OK &&
		      v90_pcm_symbol_make(
			      &altered, mapping.law, symbol.pcm.ucode,
			      index == 0 ? !symbol.pcm.positive :
					   symbol.pcm.positive),
		      "error symbol %zu was not generated", index);
		result = v90_pcm_stream_rx_receive_octet(
			&receiver, altered.octet, &frame);
		CHECK(result == (index == V90_CP_FRAME_INTERVALS - 1u ?
					 V90_PCM_STREAM_RX_FRAME_READY :
					 V90_PCM_STREAM_RX_BUFFERING),
		      "error symbol %zu returned %d", index,
		      (int)result);
	}
	bool changed = false;
	for (size_t bit = 0; bit < frame.bit_count; ++bit)
		changed = changed || frame.bits[bit] != 1;
	CHECK(changed, "altered stream did not change decoded bits");
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

static bool check_noisy_octet(
	const struct v90_pcm_stream_rx *prefix, uint8_t octet)
{
	const struct v90_pcm_mapping *mapping = &prefix->decoder.mapping;
	enum v90_pcm_mapping_plane plane = prefix->decoder.decision_plane;
	size_t phase = prefix->pending_count;
	struct v90_pcm_stream_rx expected = *prefix, received = *prefix;
	struct v90_pcm_stream_frame want = { 0 }, got = { 0 };
	struct v90_pcm_symbol input;
	uint8_t nearest = 0;
	int32_t best_distance = INT32_MAX;

	CHECK(v90_pcm_symbol_parse(&input, mapping->law, octet),
	      "octet did not parse");
	for (uint8_t label = 0; label < mapping->modulus[phase]; ++label) {
		struct v90_pcm_symbol point;
		uint8_t ucode;

		CHECK(v90_pcm_mapping_ucode(mapping, plane, phase, label, &ucode) ==
			      V90_PCM_MAPPING_OK &&
		      v90_pcm_symbol_make(&point, mapping->law, ucode, input.positive),
		      "decision oracle failed");
		int32_t distance = point.sample - input.sample;
		if (distance < 0)
			distance = -distance;
		if (distance < best_distance) {
			best_distance = distance;
			nearest = ucode;
		}
	}
	enum v90_pcm_stream_rx_result result = v90_pcm_stream_rx_receive_decision(
		&expected, nearest, input.positive, &want);
	CHECK(result == (phase == V90_CP_FRAME_INTERVALS - 1u ?
			 V90_PCM_STREAM_RX_FRAME_READY : V90_PCM_STREAM_RX_BUFFERING) &&
	      v90_pcm_stream_rx_receive_octet(&received, octet, &got) == result &&
	      memcmp(&expected, &received, sizeof(expected)) == 0 &&
	      memcmp(&want, &got, sizeof(want)) == 0,
	      "law %u plane %u phase %zu octet %02x decision differed",
	      (unsigned int)mapping->law, (unsigned int)plane, phase, octet);
	return true;
}

static bool test_noisy_octets(void)
{
	for (unsigned int law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		struct v90_pcm_mapping mapping;

		CHECK(make_mapping(&mapping, V90_CP_DATA, law, 3, 0),
		      "noisy mapping failed");
		/* Different, non-contiguous sets expose both mapper-phase and
		 * linear-versus-companded distance mistakes, including ties. */
		memset(mapping.transmit, 0, sizeof(mapping.transmit));
		memset(mapping.codec, 0, sizeof(mapping.codec));
		for (size_t phase = 0; phase < V90_CP_FRAME_INTERVALS; ++phase)
			for (uint8_t point = 0; point < 8u; ++point) {
				add_ucode(&mapping.transmit[phase],
					  (uint8_t)(12u + phase + 3u * point));
				add_ucode(&mapping.codec[phase],
					  (uint8_t)(72u + phase + 3u * point));
			}
		CHECK(v90_pcm_mapping_is_valid(&mapping), "noisy sets are invalid");
		for (size_t plane = 0; plane < 2u; ++plane) {
			struct v90_pcm_stream_rx prefix;

			CHECK(v90_pcm_stream_rx_init(&prefix, &mapping, plane) ==
				      V90_PCM_STREAM_RX_OK, "noisy receiver failed");
			for (size_t phase = 0; phase < V90_CP_FRAME_INTERVALS; ++phase) {
				uint8_t first;
				struct v90_pcm_stream_frame frame;

				CHECK(v90_pcm_mapping_ucode(&mapping, plane, phase, 0, &first) ==
					      V90_PCM_MAPPING_OK, "phase prefix failed");
				for (unsigned int octet = 0; octet < 256u; ++octet)
					CHECK(check_noisy_octet(&prefix, (uint8_t)octet),
					      "noisy decision failed");
				CHECK(v90_pcm_stream_rx_receive_decision(
					      &prefix, first, true, &frame) ==
					      (phase == V90_CP_FRAME_INTERVALS - 1u ?
					       V90_PCM_STREAM_RX_FRAME_READY : V90_PCM_STREAM_RX_BUFFERING),
				      "noisy phase prefix stopped");
			}
		}
	}
	return true;
}

static bool test_contracts(void)
{
	struct v90_pcm_mapping mapping;
	struct v90_pcm_stream_rx receiver;
	struct v90_pcm_stream_rx saved_receiver;
	union {
		struct v90_pcm_stream_rx receiver;
		struct v90_pcm_stream_frame frame;
	} aliased;
	struct v90_pcm_stream_rx untouched;
	struct v90_pcm_stream_rx saved_untouched;
	struct v90_pcm_stream_frame frame;
	struct v90_pcm_stream_frame saved_frame;
	uint8_t labels[V90_CP_FRAME_INTERVALS];
	uint8_t ucodes[V90_CP_FRAME_INTERVALS];

	CHECK(make_mapping(
		      &mapping, V90_CP_TRAINING, G711_LAW_PCMU, 0, 0),
	      "contract mapping did not initialize");
	memset(&untouched, 0xa5, sizeof(untouched));
	saved_untouched = untouched;
	CHECK(v90_pcm_stream_rx_init(
		      &untouched, &mapping,
		      (enum v90_pcm_mapping_plane)99) ==
		      V90_PCM_STREAM_RX_INVALID_ARGUMENT &&
	      memcmp(&untouched, &saved_untouched,
		     sizeof(untouched)) == 0,
	      "rejected plane changed its destination");
	CHECK(make_mapping(
		      &mapping, V90_CP_DATA, G711_LAW_PCMU, 0, 0) &&
	      v90_pcm_stream_rx_init(
		      &receiver, &mapping, V90_PCM_MAPPING_TRANSMIT) ==
		      V90_PCM_STREAM_RX_OK &&
	      receiver.decoder.mapping.kind == V90_CP_DATA,
	      "data mapping was not accepted");
	CHECK(make_mapping(
		      &mapping, V90_CP_TRAINING, G711_LAW_PCMU, 0, 0) &&
	      v90_pcm_stream_rx_init(
		      &receiver, &mapping, V90_PCM_MAPPING_CODEC) ==
		      V90_PCM_STREAM_RX_OK,
	      "contract receiver did not initialize");
	memset(&frame, 0x5a, sizeof(frame));
	saved_frame = frame;
	saved_receiver = receiver;
	aliased.receiver = receiver;
	CHECK(v90_pcm_stream_rx_receive_decision(
		      &aliased.receiver, 0, false, &aliased.frame) ==
		      V90_PCM_STREAM_RX_INVALID_ARGUMENT &&
	      v90_pcm_stream_rx_receive_octet(
		      &aliased.receiver, 0, &aliased.frame) ==
		      V90_PCM_STREAM_RX_INVALID_ARGUMENT &&
	      memcmp(&aliased.receiver, &receiver, sizeof(receiver)) == 0,
	      "overlapping output changed stream receiver state");
	CHECK(v90_pcm_stream_rx_receive_decision(
		      &receiver, 0, false, &frame) ==
		      V90_PCM_STREAM_RX_INVALID_SYMBOL &&
	      memcmp(&receiver, &saved_receiver, sizeof(receiver)) == 0 &&
	      memcmp(&frame, &saved_frame, sizeof(frame)) == 0,
	      "absent decision was not transactional");
	memset(labels, 3, sizeof(labels));
	CHECK(labels_to_ucodes(
		      &mapping, V90_PCM_MAPPING_CODEC, labels, ucodes),
	      "unused tuple did not map to valid decisions");
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS - 1u; ++interval) {
		CHECK(v90_pcm_stream_rx_receive_decision(
			      &receiver, ucodes[interval], false, &frame) ==
			      V90_PCM_STREAM_RX_BUFFERING &&
		      memcmp(&frame, &saved_frame, sizeof(frame)) == 0,
		      "partial frame %zu changed output", interval);
	}
	CHECK(v90_pcm_stream_rx_receive_decision(
		      &receiver, ucodes[V90_CP_FRAME_INTERVALS - 1u],
		      false, &frame) == V90_PCM_STREAM_RX_FRAME_READY &&
	      receiver.received_symbols == V90_CP_FRAME_INTERVALS &&
	      receiver.received_frames == 1u && receiver.pending_count == 0u,
	      "unused received tuple stopped the stream clock");
	saved_frame = frame;
	CHECK(v90_pcm_stream_rx_receive_decision(
		      NULL, ucodes[0], false, &frame) ==
		      V90_PCM_STREAM_RX_INVALID_ARGUMENT &&
	      v90_pcm_stream_rx_receive_decision(
		      &receiver, ucodes[0], false, NULL) ==
		      V90_PCM_STREAM_RX_INVALID_ARGUMENT &&
	      v90_pcm_stream_rx_receive_octet(NULL, 0, &frame) ==
		      V90_PCM_STREAM_RX_INVALID_ARGUMENT &&
	      v90_pcm_stream_rx_init(
		      NULL, &mapping, V90_PCM_MAPPING_TRANSMIT) ==
		      V90_PCM_STREAM_RX_INVALID_ARGUMENT,
	      "null argument was accepted");
	CHECK(v90_pcm_stream_rx_init(
		      &receiver, &mapping, V90_PCM_MAPPING_CODEC) ==
		      V90_PCM_STREAM_RX_OK,
	      "counter-limit receiver did not initialize");
	receiver.received_frames = UINT64_MAX / mapping.frame_bits;
	receiver.received_symbols =
		receiver.received_frames * V90_CP_FRAME_INTERVALS;
	receiver.received_bits =
		receiver.received_frames * mapping.frame_bits;
	memset(labels, 0, sizeof(labels));
	CHECK(v90_pcm_stream_rx_is_valid(&receiver) &&
	      labels_to_ucodes(
		      &mapping, V90_PCM_MAPPING_CODEC, labels, ucodes),
	      "counter-limit state was not representable");
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS - 1u; ++interval) {
		CHECK(v90_pcm_stream_rx_receive_decision(
			      &receiver, ucodes[interval], false, &frame) ==
			      V90_PCM_STREAM_RX_BUFFERING,
		      "counter-limit prefix %zu failed", interval);
	}
	saved_receiver = receiver;
	receiver.pending_ucodes[0] = 0u;
	CHECK(!v90_pcm_stream_rx_is_valid(&receiver),
	      "absent buffered codeword was accepted");
	receiver = saved_receiver;
	CHECK(v90_pcm_stream_rx_receive_decision(
		      &receiver, ucodes[V90_CP_FRAME_INTERVALS - 1u],
		      false, &frame) == V90_PCM_STREAM_RX_OUT_OF_RANGE &&
	      memcmp(&receiver, &saved_receiver, sizeof(receiver)) == 0 &&
	      memcmp(&frame, &saved_frame, sizeof(frame)) == 0,
	      "counter overflow was not rejected transactionally");
	receiver.pending_count = V90_CP_FRAME_INTERVALS;
	CHECK(!v90_pcm_stream_rx_is_valid(&receiver) &&
	      !v90_pcm_stream_rx_is_valid(NULL) &&
	      v90_pcm_stream_rx_receive_octet(&receiver, 0, &frame) ==
		      V90_PCM_STREAM_RX_INVALID_STATE,
	      "state contract changed");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "generated sequences", test_generated_sequences },
		{ "descrambled bits", test_descrambled_bits },
		{ "noisy octets", test_noisy_octets },
		{ "contracts", test_contracts },
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
