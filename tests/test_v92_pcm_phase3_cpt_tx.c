// SPDX-License-Identifier: BSD-3-Clause

#include <v92_pcm_phase3_cpt_tx.h>

#include "test.h"

#include <g711.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	REFERENCE_CAPACITY = V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS +
		V92_PCM_PHASE3_TRN1U_ALIGNMENT +
		V92_PCM_PHASE3_CPT_PREAMBLE_SYMBOLS +
		2 * V92_CPT_MAX_FRAME_BITS + V92_PCM_PHASE3_E1U_SYMBOLS,
	MID_RECORD_BITS = 17,
};

struct reference_coding {
	uint8_t scrambled[REFERENCE_CAPACITY];
	size_t count;
	uint8_t previous_sign;
};

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static struct v92_cpt_parameters make_parameters(
	enum g711_law law, uint8_t count, bool codec_differs)
{
	struct v92_cpt_parameters parameters = {
		.law = law,
		.trn1d_ratio_q3_13 = UINT16_C(0x4a35),
		.shaping_a1_q1_6 = 12,
		.shaping_a2_q1_6 = -7,
		.shaping_b1_q1_6 = 3,
		.shaping_b2_q1_6 = -2,
		.selected_rate_index = 9,
		.sign_redundancy = 1,
		.spectral_lookahead = 2,
		.constellation_count = count,
		.acknowledges_peer = true,
		.codec_constellations_differ = codec_differs,
	};

	for (uint8_t index = 0; index < count; ++index) {
		add_ucode(&parameters.transmit_constellations[index],
			  (uint8_t)(20u + index));
		add_ucode(&parameters.transmit_constellations[index],
			  (uint8_t)(80u + index));
		if (codec_differs) {
			add_ucode(&parameters.codec_constellations[index],
				  (uint8_t)(24u + index));
			add_ucode(&parameters.codec_constellations[index],
				  (uint8_t)(88u + index));
		}
	}
	for (size_t interval = 0;
	     interval < V92_CPT_FRAME_INTERVALS; ++interval)
		parameters.frame_constellation[interval] =
			(uint8_t)(interval % count);
	return parameters;
}

static uint8_t packed_bit(const uint8_t *frame, size_t index)
{
	return (uint8_t)((frame[index / 8u] >>
			  (7u - index % 8u)) &
			 1u);
}

static uint8_t reference_scramble(
	struct reference_coding *coding, uint8_t input)
{
	uint8_t output = input;

	if (coding->count >= 5u)
		output ^= coding->scrambled[coding->count - 5u];
	if (coding->count >= 23u)
		output ^= coding->scrambled[coding->count - 23u];
	coding->scrambled[coding->count++] = output;
	return output;
}

static uint8_t expected_octet(
	enum g711_law law, uint8_t ucode, bool positive)
{
	uint8_t octet = law == G711_LAW_PCMA ?
			(uint8_t)(ucode ^ UINT8_C(0xd5)) :
			(uint8_t)~ucode;

	return positive ? octet : (uint8_t)(octet ^ UINT8_C(0x80));
}

static bool symbol_matches(
	const struct v90_pcm_symbol *symbol, enum g711_law law,
	uint8_t ucode, uint8_t sign)
{
	bool positive = sign == 0u;
	uint8_t octet = expected_octet(law, ucode, positive);
	int16_t sample = law == G711_LAW_PCMA ?
				 g711_decode_pcma(octet) :
				 g711_decode_pcmu(octet);

	return symbol->ucode == ucode && symbol->positive == positive &&
	       symbol->octet == octet && symbol->sample == sample;
}

static bool emit_and_check(
	struct v92_pcm_phase3_cpt_tx *transmitter,
	struct reference_coding *reference, uint8_t input,
	bool differential, enum g711_law law, uint8_t ucode)
{
	struct v90_pcm_symbol symbol;
	uint8_t scrambled = reference_scramble(reference, input);
	uint8_t sign;

	if (differential)
		reference->previous_sign ^= scrambled;
	else
		reference->previous_sign = scrambled;
	sign = reference->previous_sign;
	return v92_pcm_phase3_cpt_tx_next(transmitter, &symbol) ==
		       V92_PCM_PHASE3_CPT_TX_OK &&
	       symbol_matches(&symbol, law, ucode, sign);
}

