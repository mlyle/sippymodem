// SPDX-License-Identifier: BSD-3-Clause

#include <v90_downstream_data_rx.h>
#include <v90_downstream_data_tx.h>
#include <v90_pcm_symbol.h>

#include "test.h"

#include <string.h>

enum {
	DATA_FRAMES = 6,
	TOTAL_FRAMES = V90_PHASE4_B1D_FRAMES + DATA_FRAMES,
};

struct observer {
	struct v90_downstream_data_rx receiver;
	uint8_t expected[TOTAL_FRAMES][V90_PCM_FRAME_MAX_BITS];
	size_t accepted_frames;
	size_t received_frames;
	bool use_decisions;
};

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
	uint8_t constellation_size = kind == V90_CP_TRAINING ? 4u : 8u;
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
	};

	for (uint8_t ordinal = 0; ordinal < constellation_size; ++ordinal)
		add_ucode(
			&parameters.transmit_constellations[0],
			(uint8_t)(10u + ordinal));
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

static bool observe_output(
	struct observer *observer,
	const struct v90_pcm_frame_output *output)
{
	for (size_t symbol = 0; symbol < output->symbol_count; ++symbol) {
		struct v90_pcm_stream_frame frame;
		struct v90_pcm_stream_frame untouched;
		enum v90_downstream_data_rx_result result;

		memset(&frame, 0xa5, sizeof(frame));
		untouched = frame;
		if (observer->use_decisions) {
			result = v90_downstream_data_rx_receive_decision(
				&observer->receiver,
				output->symbols[symbol].ucode,
				output->symbols[symbol].positive, &frame);
		} else {
			result = v90_downstream_data_rx_receive_octet(
				&observer->receiver,
				output->symbols[symbol].octet, &frame);
		}
		if (result == V90_DOWNSTREAM_DATA_RX_BUFFERING) {
			CHECK(memcmp(&frame, &untouched, sizeof(frame)) == 0,
			      "buffering changed the decoded frame");
			continue;
		}
		CHECK(observer->received_frames < observer->accepted_frames,
		      "receiver emitted a frame before it was accepted");
		enum v90_downstream_data_rx_result expected_result;
		if (observer->received_frames + 1u <
		    V90_PHASE4_B1D_FRAMES) {
			expected_result = V90_DOWNSTREAM_DATA_RX_B1D_FRAME;
		} else if (observer->received_frames + 1u ==
			   V90_PHASE4_B1D_FRAMES) {
			expected_result = V90_DOWNSTREAM_DATA_RX_B1D_COMPLETE;
		} else {
			expected_result = V90_DOWNSTREAM_DATA_RX_DATA_FRAME;
		}
		CHECK(result == expected_result,
		      "frame %zu returned %s", observer->received_frames,
		      v90_downstream_data_rx_result_name(result));
		CHECK(frame.bit_count ==
			      observer->receiver.stream.decoder.mapping.frame_bits &&
		      memcmp(frame.bits,
			     observer->expected[observer->received_frames],
			     frame.bit_count) == 0,
		      "decoded frame %zu changed",
		      observer->received_frames);
		observer->received_frames++;
	}
	return true;
}

