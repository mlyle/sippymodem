// SPDX-License-Identifier: BSD-3-Clause

#include <v90_downstream_data_tx.h>
#include <v90_pcm_frame_decoder.h>

#include "test.h"

#include <float.h>
#include <stdint.h>
#include <string.h>

enum {
	DATA_FRAMES = 8,
	TOTAL_FRAMES = V90_PHASE4_B1D_FRAMES + DATA_FRAMES,
};

struct receiver {
	struct v90_pcm_frame_decoder decoder;
	struct v34_scrambler descrambler;
	struct v90_pcm_symbol pending[V90_CP_FRAME_INTERVALS];
	uint8_t pending_count;
	uint8_t frames[TOTAL_FRAMES][V90_PCM_FRAME_MAX_BITS];
	uint8_t frame_count;
};

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static bool make_mapping(
	struct v90_pcm_mapping *mapping, enum g711_law law,
	uint8_t redundancy, uint8_t lookahead)
{
	struct v90_cp_parameters parameters = {
		.kind = V90_CP_DATA,
		.law = law,
		.shaping_a1_q1_6 = 17,
		.shaping_a2_q1_6 = -23,
		.shaping_b1_q1_6 = 39,
		.shaping_b2_q1_6 = -7,
		.selected_rate_index = 1,
		.sign_redundancy = redundancy,
		.spectral_lookahead = lookahead,
		.constellation_count = 1,
	};

	for (uint8_t ordinal = 0; ordinal < 8; ++ordinal)
		add_ucode(
			&parameters.transmit_constellations[0],
			(uint8_t)(10 + ordinal));
	return v90_pcm_mapping_init(mapping, &parameters) ==
	       V90_PCM_MAPPING_OK;
}

static void generate_bits(
	uint8_t *bits, size_t bit_count, size_t frame)
{
	for (size_t bit = 0; bit < bit_count; ++bit)
		bits[bit] = (uint8_t)(
			(frame * 13u + bit * 7u +
			 (frame >> (bit % 3u))) & 1u);
}

static void force_numeric_failure(
	struct v90_downstream_data_tx *transmitter)
{
	transmitter->encoder.mapping.shaping_a2_q1_6 = 64;
	transmitter->encoder.shaper.shaping_a2_q1_6 = 64;
	transmitter->encoder.shaper.previous_v = DBL_MAX;
}

static bool receiver_init(
	struct receiver *receiver,
	const struct v90_pcm_mapping *mapping)
{
	struct receiver initialized = { 0 };

	if (v90_pcm_frame_decoder_init(
		    &initialized.decoder, mapping,
		    V90_PCM_MAPPING_TRANSMIT) !=
		    V90_PCM_FRAME_DECODER_OK ||
	    v34_scrambler_init(
		    &initialized.descrambler, V34_POLYNOMIAL_GPC) !=
		    V34_FRAMING_OK)
		return false;
	*receiver = initialized;
	return true;
}

static bool receive_frame(struct receiver *receiver)
{
	struct v90_pcm_frame_decode_output decoded;
	uint8_t frame[V90_PCM_FRAME_MAX_BITS] = { 0 };
	uint8_t ucodes[V90_CP_FRAME_INTERVALS];
	uint8_t signs[V90_CP_FRAME_INTERVALS];

	if (receiver->frame_count >= TOTAL_FRAMES)
		return false;
	for (size_t symbol = 0;
	     symbol < V90_CP_FRAME_INTERVALS; ++symbol) {
		ucodes[symbol] = receiver->pending[symbol].ucode;
		signs[symbol] = receiver->pending[symbol].positive ? 1u : 0u;
	}
	if (v90_pcm_frame_decoder_decode(
		    &receiver->decoder, ucodes, signs,
		    V90_CP_FRAME_INTERVALS, &decoded) !=
	    V90_PCM_FRAME_DECODER_OK)
		return false;
	for (size_t bit = 0; bit < decoded.bit_count; ++bit) {
		if (v34_descramble_group(
			    &receiver->descrambler, decoded.bits[bit], 1,
			    &frame[bit]) != V34_FRAMING_OK)
			return false;
	}
	memcpy(receiver->frames[receiver->frame_count++], frame,
	       decoded.bit_count);
	receiver->pending_count = 0;
	memset(receiver->pending, 0, sizeof(receiver->pending));
	return true;
}

