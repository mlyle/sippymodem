// SPDX-License-Identifier: BSD-3-Clause

#include <v90_trn2d_rx.h>
#include <v90_downstream_tx.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

static void add_ucode(
	struct v90_cp_constellation *constellation, uint8_t ucode)
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
	const struct v90_downstream_symbol *source,
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
	const struct v90_trn2d_frame *frame,
	const struct v90_pcm_mapping *mapping,
	const enum v90_spectral_rule expected_rules[3])
{
	if (frame->bit_count != mapping->frame_bits ||
	    frame->rule_count != mapping->sign_redundancy ||
	    frame->training_bit_errors != 0 ||
	    memcmp(frame->rules, expected_rules,
		   frame->rule_count * sizeof(frame->rules[0])) != 0)
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
	uint8_t lookahead, size_t symbol_limit)
{
	struct v90_pcm_mapping mapping;
	struct v90_downstream_tx transmitter;
	struct v90_trn2d_rx receivers[2];
	enum v90_spectral_rule expected_rules[3] = { 0 };
	size_t expected_rule_count = 0;

	CHECK(make_mapping(
		      &mapping, V90_CP_TRAINING, law,
		      redundancy, lookahead) &&
	      v90_downstream_tx_init(&transmitter, &mapping) ==
		      V90_DOWNSTREAM_TX_OK &&
	      v90_trn2d_rx_init(
		      &receivers[0], &mapping,
		      V90_PCM_MAPPING_TRANSMIT) == V90_TRN2D_RX_OK &&
	      v90_trn2d_rx_init(
		      &receivers[1], &mapping,
		      V90_PCM_MAPPING_CODEC) == V90_TRN2D_RX_OK,
	      "sequence fixture did not initialize");
	for (size_t index = 0; index < symbol_limit; ++index) {
		struct v90_downstream_symbol symbol;
		enum v90_trn2d_rx_result expected_result =
			index % V90_CP_FRAME_INTERVALS ==
				V90_CP_FRAME_INTERVALS - 1u ?
				V90_TRN2D_RX_FRAME_READY :
				V90_TRN2D_RX_BUFFERING;

		CHECK(v90_downstream_tx_next(&transmitter, &symbol) ==
			      V90_DOWNSTREAM_TX_OK,
		      "transmitter stopped at symbol %zu", index);
		if (symbol.shaping_frame_start) {
			CHECK(expected_rule_count < ARRAY_LENGTH(expected_rules),
			      "frame reported too many shaping rules");
			expected_rules[expected_rule_count++] =
				symbol.shaping_rule;
		}
		for (size_t plane = 0; plane < ARRAY_LENGTH(receivers); ++plane) {
			struct v90_trn2d_frame frame;
			struct v90_trn2d_frame sentinel;
			uint8_t octet;
			enum v90_trn2d_rx_result result;

			CHECK(make_plane_octet(
				      &mapping, &symbol,
				      (enum v90_pcm_mapping_plane)plane,
				      index % V90_CP_FRAME_INTERVALS,
				      &octet),
			      "plane %zu symbol %zu did not map", plane, index);
			memset(&frame, 0xa5, sizeof(frame));
			sentinel = frame;
			result = v90_trn2d_rx_receive_octet(
				&receivers[plane], octet, &frame);
			CHECK(result == expected_result,
			      "plane %zu symbol %zu returned %s", plane, index,
			      v90_trn2d_rx_result_name(result));
			if (result == V90_TRN2D_RX_FRAME_READY)
				CHECK(frame_is_expected(
					      &frame, &mapping, expected_rules),
				      "plane %zu frame %llu did not decode",
				      plane,
				      (unsigned long long)
					      receivers[plane].received_frames);
			else
				CHECK(memcmp(&frame, &sentinel, sizeof(frame)) == 0,
				      "buffering changed frame output");
			CHECK(v90_trn2d_rx_minimum_received(&receivers[plane]) ==
				      (index + 1u >= V90_TRN2D_MINIMUM_SYMBOLS),
			      "plane %zu minimum changed at symbol %zu",
			      plane, index);
		}
		if (expected_result == V90_TRN2D_RX_FRAME_READY) {
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
		      receivers[plane].training_bit_errors == 0 &&
		      v90_trn2d_rx_is_valid(&receivers[plane]),
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
				      lookahead, 48),
				      "sequence failed for %u/%u/%u",
				      law, redundancy, lookahead);
			}
		}
	}
	return true;
}