static bool run_stream(
	enum g711_law law, uint8_t redundancy, uint8_t lookahead,
	bool use_decisions)
{
	struct v90_pcm_mapping mapping;
	struct v90_downstream_data_tx transmitter;
	struct observer observer = { .use_decisions = use_decisions };
	uint8_t bits[V90_PCM_FRAME_MAX_BITS];

	CHECK(make_mapping(
		      &mapping, V90_CP_DATA, law, redundancy, lookahead) &&
	      v90_downstream_data_tx_init(&transmitter, &mapping) ==
		      V90_DOWNSTREAM_DATA_TX_OK &&
	      v90_downstream_data_rx_init(
		      &observer.receiver, &mapping,
		      V90_PCM_MAPPING_TRANSMIT) ==
		      V90_DOWNSTREAM_DATA_RX_OK,
	      "data stream did not initialize for law %d", law);
	for (size_t source_frame = 0;
	     source_frame < V90_PHASE4_B1D_FRAMES; ++source_frame) {
		struct v90_pcm_frame_output output;
		enum v90_downstream_data_tx_result result;

		memset(observer.expected[observer.accepted_frames], 1,
		       mapping.frame_bits);
		observer.accepted_frames++;
		result = v90_downstream_data_tx_next_b1d(
			&transmitter, &output);
		CHECK(result == V90_DOWNSTREAM_DATA_TX_BUFFERING ||
		      result == V90_DOWNSTREAM_DATA_TX_SYMBOLS_READY,
		      "B1d source frame %zu returned %s", source_frame,
		      v90_downstream_data_tx_result_name(result));
		if (result == V90_DOWNSTREAM_DATA_TX_SYMBOLS_READY)
			CHECK(observe_output(&observer, &output),
			      "B1d output was rejected");
	}
	CHECK(v90_downstream_data_tx_b1d_complete(&transmitter),
	      "transmitter did not accept all B1d frames");
	for (size_t source_frame = 0;
	     source_frame < DATA_FRAMES; ++source_frame) {
		struct v90_pcm_frame_output output;
		enum v90_downstream_data_tx_result result;

		generate_bits(bits, mapping.frame_bits, source_frame + 1u);
		memcpy(observer.expected[observer.accepted_frames], bits,
		       mapping.frame_bits);
		observer.accepted_frames++;
		result = v90_downstream_data_tx_process(
			&transmitter, bits, mapping.frame_bits, &output);
		CHECK(result == V90_DOWNSTREAM_DATA_TX_BUFFERING ||
		      result == V90_DOWNSTREAM_DATA_TX_SYMBOLS_READY,
		      "DATA source frame %zu returned %s", source_frame,
		      v90_downstream_data_tx_result_name(result));
		if (result == V90_DOWNSTREAM_DATA_TX_SYMBOLS_READY)
			CHECK(observe_output(&observer, &output),
			      "DATA output was rejected");
	}
	for (;;) {
		struct v90_pcm_frame_output output;
		enum v90_downstream_data_tx_result result =
			v90_downstream_data_tx_finish(&transmitter, &output);

		if (result == V90_DOWNSTREAM_DATA_TX_COMPLETE)
			break;
		CHECK(result == V90_DOWNSTREAM_DATA_TX_SYMBOLS_READY &&
		      observe_output(&observer, &output),
		      "tail output returned %s",
		      v90_downstream_data_tx_result_name(result));
	}
	CHECK(observer.received_frames == observer.accepted_frames &&
	      observer.received_frames == TOTAL_FRAMES &&
	      observer.receiver.b1d_frames == V90_PHASE4_B1D_FRAMES &&
	      observer.receiver.b1d_bit_errors == 0 &&
	      v90_downstream_data_rx_b1d_complete(&observer.receiver) &&
	      v90_downstream_data_rx_is_valid(&observer.receiver),
	      "B1d/DATA boundary or accounting changed");
	return true;
}

static bool test_b1d_error_accounting(void)
{
	struct v90_pcm_mapping mapping;
	struct v90_downstream_data_tx transmitter;
	struct v90_downstream_data_rx receiver;
	struct v90_pcm_frame_output output;
	struct v90_pcm_stream_frame frame;

	CHECK(make_mapping(
		      &mapping, V90_CP_DATA, G711_LAW_PCMU, 0, 0) &&
	      v90_downstream_data_tx_init(&transmitter, &mapping) ==
		      V90_DOWNSTREAM_DATA_TX_OK &&
	      v90_downstream_data_rx_init(
		      &receiver, &mapping, V90_PCM_MAPPING_TRANSMIT) ==
		      V90_DOWNSTREAM_DATA_RX_OK &&
	      v90_downstream_data_tx_next_b1d(&transmitter, &output) ==
		      V90_DOWNSTREAM_DATA_TX_SYMBOLS_READY,
	      "B1d error fixture did not initialize");
	for (size_t symbol = 0; symbol < output.symbol_count; ++symbol) {
		bool positive = output.symbols[symbol].positive;
		enum v90_downstream_data_rx_result result;

		if (symbol == 0)
			positive = !positive;
		result = v90_downstream_data_rx_receive_decision(
			&receiver, output.symbols[symbol].ucode,
			positive, &frame);
		CHECK(result == (symbol + 1u == output.symbol_count ?
					 V90_DOWNSTREAM_DATA_RX_B1D_FRAME :
					 V90_DOWNSTREAM_DATA_RX_BUFFERING),
		      "altered B1d symbol %zu returned %s", symbol,
		      v90_downstream_data_rx_result_name(result));
	}
	CHECK(receiver.b1d_bit_errors > 0 &&
	      receiver.b1d_frames == 1 &&
	      receiver.stage == V90_DOWNSTREAM_DATA_RX_STAGE_B1D &&
	      v90_downstream_data_rx_is_valid(&receiver),
	      "B1d errors changed timing or escaped diagnostics");
	return true;
}