static bool receive_output(
	struct receiver *receiver,
	const struct v90_pcm_frame_output *output)
{
	if (output->symbol_count > V90_CP_FRAME_INTERVALS)
		return false;
	for (size_t symbol = 0; symbol < output->symbol_count; ++symbol) {
		receiver->pending[receiver->pending_count++] =
			output->symbols[symbol];
		if (receiver->pending_count == V90_CP_FRAME_INTERVALS &&
		    !receive_frame(receiver))
			return false;
	}
	return true;
}

static bool run_stream(
	enum g711_law law, uint8_t redundancy, uint8_t lookahead)
{
	struct v90_pcm_mapping mapping;
	struct v90_downstream_data_tx transmitter;
	struct receiver receiver;
	uint8_t expected[TOTAL_FRAMES][V90_PCM_FRAME_MAX_BITS] = { 0 };
	uint8_t data[V90_PCM_FRAME_MAX_BITS];
	uint8_t accepted = 0;

	CHECK(make_mapping(&mapping, law, redundancy, lookahead) &&
	      v90_downstream_data_tx_init(&transmitter, &mapping) ==
		      V90_DOWNSTREAM_DATA_TX_OK &&
	      receiver_init(&receiver, &mapping) &&
	      transmitter.scrambler.history == 0,
	      "data epoch did not initialize for law %d", law);
	for (size_t frame = 0;
	     frame < V90_PHASE4_B1D_FRAMES; ++frame) {
		struct v90_pcm_frame_output output;
		struct v90_pcm_frame_output untouched;
		enum v90_downstream_data_tx_result result;

		memset(expected[accepted], 1, mapping.frame_bits);
		memset(&untouched, 0xa5, sizeof(untouched));
		output = untouched;
		result = v90_downstream_data_tx_next_b1d(
			&transmitter, &output);
		CHECK(result == V90_DOWNSTREAM_DATA_TX_BUFFERING ||
		      result == V90_DOWNSTREAM_DATA_TX_SYMBOLS_READY,
		      "B1d frame %zu failed: %d", frame,
		      (int)result);
		accepted++;
		if (result == V90_DOWNSTREAM_DATA_TX_BUFFERING)
			CHECK(memcmp(&output, &untouched, sizeof(output)) == 0,
			      "buffered B1d frame changed output");
		else
			CHECK(receive_output(&receiver, &output),
			      "B1d output %zu did not decode", frame);
		CHECK(v90_downstream_data_tx_b1d_complete(&transmitter) ==
			      (frame + 1 == V90_PHASE4_B1D_FRAMES),
		      "B1d handoff occurred on frame %zu", frame);
	}
	for (size_t frame = 0; frame < DATA_FRAMES; ++frame) {
		struct v90_pcm_frame_output output;
		struct v90_pcm_frame_output untouched;
		enum v90_downstream_data_tx_result result;

		generate_bits(data, mapping.frame_bits, frame + 1);
		memcpy(expected[accepted], data, mapping.frame_bits);
		memset(&untouched, 0x5a, sizeof(untouched));
		output = untouched;
		result = v90_downstream_data_tx_process(
			&transmitter, data, mapping.frame_bits, &output);
		CHECK(result == V90_DOWNSTREAM_DATA_TX_BUFFERING ||
		      result == V90_DOWNSTREAM_DATA_TX_SYMBOLS_READY,
		      "DATA frame %zu failed: %d", frame,
		      (int)result);
		accepted++;
		if (result == V90_DOWNSTREAM_DATA_TX_BUFFERING)
			CHECK(memcmp(&output, &untouched, sizeof(output)) == 0,
			      "buffered DATA frame changed output");
		else
			CHECK(receive_output(&receiver, &output),
			      "DATA output %zu did not decode", frame);
	}
	for (;;) {
		struct v90_pcm_frame_output output;
		enum v90_downstream_data_tx_result result =
			v90_downstream_data_tx_finish(
				&transmitter, &output);

		if (result == V90_DOWNSTREAM_DATA_TX_COMPLETE)
			break;
		CHECK(result == V90_DOWNSTREAM_DATA_TX_SYMBOLS_READY &&
		      receive_output(&receiver, &output),
		      "tail output failed: %d",
		      (int)result);
	}
	CHECK(v90_downstream_data_tx_is_complete(&transmitter) &&
	      receiver.pending_count == 0 &&
	      receiver.frame_count == accepted,
	      "stream ended with %u/%u frames and %u pending symbols",
	      receiver.frame_count, accepted, receiver.pending_count);
	for (size_t frame = 0; frame < accepted; ++frame)
		CHECK(memcmp(
			      receiver.frames[frame], expected[frame],
			      mapping.frame_bits) == 0,
		      "frame %zu changed across the B1d/DATA epoch", frame);
	return true;
}