static bool test_minimum_and_errors(void)
{
	struct v90_pcm_mapping mapping;
	struct v90_downstream_tx transmitter;
	struct v90_trn2d_rx receiver;
	struct v90_trn2d_frame frame;

	CHECK(run_sequence(
		      G711_LAW_PCMU, 3, 3, V90_TRN2D_MINIMUM_SYMBOLS),
	      "minimum-length sequence failed");
	CHECK(make_mapping(
		      &mapping, V90_CP_TRAINING, G711_LAW_PCMA, 0, 0) &&
	      v90_downstream_tx_init(&transmitter, &mapping) ==
		      V90_DOWNSTREAM_TX_OK &&
	      v90_trn2d_rx_init(
		      &receiver, &mapping, V90_PCM_MAPPING_TRANSMIT) ==
		      V90_TRN2D_RX_OK,
	      "error fixture did not initialize");
	for (size_t index = 0; index < V90_CP_FRAME_INTERVALS; ++index) {
		struct v90_downstream_symbol symbol;
		struct v90_pcm_symbol altered;
		enum v90_trn2d_rx_result result;

		CHECK(v90_downstream_tx_next(&transmitter, &symbol) ==
			      V90_DOWNSTREAM_TX_OK &&
		      v90_pcm_symbol_make(
			      &altered, mapping.law, symbol.pcm.ucode,
			      index == 0 ? !symbol.pcm.positive :
					   symbol.pcm.positive),
		      "error symbol %zu was not generated", index);
		result = v90_trn2d_rx_receive_octet(
			&receiver, altered.octet, &frame);
		CHECK(result == (index == V90_CP_FRAME_INTERVALS - 1u ?
					 V90_TRN2D_RX_FRAME_READY :
					 V90_TRN2D_RX_BUFFERING),
		      "error symbol %zu returned %s", index,
		      v90_trn2d_rx_result_name(result));
	}
	CHECK(frame.training_bit_errors > 0 &&
	      receiver.training_bit_errors == frame.training_bit_errors &&
	      !v90_trn2d_rx_minimum_received(&receiver) &&
	      !v90_trn2d_rx_minimum_received(NULL),
	      "training error accounting changed");
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
	struct v90_trn2d_rx receiver;
	struct v90_trn2d_rx saved_receiver;
	struct v90_trn2d_rx untouched;
	struct v90_trn2d_rx saved_untouched;
	struct v90_trn2d_frame frame;
	struct v90_trn2d_frame saved_frame;
	uint8_t labels[V90_CP_FRAME_INTERVALS];
	uint8_t ucodes[V90_CP_FRAME_INTERVALS];

	CHECK(make_mapping(
		      &mapping, V90_CP_TRAINING, G711_LAW_PCMU, 0, 0),
	      "contract mapping did not initialize");
	memset(&untouched, 0xa5, sizeof(untouched));
	saved_untouched = untouched;
	CHECK(v90_trn2d_rx_init(
		      &untouched, &mapping,
		      (enum v90_pcm_mapping_plane)99) ==
		      V90_TRN2D_RX_INVALID_ARGUMENT &&
	      memcmp(&untouched, &saved_untouched,
		     sizeof(untouched)) == 0,
	      "rejected plane changed its destination");
	CHECK(make_mapping(
		      &mapping, V90_CP_DATA, G711_LAW_PCMU, 0, 0) &&
	      v90_trn2d_rx_init(
		      &untouched, &mapping, V90_PCM_MAPPING_TRANSMIT) ==
		      V90_TRN2D_RX_INVALID_PARAMETERS &&
	      memcmp(&untouched, &saved_untouched,
		     sizeof(untouched)) == 0,
	      "data mapping was accepted for TRN2d");
	CHECK(make_mapping(
		      &mapping, V90_CP_TRAINING, G711_LAW_PCMU, 0, 0) &&
	      v90_trn2d_rx_init(
		      &receiver, &mapping, V90_PCM_MAPPING_CODEC) ==
		      V90_TRN2D_RX_OK,
	      "contract receiver did not initialize");
	memset(&frame, 0x5a, sizeof(frame));
	saved_frame = frame;
	saved_receiver = receiver;
	CHECK(v90_trn2d_rx_receive_decision(
		      &receiver, 0, false, &frame) ==
		      V90_TRN2D_RX_INVALID_SYMBOL &&
	      memcmp(&receiver, &saved_receiver, sizeof(receiver)) == 0 &&
	      memcmp(&frame, &saved_frame, sizeof(frame)) == 0,
	      "absent decision was not transactional");
	memset(labels, 3, sizeof(labels));
	CHECK(labels_to_ucodes(
		      &mapping, V90_PCM_MAPPING_CODEC, labels, ucodes),
	      "unused tuple did not map to valid decisions");
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS - 1u; ++interval) {
		CHECK(v90_trn2d_rx_receive_decision(
			      &receiver, ucodes[interval], false, &frame) ==
			      V90_TRN2D_RX_BUFFERING &&
		      memcmp(&frame, &saved_frame, sizeof(frame)) == 0,
		      "partial frame %zu changed output", interval);
	}
	saved_receiver = receiver;
	CHECK(v90_trn2d_rx_receive_decision(
		      &receiver, ucodes[V90_CP_FRAME_INTERVALS - 1u],
		      false, &frame) == V90_TRN2D_RX_INVALID_SYMBOL &&
	      memcmp(&receiver, &saved_receiver, sizeof(receiver)) == 0 &&
	      memcmp(&frame, &saved_frame, sizeof(frame)) == 0,
	      "unused tuple was not rejected transactionally");
	CHECK(v90_trn2d_rx_receive_decision(
		      NULL, ucodes[0], false, &frame) ==
		      V90_TRN2D_RX_INVALID_ARGUMENT &&
	      v90_trn2d_rx_receive_decision(
		      &receiver, ucodes[0], false, NULL) ==
		      V90_TRN2D_RX_INVALID_ARGUMENT &&
	      v90_trn2d_rx_receive_octet(NULL, 0, &frame) ==
		      V90_TRN2D_RX_INVALID_ARGUMENT &&
	      v90_trn2d_rx_init(
		      NULL, &mapping, V90_PCM_MAPPING_TRANSMIT) ==
		      V90_TRN2D_RX_INVALID_ARGUMENT,
	      "null argument was accepted");
	CHECK(v90_trn2d_rx_init(
		      &receiver, &mapping, V90_PCM_MAPPING_CODEC) ==
		      V90_TRN2D_RX_OK,
	      "counter-limit receiver did not initialize");
	receiver.received_frames = UINT64_MAX / mapping.frame_bits;
	receiver.received_symbols =
		receiver.received_frames * V90_CP_FRAME_INTERVALS;
	receiver.received_bits =
		receiver.received_frames * mapping.frame_bits;
	memset(labels, 0, sizeof(labels));
	CHECK(v90_trn2d_rx_is_valid(&receiver) &&
	      labels_to_ucodes(
		      &mapping, V90_PCM_MAPPING_CODEC, labels, ucodes),
	      "counter-limit state was not representable");
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS - 1u; ++interval) {
		CHECK(v90_trn2d_rx_receive_decision(
			      &receiver, ucodes[interval], false, &frame) ==
			      V90_TRN2D_RX_BUFFERING,
		      "counter-limit prefix %zu failed", interval);
	}
	saved_receiver = receiver;
	CHECK(v90_trn2d_rx_receive_decision(
		      &receiver, ucodes[V90_CP_FRAME_INTERVALS - 1u],
		      false, &frame) == V90_TRN2D_RX_OUT_OF_RANGE &&
	      memcmp(&receiver, &saved_receiver, sizeof(receiver)) == 0 &&
	      memcmp(&frame, &saved_frame, sizeof(frame)) == 0,
	      "counter overflow was not rejected transactionally");
	receiver.pending_count = V90_CP_FRAME_INTERVALS;
	CHECK(!v90_trn2d_rx_is_valid(&receiver) &&
	      !v90_trn2d_rx_is_valid(NULL) &&
	      v90_trn2d_rx_receive_octet(&receiver, 0, &frame) ==
		      V90_TRN2D_RX_INVALID_STATE &&
	      strcmp(v90_trn2d_rx_result_name(V90_TRN2D_RX_FRAME_READY),
		     "frame ready") == 0 &&
	      strcmp(v90_trn2d_rx_result_name(
		     (enum v90_trn2d_rx_result)99),
		     "unknown") == 0,
	      "state or result-name contract changed");
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
		{ "minimum and errors", test_minimum_and_errors },
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
