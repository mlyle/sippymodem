// SPDX-License-Identifier: BSD-3-Clause

#include <pcm_downstream_rx.h>
#include <pcm_downstream_tx.h>

#include "test.h"

#include <float.h>
#include <stdint.h>
#include <string.h>

enum {
	REFERENCE_MAX_SYMBOLS = 192,
	GPC_HISTORY_BITS = 23,
	GPC_TAP_DELAY = 18,
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
	};
	uint8_t size = kind == V90_CP_TRAINING ? 4 : 8;

	for (uint8_t ordinal = 0; ordinal < size; ++ordinal)
		add_ucode(
			&parameters.transmit_constellations[0],
			(uint8_t)(12 + ordinal));
	return v90_pcm_mapping_init(mapping, &parameters) ==
	       V90_PCM_MAPPING_OK;
}

static bool make_width_mapping(
	struct v90_pcm_mapping *mapping, enum g711_law law,
	uint8_t frame_bits)
{
	struct v90_cp_parameters parameters = {
		.kind = V90_CP_TRAINING,
		.law = law,
		.selected_rate_index = (uint8_t)(frame_bits - 8u),
		.constellation_count = V90_CP_MAX_CONSTELLATIONS,
	};
	uint8_t sign_bits = frame_bits > 27u ?
				    (uint8_t)(frame_bits - 24u) : 3u;
	uint8_t modulus_bits = (uint8_t)(frame_bits - sign_bits);

	parameters.sign_redundancy =
		(uint8_t)(V90_CP_FRAME_INTERVALS - sign_bits);
	for (size_t interval = 0u;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		uint8_t exponent = (uint8_t)(
			modulus_bits / V90_CP_FRAME_INTERVALS +
			(interval < modulus_bits % V90_CP_FRAME_INTERVALS));
		uint8_t count = (uint8_t)(1u << exponent);

		parameters.frame_constellation[interval] = (uint8_t)interval;
		for (uint8_t ordinal = 0u; ordinal < count; ++ordinal) {
			add_ucode(
				&parameters.transmit_constellations[interval],
				(uint8_t)(4u + interval * 20u + ordinal));
		}
	}
	return v90_pcm_mapping_init(mapping, &parameters) ==
	       V90_PCM_MAPPING_OK;
}

static uint8_t reference_scrambled_one(uint32_t *history)
{
	uint8_t feedback = (uint8_t)(
		((*history >> (GPC_TAP_DELAY - 1u)) ^
		 (*history >> (GPC_HISTORY_BITS - 1u))) & 1u);
	uint8_t output = 1u ^ feedback;

	*history = ((*history << 1) | output) &
		   ((UINT32_C(1) << GPC_HISTORY_BITS) - 1u);
	return output;
}

static bool append_output(
	const struct v90_pcm_mapping *mapping,
	const struct v90_pcm_frame_output *output,
	struct pcm_downstream_symbol *symbols, size_t *symbol_count)
{
	size_t shaping_length = mapping->sign_redundancy == 0 ?
				0 : V90_CP_FRAME_INTERVALS /
					    mapping->sign_redundancy;
	size_t rule_index = 0;
	enum v90_spectral_rule rule = V90_SPECTRAL_RULE_A;

	if (*symbol_count + output->symbol_count >
	    REFERENCE_MAX_SYMBOLS)
		return false;
	for (size_t index = 0; index < output->symbol_count; ++index) {
		struct pcm_downstream_symbol symbol = {
			.pcm = output->symbols[index],
		};

		if (mapping->sign_redundancy != 0) {
			bool starts = (*symbol_count + index) %
				      shaping_length == 0;

			if (starts) {
				if (rule_index >= output->rule_count)
					return false;
				rule = output->rules[rule_index++];
			}
			symbol.shaping_rule = rule;
			symbol.shaping_rule_valid = true;
			symbol.shaping_frame_start = starts;
		}
		symbols[*symbol_count + index] = symbol;
	}
	if (rule_index != output->rule_count)
		return false;
	*symbol_count += output->symbol_count;
	return true;
}

static bool make_reference(
	const struct v90_pcm_mapping *mapping, uint64_t frame_count,
	struct pcm_downstream_symbol *symbols, size_t *symbol_count)
{
	struct v90_pcm_frame_encoder encoder;
	uint32_t scrambler_history = 0;
	size_t produced = 0;

