// SPDX-License-Identifier: BSD-3-Clause

#include <v90_downstream_data_rx.h>
#include <v90_downstream_data_tx.h>
#include <v90_pcm_symbol.h>

#include "test.h"

#include <string.h>

enum {
	DATA_FRAMES = 6,
	TOTAL_FRAMES = V90_PHASE4_B1D_FRAMES + DATA_FRAMES,
	B1D_DAMAGE_FIRST = V90_PHASE4_B1D_FRAMES / 2,
	B1D_DAMAGE_END = V90_PHASE4_B1D_FRAMES - 4,
};

struct observer {
	struct v90_downstream_data_rx receiver;
	uint8_t expected[TOTAL_FRAMES][V90_PCM_FRAME_MAX_BITS];
	size_t accepted_frames;
	size_t received_frames;
	bool use_decisions;
	bool damage_b1d;
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
		struct v90_pcm_symbol received = output->symbols[symbol];
		enum v90_downstream_data_rx_result result;

		if (observer->damage_b1d &&
		    observer->received_frames >= B1D_DAMAGE_FIRST &&
		    observer->received_frames < B1D_DAMAGE_END)
			CHECK(v90_pcm_symbol_make(
				      &received,
				      observer->receiver.stream.decoder.mapping.law,
				      received.ucode, !received.positive),
			      "damaged B1d symbol was not representable");
		memset(&frame, 0xa5, sizeof(frame));
		untouched = frame;
		if (observer->use_decisions) {
			result = v90_downstream_data_rx_receive_decision(
				&observer->receiver,
				received.ucode, received.positive, &frame);
		} else {
			result = v90_downstream_data_rx_receive_octet(
				&observer->receiver,
				received.octet, &frame);
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
		CHECK(v90_downstream_data_rx_b1d_complete(&observer->receiver) ==
			      (observer->received_frames + 1u >= V90_PHASE4_B1D_FRAMES),
		      "B1d readiness changed at frame %zu", observer->received_frames);
		CHECK(frame.bit_count ==
			      observer->receiver.stream.decoder.mapping.frame_bits,
		      "decoded frame %zu changed length", observer->received_frames);
		/* A burst can corrupt subsequent descrambler output. Leave four
		 * clean B1d frames and require exact recovery by the final one;
		 * no payload frames are skipped or delayed to hide errors. */
		if (!observer->damage_b1d ||
		    observer->received_frames < B1D_DAMAGE_FIRST ||
		    observer->received_frames >= V90_PHASE4_B1D_FRAMES - 1u)
			CHECK(memcmp(frame.bits,
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
	bool use_decisions, bool damage_b1d)
{
	struct v90_pcm_mapping mapping;
	struct v90_downstream_data_tx transmitter;
	struct observer observer = {
		.use_decisions = use_decisions,
		.damage_b1d = damage_b1d,
	};
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
	      (observer.receiver.b1d_bit_errors > 0) == damage_b1d &&
	      v90_downstream_data_rx_b1d_complete(&observer.receiver) &&
	      v90_downstream_data_rx_is_valid(&observer.receiver),
	      "B1d/DATA boundary or accounting changed");
	return true;
}

static bool test_contracts(void)
{
	struct v90_pcm_mapping data_mapping;
	struct v90_pcm_mapping training_mapping;
	struct v90_downstream_data_rx receiver;
	struct v90_downstream_data_rx saved;
	union {
		struct v90_downstream_data_rx receiver;
		struct v90_pcm_stream_frame frame;
	} aliased;
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
	aliased.receiver = receiver;
	CHECK(v90_downstream_data_rx_receive_decision(
		      &aliased.receiver, 0, false, &aliased.frame) ==
		      V90_DOWNSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      v90_downstream_data_rx_receive_octet(
		      &aliased.receiver, 0, &aliased.frame) ==
		      V90_DOWNSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      memcmp(&aliased.receiver, &receiver, sizeof(receiver)) == 0,
	      "overlapping output changed downstream receiver state");
	memset(&frame, 0x5a, sizeof(frame));
	frame_saved = frame;
	CHECK(v90_pcm_symbol_make(
		      &invalid, data_mapping.law, 1, false) &&
	      v90_downstream_data_rx_receive_decision(
		      &receiver, invalid.ucode, invalid.positive, &frame) ==
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
	receiver.stream.decoder.mapping.frame_bits = 0;
	saved = receiver;
	CHECK(!v90_downstream_data_rx_is_valid(&receiver) &&
	      v90_downstream_data_rx_receive_decision(
		      &receiver, 0, true, &frame) ==
		      V90_DOWNSTREAM_DATA_RX_INVALID_STATE &&
	      v90_downstream_data_rx_receive_octet(
		      &receiver, 0, &frame) ==
		      V90_DOWNSTREAM_DATA_RX_INVALID_STATE &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      memcmp(&frame, &frame_saved, sizeof(frame)) == 0,
	      "invalid stream state changed receiver or output");
	receiver.stream.decoder.mapping.frame_bits = data_mapping.frame_bits;

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

static bool test_streams(void)
{
	for (unsigned damaged = 0; damaged < 2; ++damaged) {
		CHECK(run_stream(G711_LAW_PCMU, 3, 3, false, damaged),
		      "shaped PCMU octet stream failed (damage %u)", damaged);
		CHECK(run_stream(G711_LAW_PCMA, 3, 3, true, damaged),
		      "shaped PCMA decision stream failed (damage %u)", damaged);
		CHECK(run_stream(G711_LAW_PCMU, 0, 0, false, damaged),
		      "unshaped PCMU stream failed (damage %u)", damaged);
		CHECK(run_stream(G711_LAW_PCMA, 0, 0, false, damaged),
		      "unshaped PCMA stream failed (damage %u)", damaged);
	}
	CHECK(test_contracts(), "contract test failed");
	return true;
}

int main(void)
{
	return test_streams() ? 0 : 1;
}