static bool check_complete_sequence(
	enum g711_law law, uint8_t ucode, uint8_t constellation_count,
	bool codec_differs, bool finish_mid_record, size_t trn1u_symbols)
{
	struct v92_cpt_parameters parameters = make_parameters(
		law, constellation_count, codec_differs);
	struct v92_pcm_phase3_cpt_tx transmitter;
	struct v92_pcm_phase3_cpt_tx before;
	struct reference_coding reference = { 0 };
	struct v90_pcm_symbol symbol;
	struct v90_pcm_symbol symbol_before;
	uint8_t frame[V92_CPT_MAX_FRAME_BYTES] = { 0 };
	size_t frame_bits = 0;

	CHECK(v92_cpt_encode(
		      &parameters, frame, sizeof(frame), &frame_bits) ==
		      V92_CPT_OK &&
	      v92_pcm_phase3_cpt_tx_init(
		      &transmitter, law, ucode, &parameters) ==
		      V92_PCM_PHASE3_CPT_TX_OK &&
	      v92_pcm_phase3_cpt_tx_is_valid(&transmitter) &&
	      !v92_pcm_phase3_cpt_tx_is_complete(&transmitter) &&
	      transmitter.stage == V92_PCM_PHASE3_CPT_TX_TRN1U &&
	      transmitter.frame_bit_count == frame_bits &&
	      memcmp(transmitter.frame, frame, sizeof(frame)) == 0,
	      "CPt transmitter did not initialize for law %u",
	      (unsigned int)law);

	before = transmitter;
	CHECK(v92_pcm_phase3_cpt_tx_start(&transmitter) ==
		      V92_PCM_PHASE3_CPT_TX_INVALID_STATE &&
	      memcmp(&transmitter, &before, sizeof(transmitter)) == 0,
	      "CPt started before the second TRN1u minimum");
	for (size_t index = 0; index < trn1u_symbols; ++index) {
		CHECK(emit_and_check(
			      &transmitter, &reference, 1u, false,
			      law, ucode),
		      "second TRN1u differed at symbol %zu", index);
		if (index + 1u ==
		    V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS - 1u) {
			before = transmitter;
			CHECK(v92_pcm_phase3_cpt_tx_start(&transmitter) ==
				      V92_PCM_PHASE3_CPT_TX_INVALID_STATE &&
			      memcmp(&transmitter, &before,
				     sizeof(transmitter)) == 0,
			      "CPt accepted a short second TRN1u");
		}
		if (index + 1u ==
			    V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS + 1u &&
		    trn1u_symbols > V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS) {
			before = transmitter;
			CHECK(v92_pcm_phase3_cpt_tx_start(&transmitter) ==
				      V92_PCM_PHASE3_CPT_TX_INVALID_STATE &&
			      memcmp(&transmitter, &before,
				     sizeof(transmitter)) == 0,
			      "CPt accepted an unaligned second TRN1u");
		}
	}
	CHECK(v92_pcm_phase3_cpt_tx_start(&transmitter) ==
		      V92_PCM_PHASE3_CPT_TX_OK &&
	      transmitter.stage == V92_PCM_PHASE3_CPT_TX_PREAMBLE &&
	      transmitter.coding.scrambler.history ==
		      transmitter.trn1u.scrambler.history &&
	      transmitter.coding.previous_sign == reference.previous_sign,
	      "CPt did not preserve TRN1u coding history");
	before = transmitter;
	CHECK(v92_pcm_phase3_cpt_tx_start(&transmitter) ==
		      V92_PCM_PHASE3_CPT_TX_INVALID_STATE &&
	      v92_pcm_phase3_cpt_tx_request_finish(&transmitter) ==
		      V92_PCM_PHASE3_CPT_TX_INVALID_STATE &&
	      memcmp(&transmitter, &before, sizeof(transmitter)) == 0,
	      "CPt prefix accepted an invalid transition");

	for (size_t index = 0;
	     index < V92_PCM_PHASE3_CPT_PREAMBLE_SYMBOLS; ++index)
		CHECK(emit_and_check(
			      &transmitter, &reference, 1u, true,
			      law, ucode),
		      "CPt preamble differed at symbol %zu", index);
	CHECK(transmitter.stage == V92_PCM_PHASE3_CPT_TX_RECORD &&
	      transmitter.preamble_symbols ==
		      V92_PCM_PHASE3_CPT_PREAMBLE_SYMBOLS &&
	      transmitter.frame_bit_index == 0u,
	      "CPt preamble did not end on the record boundary");

	for (size_t index = 0; index < frame_bits; ++index) {
		if (index == MID_RECORD_BITS) {
			before = transmitter;
			CHECK(v92_pcm_phase3_cpt_tx_request_finish(
				      &transmitter) ==
				      V92_PCM_PHASE3_CPT_TX_INVALID_STATE &&
			      memcmp(&transmitter, &before,
				     sizeof(transmitter)) == 0,
			      "CPt allowed completion before one record");
		}
		CHECK(emit_and_check(
			      &transmitter, &reference,
			      packed_bit(frame, index), true, law, ucode),
		      "first CPt record differed at bit %zu", index);
	}
	CHECK(transmitter.completed_records == 1u &&
	      transmitter.frame_bit_index == 0u &&
	      transmitter.stage == V92_PCM_PHASE3_CPT_TX_RECORD,
	      "first CPt record did not complete cleanly");

	if (finish_mid_record) {
		for (size_t index = 0; index < MID_RECORD_BITS; ++index)
			CHECK(emit_and_check(
				      &transmitter, &reference,
				      packed_bit(frame, index), true,
				      law, ucode),
			      "second CPt record differed at bit %zu", index);
		CHECK(v92_pcm_phase3_cpt_tx_request_finish(&transmitter) ==
			      V92_PCM_PHASE3_CPT_TX_OK &&
		      transmitter.finish_requested &&
		      transmitter.stage == V92_PCM_PHASE3_CPT_TX_RECORD,
		      "mid-record CPt completion did not latch");
		before = transmitter;
		CHECK(v92_pcm_phase3_cpt_tx_request_finish(&transmitter) ==
			      V92_PCM_PHASE3_CPT_TX_INVALID_STATE &&
		      memcmp(&transmitter, &before, sizeof(transmitter)) == 0,
		      "CPt accepted a duplicate completion request");
		for (size_t index = MID_RECORD_BITS;
		     index < frame_bits; ++index)
			CHECK(emit_and_check(
				      &transmitter, &reference,
				      packed_bit(frame, index), true,
				      law, ucode),
			      "finishing CPt record differed at bit %zu", index);
		CHECK(transmitter.completed_records == 2u,
		      "mid-record completion lost its final CPt record");
	} else {
		CHECK(v92_pcm_phase3_cpt_tx_request_finish(&transmitter) ==
			      V92_PCM_PHASE3_CPT_TX_OK,
		      "record-boundary CPt completion was rejected");
	}
	CHECK(transmitter.stage == V92_PCM_PHASE3_CPT_TX_E1U &&
	      transmitter.frame_bit_index == 0u,
	      "CPt completion did not enter E1u on a record boundary");

	for (size_t index = 0;
	     index < V92_PCM_PHASE3_E1U_SYMBOLS; ++index)
		CHECK(emit_and_check(
			      &transmitter, &reference, 0u, true,
			      law, ucode),
		      "E1u differed at symbol %zu", index);
	CHECK(v92_pcm_phase3_cpt_tx_is_complete(&transmitter) &&
	      transmitter.e1u_symbols == V92_PCM_PHASE3_E1U_SYMBOLS,
	      "E1u did not complete after one data frame");
	before = transmitter;
	memset(&symbol, 0xa5, sizeof(symbol));
	symbol_before = symbol;
	CHECK(v92_pcm_phase3_cpt_tx_next(&transmitter, &symbol) ==
		      V92_PCM_PHASE3_CPT_TX_DONE &&
	      v92_pcm_phase3_cpt_tx_request_finish(&transmitter) ==
		      V92_PCM_PHASE3_CPT_TX_INVALID_STATE &&
	      memcmp(&transmitter, &before, sizeof(transmitter)) == 0 &&
	      memcmp(&symbol, &symbol_before, sizeof(symbol)) == 0,
	      "completed CPt transmission was not stable");
	return true;
}