	if (frame_count > REFERENCE_MAX_SYMBOLS / V90_CP_FRAME_INTERVALS ||
	    v90_pcm_frame_encoder_init(&encoder, mapping) !=
		    V90_PCM_FRAME_ENCODER_OK)
		return false;
	for (uint64_t frame = 0; frame < frame_count; ++frame) {
		struct v90_pcm_frame_output output;
		uint8_t bits[V90_PCM_FRAME_MAX_BITS];
		enum v90_pcm_frame_encoder_result result;

		for (size_t bit = 0; bit < mapping->frame_bits; ++bit)
			bits[bit] = reference_scrambled_one(
				&scrambler_history);
		result = v90_pcm_frame_encoder_process(
			&encoder, bits, mapping->frame_bits, &output);
		if (result == V90_PCM_FRAME_ENCODER_SYMBOLS_READY) {
			if (!append_output(
				    mapping, &output, symbols, &produced))
				return false;
		} else if (result != V90_PCM_FRAME_ENCODER_BUFFERING) {
			return false;
		}
	}
	for (;;) {
		struct v90_pcm_frame_output output;
		enum v90_pcm_frame_encoder_result result =
			v90_pcm_frame_encoder_finish(&encoder, &output);

		if (result == V90_PCM_FRAME_ENCODER_COMPLETE)
			break;
		if (result != V90_PCM_FRAME_ENCODER_SYMBOLS_READY ||
		    !append_output(mapping, &output, symbols, &produced))
			return false;
	}
	*symbol_count = produced;
	return produced == frame_count * V90_CP_FRAME_INTERVALS;
}

static bool symbols_equal(
	const struct pcm_downstream_symbol *left,
	const struct pcm_downstream_symbol *right)
{
	return left->pcm.ucode == right->pcm.ucode &&
	       left->pcm.positive == right->pcm.positive &&
	       left->pcm.octet == right->pcm.octet &&
	       left->pcm.sample == right->pcm.sample &&
	       left->shaping_rule == right->shaping_rule &&
	       left->shaping_rule_valid == right->shaping_rule_valid &&
	       left->shaping_frame_start == right->shaping_frame_start;
}