static bool test_contracts(void)
{
	struct v90_pcm_mapping data_mapping;
	struct v90_pcm_mapping training_mapping;
	struct v90_downstream_data_rx receiver;
	struct v90_downstream_data_rx saved;
	struct v90_downstream_data_rx untouched;
	struct v90_downstream_data_rx untouched_saved;
	struct v90_pcm_stream_frame frame;
	struct v90_pcm_stream_frame frame_saved;
	struct v90_pcm_symbol invalid;

	CHECK(make_mapping(
		      &data_mapping, V90_CP_DATA, G711_LAW_PCMU, 0, 0) &&
	      make_mapping(
		      &training_mapping, V90_CP_TRAINING,
		      G711_LAW_PCMU, 0, 0),
	      "contract mappings did not initialize");
	memset(&untouched, 0xa5, sizeof(untouched));
	untouched_saved = untouched;
	CHECK(v90_downstream_data_rx_init(
		      NULL, &data_mapping, V90_PCM_MAPPING_TRANSMIT) ==
		      V90_DOWNSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      v90_downstream_data_rx_init(
		      &untouched, NULL, V90_PCM_MAPPING_TRANSMIT) ==
		      V90_DOWNSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      v90_downstream_data_rx_init(
		      &untouched, &training_mapping,
		      V90_PCM_MAPPING_TRANSMIT) ==
		      V90_DOWNSTREAM_DATA_RX_INVALID_PARAMETERS &&
	      v90_downstream_data_rx_init(
		      &untouched, &data_mapping,
		      (enum v90_pcm_mapping_plane)99) ==
		      V90_DOWNSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      memcmp(&untouched, &untouched_saved, sizeof(untouched)) == 0,
	      "rejected initialization changed its destination");
	CHECK(v90_downstream_data_rx_init(
		      &receiver, &data_mapping,
		      V90_PCM_MAPPING_TRANSMIT) ==
		      V90_DOWNSTREAM_DATA_RX_OK &&
	      !v90_downstream_data_rx_b1d_complete(&receiver),
	      "contract receiver did not initialize");
	saved = receiver;
	memset(&frame, 0x5a, sizeof(frame));
	frame_saved = frame;
	CHECK(v90_pcm_symbol_make(
		      &invalid, data_mapping.law, 1, false) &&
	      v90_downstream_data_rx_receive_octet(
		      &receiver, invalid.octet, &frame) ==
		      V90_DOWNSTREAM_DATA_RX_INVALID_SYMBOL &&
	      v90_downstream_data_rx_receive_decision(
		      NULL, 0, false, &frame) ==
		      V90_DOWNSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      v90_downstream_data_rx_receive_octet(
		      &receiver, 0, NULL) ==
		      V90_DOWNSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      memcmp(&frame, &frame_saved, sizeof(frame)) == 0,
	      "rejected input changed receiver or output");
	receiver.stage = (enum v90_downstream_data_rx_stage)99;
	CHECK(!v90_downstream_data_rx_is_valid(&receiver) &&
	      !v90_downstream_data_rx_is_valid(NULL) &&
	      !v90_downstream_data_rx_b1d_complete(NULL) &&
	      strcmp(v90_downstream_data_rx_result_name(
		     V90_DOWNSTREAM_DATA_RX_B1D_COMPLETE),
		     "B1d complete") == 0 &&
	      strcmp(v90_downstream_data_rx_result_name(
		     (enum v90_downstream_data_rx_result)99),
		     "unknown") == 0,
	      "state or result-name contract changed");
	return true;
}

int main(void)
{
	CHECK(run_stream(G711_LAW_PCMU, 3, 3, false),
	      "shaped PCMU octet stream failed");
	CHECK(run_stream(G711_LAW_PCMA, 3, 3, true),
	      "shaped PCMA decision stream failed");
	CHECK(run_stream(G711_LAW_PCMU, 0, 0, false),
	      "unshaped PCMU stream failed");
	CHECK(test_b1d_error_accounting(),
	      "B1d error accounting failed");
	CHECK(test_contracts(), "contract test failed");
	return 0;
}