static bool test_generated_sequences(void)
{
	CHECK(V92_PCM_PHASE3_CPT_PREAMBLE_SYMBOLS == 24 &&
	      V92_PCM_PHASE3_E1U_SYMBOLS == 12,
	      "CPt or E1u dimensions changed");
	CHECK(check_complete_sequence(
		      G711_LAW_PCMU, 91u, 1u, false, true,
		      V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS),
	      "PCMU CPt sequence failed");
	CHECK(check_complete_sequence(
		      G711_LAW_PCMA, 103u, 2u, true, false,
		      V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS +
			      V92_PCM_PHASE3_TRN1U_ALIGNMENT),
	      "PCMA CPt sequence failed");
	return true;
}

static bool test_contracts(void)
{
	struct v92_cpt_parameters parameters =
		make_parameters(G711_LAW_PCMU, 1u, false);
	struct v92_cpt_parameters invalid = parameters;
	struct v92_pcm_phase3_cpt_tx transmitter;
	struct v92_pcm_phase3_cpt_tx before;
	struct v90_pcm_symbol symbol;
	struct v90_pcm_symbol symbol_before;

	memset(&transmitter, 0xa5, sizeof(transmitter));
	before = transmitter;
	invalid.constellation_count = 0u;
	CHECK(v92_pcm_phase3_cpt_tx_init(
		      NULL, G711_LAW_PCMU, 91u, &parameters) ==
		      V92_PCM_PHASE3_CPT_TX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_cpt_tx_init(
		      &transmitter, G711_LAW_PCMU, 91u, NULL) ==
		      V92_PCM_PHASE3_CPT_TX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_cpt_tx_init(
		      &transmitter, (enum g711_law)-1, 91u,
		      &parameters) == V92_PCM_PHASE3_CPT_TX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_cpt_tx_init(
		      &transmitter, G711_LAW_PCMA, 91u,
		      &parameters) == V92_PCM_PHASE3_CPT_TX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_cpt_tx_init(
		      &transmitter, G711_LAW_PCMU, 0u, &parameters) ==
		      V92_PCM_PHASE3_CPT_TX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_cpt_tx_init(
		      &transmitter, G711_LAW_PCMU, 91u, &invalid) ==
		      V92_PCM_PHASE3_CPT_TX_INVALID_ARGUMENT &&
	      memcmp(&transmitter, &before, sizeof(transmitter)) == 0,
	      "invalid CPt initialization changed its destination");

	CHECK(v92_pcm_phase3_cpt_tx_init(
		      &transmitter, G711_LAW_PCMU, 91u, &parameters) ==
		      V92_PCM_PHASE3_CPT_TX_OK,
	      "CPt contract fixture did not initialize");
	memset(&symbol, 0xa5, sizeof(symbol));
	symbol_before = symbol;
	before = transmitter;
	CHECK(!v92_pcm_phase3_cpt_tx_is_valid(NULL) &&
	      !v92_pcm_phase3_cpt_tx_is_complete(NULL) &&
	      v92_pcm_phase3_cpt_tx_next(NULL, &symbol) ==
		      V92_PCM_PHASE3_CPT_TX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_cpt_tx_next(&transmitter, NULL) ==
		      V92_PCM_PHASE3_CPT_TX_INVALID_ARGUMENT &&
	      memcmp(&transmitter, &before, sizeof(transmitter)) == 0 &&
	      memcmp(&symbol, &symbol_before, sizeof(symbol)) == 0,
	      "invalid CPt next call changed state or output");

	transmitter.frame_bit_count = 0u;
	before = transmitter;
	CHECK(v92_pcm_phase3_cpt_tx_next(&transmitter, &symbol) ==
		      V92_PCM_PHASE3_CPT_TX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_cpt_tx_start(&transmitter) ==
		      V92_PCM_PHASE3_CPT_TX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_cpt_tx_request_finish(&transmitter) ==
		      V92_PCM_PHASE3_CPT_TX_INVALID_ARGUMENT &&
	      memcmp(&transmitter, &before, sizeof(transmitter)) == 0 &&
	      memcmp(&symbol, &symbol_before, sizeof(symbol)) == 0,
	      "corrupt CPt state was not rejected transactionally");

	CHECK(v92_pcm_phase3_cpt_tx_init(
		      &transmitter, G711_LAW_PCMU, 91u, &parameters) ==
		      V92_PCM_PHASE3_CPT_TX_OK,
	      "CPt overflow fixture did not reset");
	transmitter.stage = V92_PCM_PHASE3_CPT_TX_RECORD;
	transmitter.trn1u.emitted_symbols =
		V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS;
	CHECK(pcm_binary_sign_init_from_scrambler(
		      &transmitter.coding, &transmitter.trn1u.scrambler,
		      (uint8_t)(transmitter.trn1u.scrambler.history & 1u)) ==
		      PCM_BINARY_SIGN_OK,
	      "CPt overflow coding fixture did not initialize");
	transmitter.preamble_symbols = V92_PCM_PHASE3_CPT_PREAMBLE_SYMBOLS;
	transmitter.completed_records = UINT32_MAX - 1u;
	transmitter.frame_bit_index = transmitter.frame_bit_count - 1u;
	before = transmitter;
	CHECK(v92_pcm_phase3_cpt_tx_is_valid(&transmitter) &&
	      v92_pcm_phase3_cpt_tx_next(&transmitter, &symbol) ==
		      V92_PCM_PHASE3_CPT_TX_INTERNAL_ERROR &&
	      memcmp(&transmitter, &before, sizeof(transmitter)) == 0 &&
	      memcmp(&symbol, &symbol_before, sizeof(symbol)) == 0,
	      "CPt record overflow was not transactional");

	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] V.92 PCM-upstream CPt transmitter\n");
	if (!test_generated_sequences() || !test_contracts()) {
		fprintf(stderr, "[  FAILED  ] V.92 PCM-upstream CPt transmitter\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] V.92 PCM-upstream CPt transmitter\n");
	return 0;
}