static bool test_contract(void)
{
	struct v90_pcm_mapping data_mapping;
	struct v90_pcm_mapping training_mapping;
	struct v90_downstream_data_tx transmitter;
	struct v90_downstream_data_tx saved;
	struct v90_downstream_data_tx numeric;
	union {
		struct v90_downstream_data_tx transmitter;
		struct v90_pcm_frame_output output;
	} aliased;
	struct v90_pcm_frame_output output;
	struct v90_pcm_frame_output untouched;
	uint8_t bits[V90_PCM_FRAME_MAX_BITS] = { 0 };

	CHECK(make_mapping(&data_mapping, G711_LAW_PCMU, 1, 1) &&
	      make_mapping(&training_mapping, G711_LAW_PCMU, 1, 1),
	      "contract mappings did not initialize");
	training_mapping.kind = V90_CP_TRAINING;
	CHECK(v90_downstream_data_tx_init(NULL, &data_mapping) ==
		      V90_DOWNSTREAM_DATA_TX_INVALID_ARGUMENT &&
	      v90_downstream_data_tx_init(&transmitter, NULL) ==
		      V90_DOWNSTREAM_DATA_TX_INVALID_ARGUMENT &&
	      v90_downstream_data_tx_init(
		      &transmitter, &training_mapping) ==
		      V90_DOWNSTREAM_DATA_TX_INVALID_PARAMETERS &&
	      v90_downstream_data_tx_init(
		      &transmitter, &data_mapping) ==
		      V90_DOWNSTREAM_DATA_TX_OK,
	      "initialization contract failed");
	saved = transmitter;
	memset(&untouched, 0xa5, sizeof(untouched));
	output = untouched;
	aliased.transmitter = transmitter;
	CHECK(v90_downstream_data_tx_next_b1d(
		      &aliased.transmitter, &aliased.output) ==
		      V90_DOWNSTREAM_DATA_TX_INVALID_ARGUMENT &&
	      memcmp(&aliased.transmitter, &transmitter,
		     sizeof(transmitter)) == 0,
	      "overlapping B1d output changed state");
	CHECK(v90_downstream_data_tx_process(
		      &transmitter, bits, data_mapping.frame_bits,
		      &output) == V90_DOWNSTREAM_DATA_TX_INVALID_STATE &&
	      v90_downstream_data_tx_finish(&transmitter, &output) ==
		      V90_DOWNSTREAM_DATA_TX_INVALID_STATE &&
	      memcmp(&transmitter, &saved, sizeof(saved)) == 0 &&
	      memcmp(&output, &untouched, sizeof(output)) == 0,
	      "pre-B1d rejection changed state or output");
	for (size_t frame = 0;
	     frame < V90_PHASE4_B1D_FRAMES; ++frame) {
		enum v90_downstream_data_tx_result result =
			v90_downstream_data_tx_next_b1d(
				&transmitter, &output);

		CHECK(result == V90_DOWNSTREAM_DATA_TX_BUFFERING ||
		      result == V90_DOWNSTREAM_DATA_TX_SYMBOLS_READY,
		      "contract B1d frame failed");
	}
	saved = transmitter;
	aliased.transmitter = transmitter;
	CHECK(v90_downstream_data_tx_process(
		      &aliased.transmitter, bits, data_mapping.frame_bits,
		      &aliased.output) ==
		      V90_DOWNSTREAM_DATA_TX_INVALID_ARGUMENT &&
	      v90_downstream_data_tx_finish(
		      &aliased.transmitter, &aliased.output) ==
		      V90_DOWNSTREAM_DATA_TX_INVALID_ARGUMENT &&
	      memcmp(&aliased.transmitter, &transmitter,
		     sizeof(transmitter)) == 0,
	      "overlapping DATA output changed state");
	bits[0] = 2;
	output = untouched;
	CHECK(v90_downstream_data_tx_next_b1d(
		      &transmitter, &output) ==
		      V90_DOWNSTREAM_DATA_TX_INVALID_STATE &&
	      v90_downstream_data_tx_process(
		      &transmitter, bits, data_mapping.frame_bits,
		      &output) == V90_DOWNSTREAM_DATA_TX_INVALID_BITS &&
	      memcmp(&transmitter, &saved, sizeof(saved)) == 0 &&
	      memcmp(&output, &untouched, sizeof(output)) == 0,
	      "DATA rejection changed state or output");
	bits[0] = 0;
	numeric = transmitter;
	force_numeric_failure(&numeric);
	saved = numeric;
	CHECK(v90_downstream_data_tx_is_valid(&numeric) &&
	      v90_downstream_data_tx_finish(&numeric, &output) ==
		      V90_DOWNSTREAM_DATA_TX_NUMERIC_FAILURE &&
	      memcmp(&numeric, &saved, sizeof(saved)) == 0 &&
	      memcmp(&output, &untouched, sizeof(output)) == 0,
	      "numeric finish advanced encoder or stage");
	CHECK(make_mapping(&data_mapping, G711_LAW_PCMU, 1, 0) &&
	      v90_downstream_data_tx_init(&numeric, &data_mapping) ==
		      V90_DOWNSTREAM_DATA_TX_OK,
	      "numeric fixture did not initialize");
	force_numeric_failure(&numeric);
	saved = numeric;
	CHECK(v90_downstream_data_tx_is_valid(&numeric) &&
	      v90_downstream_data_tx_next_b1d(&numeric, &output) ==
		      V90_DOWNSTREAM_DATA_TX_NUMERIC_FAILURE &&
	      memcmp(&numeric, &saved, sizeof(saved)) == 0 &&
	      memcmp(&output, &untouched, sizeof(output)) == 0,
	      "numeric B1d advanced encoder, scrambler, or frame count");
	CHECK(v90_downstream_data_tx_init(&numeric, &data_mapping) ==
		      V90_DOWNSTREAM_DATA_TX_OK,
	      "numeric fixture reset failed");
	for (size_t frame = 0; frame < V90_PHASE4_B1D_FRAMES; ++frame)
		CHECK(v90_downstream_data_tx_next_b1d(&numeric, &output) ==
			      V90_DOWNSTREAM_DATA_TX_SYMBOLS_READY,
		      "numeric fixture B1d failed");
	force_numeric_failure(&numeric);
	output = untouched;
	saved = numeric;
	CHECK(v90_downstream_data_tx_is_valid(&numeric) &&
	      v90_downstream_data_tx_process(
		      &numeric, bits, data_mapping.frame_bits, &output) ==
		      V90_DOWNSTREAM_DATA_TX_NUMERIC_FAILURE &&
	      memcmp(&numeric, &saved, sizeof(saved)) == 0 &&
	      memcmp(&output, &untouched, sizeof(output)) == 0,
	      "numeric failure advanced encoder or scrambler");
	return true;
}

int main(void)
{
	return run_stream(G711_LAW_PCMU, 3, 3) &&
	       run_stream(G711_LAW_PCMA, 3, 3) &&
	       run_stream(G711_LAW_PCMU, 0, 0) && test_contract() ? 0 : 1;
}