static bool test_generated_sequences(void)
{
	uint8_t first_ucodes[2][4][4][V90_CP_FRAME_INTERVALS] = { 0 };

	for (unsigned int law_index = 0; law_index < 2; ++law_index) {
		enum g711_law law = law_index == 0 ?
				    G711_LAW_PCMU : G711_LAW_PCMA;

		for (uint8_t redundancy = 0; redundancy <= 3; ++redundancy) {
			for (uint8_t lookahead = 0; lookahead <= 3; ++lookahead) {
				struct v90_pcm_mapping mapping;
				struct pcm_downstream_tx transmitter;
				struct pcm_downstream_symbol actual[REFERENCE_MAX_SYMBOLS];
				struct pcm_downstream_symbol expected[REFERENCE_MAX_SYMBOLS];
				size_t actual_count = 0;
				size_t expected_count = 0;
				uint64_t accepted_at_finish;

				CHECK(make_mapping(
					      &mapping, V90_CP_TRAINING, law,
					      redundancy, lookahead) &&
				      pcm_downstream_tx_init(
					      &transmitter, PCM_PROTOCOL_V90,
					      &mapping) ==
					      PCM_DOWNSTREAM_TX_OK,
				      "sequence %u/%u/%u did not initialize",
				      law_index, redundancy, lookahead);
				while (actual_count < 13) {
					CHECK(pcm_downstream_tx_next(
						      &transmitter,
						      &actual[actual_count]) ==
						      PCM_DOWNSTREAM_TX_OK,
					      "sequence %u/%u/%u stopped early",
					      law_index, redundancy, lookahead);
					actual_count++;
				}
				accepted_at_finish = transmitter.accepted_frames;
				CHECK(pcm_downstream_tx_request_finish(&transmitter) ==
					      PCM_DOWNSTREAM_TX_OK &&
				      pcm_downstream_tx_request_finish(&transmitter) ==
					      PCM_DOWNSTREAM_TX_OK,
				      "sequence %u/%u/%u finish was not idempotent",
				      law_index, redundancy, lookahead);
				while (!pcm_downstream_tx_is_complete(&transmitter)) {
					CHECK(actual_count < REFERENCE_MAX_SYMBOLS,
					      "sequence %u/%u/%u exceeded its bounded output",
					      law_index, redundancy, lookahead);
					enum pcm_downstream_tx_result result =
						pcm_downstream_tx_next(
							&transmitter,
							&actual[actual_count]);

					if (result == PCM_DOWNSTREAM_TX_COMPLETE)
						break;
					CHECK(result == PCM_DOWNSTREAM_TX_OK &&
					      actual_count < REFERENCE_MAX_SYMBOLS,
					      "sequence %u/%u/%u failed while draining",
					      law_index, redundancy, lookahead);
					actual_count++;
				}
				CHECK(make_reference(
					      &mapping, accepted_at_finish,
					      expected, &expected_count) &&
				      actual_count == expected_count &&
				      transmitter.accepted_frames ==
					      accepted_at_finish &&
				      transmitter.transmitted_symbols ==
					      actual_count &&
				      actual_count % V90_CP_FRAME_INTERVALS == 0,
				      "sequence %u/%u/%u lost an accepted frame",
				      law_index, redundancy, lookahead);
				for (size_t symbol = 0;
				     symbol < actual_count; ++symbol) {
					CHECK(symbols_equal(
						      &actual[symbol],
						      &expected[symbol]),
					      "sequence %u/%u/%u symbol %zu differed",
					      law_index, redundancy, lookahead,
					      symbol);
				}
				for (size_t symbol = 0;
				     symbol < V90_CP_FRAME_INTERVALS; ++symbol)
					first_ucodes[law_index][redundancy]
						[lookahead][symbol] =
						actual[symbol].pcm.ucode;
			}
			for (uint8_t lookahead = 1; lookahead <= 3; ++lookahead) {
				CHECK(memcmp(
					      first_ucodes[law_index][redundancy][0],
					      first_ucodes[law_index][redundancy]
						      [lookahead],
					      V90_CP_FRAME_INTERVALS) == 0,
				      "first TRN2d magnitudes changed with look-ahead %u/%u/%u",
				      law_index, redundancy, lookahead);
			}
		}
	}
	return true;
}

static bool run_v92_width(enum g711_law law, uint8_t frame_bits)
{
	const struct v92_suvd_parameters parameters = {
		.request_silence = (frame_bits & 1u) != 0u,
		.acknowledges_cpu = law == G711_LAW_PCMA,
	};
	struct v90_pcm_mapping mapping;
	struct pcm_downstream_tx transmitter;
	struct pcm_downstream_rx receiver;
	size_t training_frames = 0u;
	size_t records = 0u;
	size_t symbol_count = 0u;
	size_t maximum_symbols =
		V90_PHASE4_TRN2D_MINIMUM_SYMBOLS + 512u;

	CHECK(make_width_mapping(&mapping, law, frame_bits) &&
	      pcm_downstream_tx_init(
		      &transmitter, PCM_PROTOCOL_V92, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_rx_init(
		      &receiver, PCM_PROTOCOL_V92, &mapping,
		      V90_PCM_MAPPING_TRANSMIT) == PCM_DOWNSTREAM_RX_OK &&
	      pcm_downstream_tx_start_suvd(
		      &transmitter, &parameters) == PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_start_suvd(
		      &transmitter, &parameters) == PCM_DOWNSTREAM_TX_OK,
	      "V.92 width %u law %u fixture did not initialize",
	      frame_bits, law);
	while (records < 2u && symbol_count < maximum_symbols) {
		struct pcm_downstream_symbol symbol;
		struct pcm_downstream_event event;
		struct pcm_downstream_event untouched;
		enum pcm_downstream_rx_result result;

		CHECK(pcm_downstream_tx_next(&transmitter, &symbol) ==
			      PCM_DOWNSTREAM_TX_OK,
		      "V.92 width %u law %u transmitter stopped",
		      frame_bits, law);
		symbol_count++;
		memset(&event, 0xa5, sizeof(event));
		untouched = event;
		result = pcm_downstream_rx_receive(
			&receiver, symbol.pcm.octet, &event);
		if (result == PCM_DOWNSTREAM_RX_BUFFERING) {
			CHECK(memcmp(&event, &untouched, sizeof(event)) == 0,
			      "V.92 width %u law %u buffering changed event",
			      frame_bits, law);
			continue;
		}
		if (result == PCM_DOWNSTREAM_RX_TRN2D_FRAME) {
			CHECK(event.trn2d.bit_count == frame_bits &&
			      event.trn2d.training_bit_errors == 0u,
			      "V.92 width %u law %u corrupted TRN2d",
			      frame_bits, law);
			training_frames++;
			continue;
		}
		CHECK(result == PCM_DOWNSTREAM_RX_SUVD &&
		      event.control.type == V92_CONTROL_EVENT_SUVD &&
		      event.control.record.suvd.frame_result == V92_SUVD_OK &&
		      event.control.record.suvd.parameters.request_silence ==
			      parameters.request_silence &&
		      event.control.record.suvd.parameters.acknowledges_cpu ==
			      parameters.acknowledges_cpu,
		      "V.92 width %u law %u receiver returned %s",
		      frame_bits, law,
		      pcm_downstream_rx_result_name(result));
		records++;
	}
	CHECK(records == 2u &&
	      training_frames == V90_PHASE4_TRN2D_MINIMUM_FRAMES &&
	      transmitter.protocol == PCM_PROTOCOL_V92 &&
	      transmitter.stage == PCM_DOWNSTREAM_TX_V92_CONTROL &&
	      transmitter.control_configured &&
	      receiver.stage == PCM_DOWNSTREAM_RX_V92_CONTROL &&
	      receiver.trn2d_bit_errors == 0u &&
	      pcm_downstream_tx_is_valid(&transmitter) &&
	      pcm_downstream_rx_is_valid(&receiver),
	      "V.92 width %u law %u lost its continuous control epoch",
	      frame_bits, law);
	return true;
}

static bool test_v92_sequences(void)
{
	for (unsigned int law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law) {
		for (uint8_t frame_bits =
			     V92_STATUS_UPDATE_DOWNSTREAM_FRAME_MIN_BITS;
		     frame_bits <=
			     V92_STATUS_UPDATE_DOWNSTREAM_FRAME_MAX_BITS;
		     ++frame_bits) {
			CHECK(run_v92_width(
				      (enum g711_law)law, frame_bits),
			      "V.92 downstream transmitter matrix stopped");
		}
	}
	return true;
}

static bool test_empty_finish(void)
{
	struct v90_pcm_mapping mapping;
	struct pcm_downstream_tx transmitter;
	struct pcm_downstream_symbol symbol;
	struct pcm_downstream_symbol saved;

	CHECK(make_mapping(
		      &mapping, V90_CP_TRAINING, G711_LAW_PCMU, 2, 3) &&
	      pcm_downstream_tx_init(
		      &transmitter, PCM_PROTOCOL_V90, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK,
	      "empty finish fixture did not initialize");
	memset(&symbol, 0xa5, sizeof(symbol));
	saved = symbol;
	CHECK(pcm_downstream_tx_request_finish(&transmitter) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_next(&transmitter, &symbol) ==
		      PCM_DOWNSTREAM_TX_COMPLETE &&
	      pcm_downstream_tx_next(&transmitter, &symbol) ==
		      PCM_DOWNSTREAM_TX_COMPLETE &&
	      memcmp(&symbol, &saved, sizeof(symbol)) == 0 &&
	      transmitter.accepted_frames == 0 &&
	      transmitter.transmitted_symbols == 0,
	      "empty finish generated or changed a symbol");
	return true;
}

static bool test_contracts(void)
{
	const struct v92_suvd_parameters suvd = {
		.request_silence = true,
	};
	const struct v92_suvd_parameters different_suvd = {
		.acknowledges_cpu = true,
	};
	const struct v90_mp_record empty_mp = { 0 };
	struct v90_pcm_mapping mapping;
	struct v90_pcm_mapping data_mapping;
	struct pcm_downstream_tx transmitter;
	struct pcm_downstream_tx saved_transmitter;
	struct pcm_downstream_symbol symbol;
	struct pcm_downstream_symbol saved_symbol;

	CHECK(make_mapping(
		      &mapping, V90_CP_TRAINING, G711_LAW_PCMU, 1, 0) &&
	      make_mapping(
		      &data_mapping, V90_CP_DATA, G711_LAW_PCMU, 1, 0) &&
	      pcm_downstream_tx_init(
		      &transmitter, PCM_PROTOCOL_V90, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_init(
		      &transmitter, PCM_PROTOCOL_V90, &data_mapping) ==
		      PCM_DOWNSTREAM_TX_INVALID_PARAMETERS &&
	      pcm_downstream_tx_init(NULL, PCM_PROTOCOL_V90, &mapping) ==
		      PCM_DOWNSTREAM_TX_INVALID_ARGUMENT &&
	      pcm_downstream_tx_init(
		      &transmitter, (enum pcm_protocol)99, &mapping) ==
		      PCM_DOWNSTREAM_TX_INVALID_ARGUMENT,
	      "initialization contract changed");
	CHECK(pcm_downstream_tx_init(
		      &transmitter, PCM_PROTOCOL_V90, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_start_suvd(&transmitter, &suvd) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      pcm_downstream_tx_init(
		      &transmitter, PCM_PROTOCOL_V92, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_start_mp(&transmitter, &empty_mp) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      pcm_downstream_tx_request_mp_prime(&transmitter) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      pcm_downstream_tx_request_ed(&transmitter) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      pcm_downstream_tx_start_suvd(&transmitter, &suvd) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_start_suvd(
		      &transmitter, &different_suvd) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      pcm_downstream_tx_request_finish(&transmitter) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE,
	      "protocol-specific control branch was not isolated");
	CHECK(pcm_downstream_tx_init(
		      &transmitter, PCM_PROTOCOL_V90, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_next(&transmitter, &symbol) ==
		      PCM_DOWNSTREAM_TX_OK,
	      "corruption fixture did not generate");
	transmitter.queue[0].pcm.octet ^= 1u;
	saved_transmitter = transmitter;
	memset(&symbol, 0xa5, sizeof(symbol));
	saved_symbol = symbol;
	CHECK(!pcm_downstream_tx_is_valid(&transmitter) &&
	      pcm_downstream_tx_next(&transmitter, &symbol) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      pcm_downstream_tx_request_finish(&transmitter) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      memcmp(&transmitter, &saved_transmitter,
		     sizeof(transmitter)) == 0 &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0,
	      "corrupt queue was accepted or changed");

	CHECK(pcm_downstream_tx_init(
		      &transmitter, PCM_PROTOCOL_V90, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK,
	      "numeric fixture did not initialize");
	transmitter.encoder.mapping.shaping_a2_q1_6 = 64;
	transmitter.encoder.shaper.shaping_a2_q1_6 = 64;
	transmitter.encoder.shaper.previous_v = DBL_MAX;
	saved_transmitter = transmitter;
	CHECK(pcm_downstream_tx_is_valid(&transmitter) &&
	      pcm_downstream_tx_next(&transmitter, &symbol) ==
		      PCM_DOWNSTREAM_TX_NUMERIC_FAILURE &&
	      memcmp(&transmitter, &saved_transmitter,
		     sizeof(transmitter)) == 0 &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0,
	      "numeric failure was not transactional");

	CHECK(make_mapping(
		      &mapping, V90_CP_TRAINING, G711_LAW_PCMU, 0, 0) &&
	      pcm_downstream_tx_init(
		      &transmitter, PCM_PROTOCOL_V90, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK,
	      "overflow fixture did not initialize");
	transmitter.accepted_frames =
		UINT64_MAX / V90_CP_FRAME_INTERVALS;
	transmitter.transmitted_symbols = transmitter.accepted_frames *
					  V90_CP_FRAME_INTERVALS;
	saved_transmitter = transmitter;
	CHECK(pcm_downstream_tx_is_valid(&transmitter) &&
	      pcm_downstream_tx_next(&transmitter, &symbol) ==
		      PCM_DOWNSTREAM_TX_OUT_OF_RANGE &&
	      memcmp(&transmitter, &saved_transmitter,
		     sizeof(transmitter)) == 0 &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0 &&
	      !pcm_downstream_tx_is_valid(NULL) &&
	      strcmp(pcm_downstream_tx_result_name(PCM_DOWNSTREAM_TX_OK),
		     "ok") == 0 &&
	      strcmp(pcm_downstream_tx_result_name(
			     (enum pcm_downstream_tx_result)99),
		     "unknown") == 0,
	      "overflow or result-name contract changed");
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
		{ "V.92 sequences", test_v92_sequences },
		{ "empty finish", test_empty_finish },
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
