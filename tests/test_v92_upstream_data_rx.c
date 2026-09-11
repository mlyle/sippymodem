// SPDX-License-Identifier: BSD-3-Clause

#include <pcm_downstream_rx.h>
#include <v92_cpd_direct.h>
#include <v92_cpd_channel.h>
#include <v92_data_quality.h>
#include <v92_e2u_rx.h>
#include <v92_phase4_digital_station.h>
#include <v92_phase4_record_tx.h>
#include <v92_ru_line_rx.h>
#include <v92_upstream_control_line_rx.h>
#include <v92_upstream_control.h>
#include <v92_upstream_data_rx.h>
#include <v92_upstream_data_tx.h>

#include "test.h"
#include "packed_bits.h"

#include <float.h>
#include <math.h>
#include <string.h>

enum { TEST_LU = 4096, DATA_FRAMES = 80 };

static const struct v92_upstream_plan direct_plan;

struct channel {
	double gain;
	double postcursor;
	/* Zero keeps the original codec output; nonzero scales after G.711. */
	double receive_gain;
	int16_t previous;
	enum g711_law law;
	int8_t polarity;
};

static int16_t wire_sample(struct channel *channel, int16_t sample)
{
	int16_t mixed = (int16_t)lround(channel->gain * sample +
		channel->postcursor * channel->previous);

	channel->previous = sample;
	int16_t decoded = channel->law == G711_LAW_PCMU ?
		g711_decode_pcmu(g711_encode_pcmu(mixed)) :
		g711_decode_pcma(g711_encode_pcma(mixed));
	return channel->polarity * (channel->receive_gain != 0.0 ?
		(int16_t)lround(decoded * channel->receive_gain) : decoded);
}

static void payload(uint8_t *bits, size_t count, unsigned int frame)
{
	uint32_t random = UINT32_C(0x39458672) ^ frame;

	for (size_t bit = 0u; bit < count; ++bit) {
		random = random * UINT32_C(1664525) + UINT32_C(1013904223);
		bits[bit] = (uint8_t)(random >> 31);
	}
}

static bool source(void *context, uint8_t *bits, size_t count)
{
	unsigned int *frame = context;

	payload(bits, count, (*frame)++);
	return true;
}

static bool receive_compared(
	struct v92_upstream_data_rx *receiver, int16_t sample,
	struct v92_upstream_data_frame *frame,
	enum v92_upstream_data_rx_result *result)
{
	struct v92_upstream_data_rx owned = *receiver;
	struct v92_upstream_data_frame output;

	memcpy(&output, frame, sizeof(output));
	CHECK(v92_upstream_data_rx_is_valid(receiver), "owned fixture is not validated");
	*result = v92_upstream_data_rx_sample(receiver, sample, frame);
	CHECK(v92_upstream_data_rx_sample_owned(&owned, sample, &output) == *result &&
	      memcmp(receiver, &owned, sizeof(owned)) == 0 &&
	      memcmp(frame, &output, sizeof(output)) == 0 &&
	      v92_upstream_data_rx_is_valid(&owned),
	      "owned/checked receive changed result, output, state, or invariants");
	return true;
}

/* A trained FIR with the real causal delay, not a zero-delay stand-in. Feed
 * E2u's first delayed samples before exporting its receive-side boundary. */
static bool prepare_e2u_at_level(
	struct channel *channel, bool extended,
	struct v92_phase4_record_tx *transmitter, struct v92_e2u_rx *receiver,
	int16_t lu, int16_t measured_lu, double adaptation,
	enum v92_constellation_size constellation)
{
	struct v92_phase4_modulator modulator;
	struct v92_pcm_phase4_trn2u_line_handoff handoff = {
		.input_samples = V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS,
		.training_symbols = V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS,
		.adaptation = adaptation,
		.law = channel->law,
		.polarity = channel->polarity,
	};
	struct v92_upstream_rx signal;
	struct qam_point reference = { .in_phase = measured_lu };
	struct qam_point output;
	int16_t level;
	bool ready;

	CHECK(v92_phase4_modulator_init(&modulator, constellation,
			0u, lu) == V92_PHASE4_MODULATOR_OK &&
	      v92_phase4_demodulator_init(&handoff.demodulator,
			constellation, 0u, measured_lu) == V92_PHASE4_MODULATOR_OK &&
	      v92_phase4_record_tx_init_e2u(transmitter, &modulator, extended) ==
		      V92_PHASE4_RECORD_TX_OK &&
	      qam_equalizer_init_geometry(&handoff.equalizer, &reference, &reference,
			V92_PCM_PHASE4_TRN2U_LINE_EQUALIZER_TAPS,
			V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY) == QAM_EQUALIZER_OK,
	      "E2u fixture initialization failed");
	/* Finite inverse of the known two-tap channel; B1u continues adaptation. */
	double coefficient = channel->receive_gain != 0.0 ? 1.0 / channel->receive_gain : 1.0;
	for (size_t tap = V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY;
	     tap < handoff.equalizer.tap_count; ++tap) {
		handoff.equalizer.coefficients[tap].in_phase = coefficient;
		coefficient *= -channel->postcursor / channel->gain;
	}
	for (size_t index = 0u; index <= V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY; ++index) {
		int16_t sample = lu;
		struct qam_point input;

		if (index != 0u)
			CHECK(v92_phase4_record_tx_next(transmitter, &sample) ==
				      V92_PHASE4_RECORD_TX_OK, "E2u lookahead failed");
		input = (struct qam_point){
			.in_phase = channel->polarity * wire_sample(channel, sample),
		};
		CHECK(qam_equalizer_push(&handoff.equalizer, &input, &output, &ready) ==
			      QAM_EQUALIZER_OK, "FIR fill failed");
	}
	CHECK(ready && qam_equalizer_adapt(&handoff.equalizer, &output, 0.0) ==
		      QAM_EQUALIZER_OK &&
	      v92_phase4_modulator_level(constellation, measured_lu, 0u, 0u,
			&level) == V92_PHASE4_MODULATOR_OK, "FIR did not become warm");
	level = channel->law == G711_LAW_PCMU ?
		g711_decode_pcmu(g711_encode_pcmu(level)) :
		g711_decode_pcma(g711_encode_pcma(level));
	for (size_t index = 0u; index < V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY; ++index)
		handoff.pending_training_targets[index] = level;
	CHECK(v92_upstream_rx_init(&signal, &handoff) == V92_UPSTREAM_RX_OK,
	      "line initialization failed");
	signal.training_tail_remaining = 0u;
	CHECK(v92_e2u_rx_init(receiver, &signal, extended) == V92_E2U_RX_OK,
	      "E2u receiver initialization failed");
	return true;
}

static bool prepare_e2u(
	struct channel *channel, bool extended,
	struct v92_phase4_record_tx *transmitter, struct v92_e2u_rx *receiver)
{
	return prepare_e2u_at_level(channel, extended, transmitter, receiver,
		TEST_LU, (int16_t)lround(TEST_LU * channel->gain), 0.002, V92_CONSTELLATION_4_POINT);
}

struct idle_source {
	unsigned int frames;
	unsigned int idle_frames;
};

static bool after_idle(void *context, uint8_t *bits, size_t count)
{
	struct idle_source *source = context;

	if (source->frames < source->idle_frames)
		memset(bits, 1, count);
	else
		payload(bits, count, source->frames - source->idle_frames);
	source->frames++;
	return true;
}

static bool test_quantized_level_tracking(
	enum g711_law law, uint8_t rate, bool drift, int16_t lu, bool select)
{
	enum { IDLE_FRAMES = 2000, PAYLOAD_FRAMES = 400 };
	struct channel channel = { .gain = 1.0, .law = law, .polarity = 1 };
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct v92_phase4_record_tx e2u_tx;
	struct v92_e2u_rx e2u_rx;
	struct v92_upstream_rx handoff;
	struct v92_upstream_data_tx transmitter;
	struct v92_upstream_data_rx receiver;
	struct idle_source source = { .idle_frames = IDLE_FRAMES };
	unsigned int delivered = 0u;
	double b1u_error = 0.0;
	int16_t trained_lu = 0;
	int16_t trained_minimum = 0, trained_maximum = 0;
	int16_t measured_lu = law == G711_LAW_PCMU ?
		g711_decode_pcmu(g711_encode_pcmu(lu)) :
		g711_decode_pcma(g711_encode_pcma(lu));

	/* TRN1u estimates LU from quantized samples, not the transmitter's
	 * exact scalar. Carry the fast training step through real E2u, then
	 * require usable B1u and clean idle/payload. Dense 40k/48k cases check
	 * quantization stability; the lower-rate case checks ongoing tracking. */
	CHECK((select ? v92_cpd_direct_select(&cpd, law, rate, measured_lu, false) :
			v92_cpd_direct_init(&cpd, law, rate, false)) == V92_CPD_OK &&
	      v92_pcm_mapping_init(&mapping, law, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_upstream_data_tx_init(&transmitter, &mapping, lu) ==
		      V92_UPSTREAM_DATA_TX_OK &&
	      prepare_e2u_at_level(&channel, false, &e2u_tx, &e2u_rx,
			lu, measured_lu, 0.05, V92_CONSTELLATION_4_POINT), "tracking fixture failed");
	while (!v92_e2u_rx_is_complete(&e2u_rx)) {
		int16_t sample;
		enum v92_e2u_rx_result result;

		if (!v92_phase4_record_tx_is_complete(&e2u_tx))
			CHECK(v92_phase4_record_tx_next(&e2u_tx, &sample) ==
				      V92_PHASE4_RECORD_TX_OK, "tracking E2u stopped");
		else
			CHECK(v92_upstream_data_tx_next(&transmitter, after_idle, &source,
					&sample) == V92_UPSTREAM_DATA_TX_OK, "tracking B1u stopped");
		result = v92_e2u_rx_sample(&e2u_rx, wire_sample(&channel, sample));
		CHECK(result == V92_E2U_RX_OK || result == V92_E2U_RX_COMPLETE,
		      "tracking E2u did not match");
	}
	CHECK(v92_e2u_rx_handoff(&e2u_rx, &handoff) == V92_E2U_RX_OK &&
	      v92_upstream_data_rx_init(&receiver, &handoff, &mapping, &direct_plan) ==
		      V92_UPSTREAM_DATA_RX_OK &&
	      receiver.line.adaptation > 0.0 &&
	      receiver.line.adaptation < handoff.line.adaptation &&
	      memcmp(&receiver.line.equalizer, &handoff.line.equalizer,
		      sizeof(handoff.line.equalizer)) == 0,
	      "tracking handoff changed learned coefficients or pending inputs");
	for (unsigned int symbol = 0u;
	     symbol < (V92_B1U_FRAMES + IDLE_FRAMES + PAYLOAD_FRAMES +
		       V92_PCM_RX_DELAY_FRAMES) * V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		int16_t sample;
		struct v92_upstream_data_frame frame;
		enum v92_upstream_data_rx_result result;

		/* Slow postcursor drift requires continuing adaptation; freezing
		 * the learned FIR is not a substitute for stable data tracking. */
		if (drift)
			channel.postcursor = 0.02 * symbol /
				((V92_B1U_FRAMES + IDLE_FRAMES + PAYLOAD_FRAMES) *
				 V92_PCM_MAPPING_FRAME_SYMBOLS);
		CHECK(v92_upstream_data_tx_next(&transmitter, after_idle, &source,
				&sample) == V92_UPSTREAM_DATA_TX_OK, "tracking transmitter stopped");
		result = v92_upstream_data_rx_sample(&receiver,
			wire_sample(&channel, sample), &frame);
		CHECK(result == V92_UPSTREAM_DATA_RX_OK || result == V92_UPSTREAM_DATA_RX_FRAME,
		      "tracking receiver stopped");
		if (result == V92_UPSTREAM_DATA_RX_FRAME &&
		    receiver.line.received_frames <= V92_B1U_FRAMES)
			b1u_error += frame.squared_error;
		if (receiver.line.received_frames == V92_B1U_FRAMES) {
			trained_lu = receiver.decoder.upstream_level;
			trained_minimum = receiver.decoder.minimum_level;
			trained_maximum = receiver.decoder.maximum_level;
		}
		if (receiver.line.received_frames > V92_B1U_FRAMES)
			CHECK(receiver.decoder.upstream_level == trained_lu &&
			      receiver.decoder.minimum_level == trained_minimum &&
			      receiver.decoder.maximum_level == trained_maximum,
			      "unknown payload changed the trained LU");
		if (result != V92_UPSTREAM_DATA_RX_FRAME || frame.bit_count == 0u)
			continue;
		uint8_t expected[V92_PCM_MAPPING_MAX_BITS];

		if (delivered < V92_B1U_FRAMES + IDLE_FRAMES)
			memset(expected, 1, mapping.data_bits_per_frame);
		else
			payload(expected, mapping.data_bits_per_frame,
				delivered - V92_B1U_FRAMES - IDLE_FRAMES);
		CHECK(!frame.ambiguous && frame.bit_count == mapping.data_bits_per_frame &&
		      memcmp(expected, frame.bits, frame.bit_count) == 0,
		      "law %u rate %u drift %u frame %u changed data: LU=%d ambiguous=%u error=%.2f",
		      (unsigned int)law, rate, drift, delivered, receiver.decoder.upstream_level,
		      frame.ambiguous, frame.squared_error);
		delivered++;
	}
	CHECK(delivered == V92_B1U_FRAMES + IDLE_FRAMES + PAYLOAD_FRAMES,
	      "tracking lost payload frames");
	CHECK(trained_minimum <= lu && trained_maximum >= lu,
	      "training excluded the transmitted LU");
	CHECK(b1u_error <= (double)V92_B1U_SYMBOLS *
		      V92_DATA_MAX_RMS_ERROR * V92_DATA_MAX_RMS_ERROR,
	      "law %u rate %u rejected clean quantized B1u: RMS %.2f",
	      (unsigned int)law, rate, sqrt(b1u_error / V92_B1U_SYMBOLS));
	return true;
}

enum stream_control { STREAM_DATA, STREAM_RESET, STREAM_RM };

static bool run_stream(enum g711_law law, int8_t polarity, bool extended,
		       uint8_t rate, double gain, double postcursor, enum stream_control control)
{
	enum { RM_START = V92_B1U_FRAMES + 16, RM_PRIME = RM_START + 32,
		RM_END = RM_PRIME + 2 };
	bool rm = control == STREAM_RM, requested = false;
	struct channel channel = { .gain = gain, .postcursor = postcursor,
		.law = law, .polarity = polarity };
	bool distorted = gain != 1.0 || postcursor != 0.0;
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct v92_phase4_record_tx e2u_tx;
	struct v92_e2u_rx e2u_rx;
	struct v92_upstream_rx handoff;
	struct v92_upstream_data_tx transmitter;
	struct v92_upstream_data_rx receiver;
	unsigned int source_frames = 0u;
	unsigned int delivered = 0u;
	unsigned int decisions = 0u;
	double b1u_error = 0.0;
	struct v92_pcm_frame_tx b1u_reference;
	struct v92_pcm_frame expected_b1u;
	uint8_t ones[V92_PCM_MAPPING_MAX_BITS];

	CHECK(v92_cpd_direct_init(&cpd, law, rate, extended) == V92_CPD_OK &&
	      v92_pcm_mapping_init(&mapping, law, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_upstream_data_tx_init(&transmitter, &mapping, TEST_LU) ==
		      V92_UPSTREAM_DATA_TX_OK &&
	      prepare_e2u(&channel, extended, &e2u_tx, &e2u_rx), "fixture failed");
	memset(ones, 1, mapping.data_bits_per_frame);
	CHECK(v92_pcm_frame_tx_init(&b1u_reference, &mapping) == V92_PCM_FRAME_OK,
	      "B1u reference initialization failed");
	for (unsigned int frame = 0u; frame < V92_B1U_FRAMES; ++frame)
		CHECK(v92_pcm_frame_encode(&b1u_reference, ones, mapping.data_bits_per_frame,
				&expected_b1u) == V92_PCM_FRAME_OK, "B1u reference stopped");
	while (!v92_e2u_rx_is_complete(&e2u_rx)) {
		int16_t sample;
		enum v92_e2u_rx_result result;

		if (!v92_phase4_record_tx_is_complete(&e2u_tx))
			CHECK(v92_phase4_record_tx_next(&e2u_tx, &sample) ==
				      V92_PHASE4_RECORD_TX_OK, "E2u transmitter stopped");
		else
			CHECK(v92_upstream_data_tx_next(&transmitter, source, &source_frames,
					&sample) == V92_UPSTREAM_DATA_TX_OK, "B1u lookahead stopped");
		result = v92_e2u_rx_sample(&e2u_rx, wire_sample(&channel, sample));
		CHECK(result == V92_E2U_RX_OK || result == V92_E2U_RX_COMPLETE,
		      "law %u distorted %u E2u did not match: %s", (unsigned int)law,
		      distorted, v92_e2u_rx_result_name(result));
	}
	CHECK(v92_e2u_rx_handoff(&e2u_rx, &handoff) == V92_E2U_RX_OK &&
	      v92_upstream_data_rx_init(&receiver, &handoff, &mapping, &direct_plan) ==
		      V92_UPSTREAM_DATA_RX_OK &&
	      transmitter.b1u_symbols_remaining == V92_B1U_SYMBOLS -
		      V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY &&
	      memcmp(&receiver.line.equalizer, &handoff.line.equalizer,
		      sizeof(handoff.line.equalizer)) == 0,
	      "E2u lost pending B1u inputs or changed the trained FIR");
	for (unsigned int symbol = 0u;
	     symbol < (V92_B1U_FRAMES + DATA_FRAMES + V92_PCM_RX_DELAY_FRAMES + (rm ? 34u : 0u)) *
		      V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		int16_t sample;
		struct v92_upstream_data_frame frame;
		struct v92_upstream_data_frame saved;
		enum v92_upstream_data_rx_result result;

		memset(&frame, 0xa5, sizeof(frame));
		saved = frame;
		/* Two epochs closer than traceback: both pending generations must
		 * survive. Transmit and equalized receive boundaries differ by the
		 * trained FIR's physical delay. No request acquisition is simulated. */
		if (control == STREAM_RESET && transmitter.queue_count == 0u &&
		    (source_frames == 16u || source_frames == 18u)) {
			struct v92_upstream_data_tx expected = transmitter;

			expected.encoder.state.scrambler.history = 0u;
			expected.encoder.state.differential = 0u;
			CHECK(v92_upstream_data_tx_reset_control(&transmitter) ==
				      V92_UPSTREAM_DATA_TX_OK &&
			      memcmp(&transmitter, &expected, sizeof(expected)) == 0,
			      "transmit control reset changed retained state");
		}
		if (control == STREAM_RESET && receiver.line.symbol_count == 0u &&
		    (receiver.line.received_frames == V92_B1U_FRAMES + 16u ||
		     receiver.line.received_frames == V92_B1U_FRAMES + 18u)) {
			struct v92_upstream_data_rx expected = receiver;

			for (size_t path = 0u; path < expected.decoder.path_count; ++path) {
				expected.decoder.paths[path].descrambler.history = 0u;
				expected.decoder.paths[path].differential = 0u;
			}
			CHECK(v92_upstream_data_rx_reset_control(&receiver) ==
				      V92_UPSTREAM_DATA_RX_OK &&
			      memcmp(&receiver, &expected, sizeof(expected)) == 0,
			      "receive control reset changed retained state");
		}
		if (control == STREAM_RESET && receiver.line.received_frames == V92_B1U_FRAMES + 16u &&
		    receiver.line.symbol_count != 0u) {
			struct v92_upstream_data_rx expected = receiver;

			CHECK(v92_upstream_data_rx_reset_control(&receiver) ==
				      V92_UPSTREAM_DATA_RX_INVALID_STATE &&
			      memcmp(&receiver, &expected, sizeof(expected)) == 0,
			      "partial equalized frame accepted a control reset");
		}
		if (control == STREAM_RESET && source_frames == 17u && transmitter.queue_count != 0u) {
			struct v92_upstream_data_tx expected = transmitter;

			CHECK(v92_upstream_data_tx_reset_control(&transmitter) ==
				      V92_UPSTREAM_DATA_TX_INVALID_STATE &&
			      memcmp(&transmitter, &expected, sizeof(expected)) == 0,
			      "partial transmit frame accepted a control reset");
		}
		if (rm && !requested && source_frames == 16u && transmitter.queue_count == 0u) {
			CHECK(v92_upstream_data_tx_start_rm(&transmitter) == V92_UPSTREAM_DATA_TX_OK,
			      "stream RM request failed");
			requested = true;
		}
		if (rm && receiver.line.symbol_count == 0u && receiver.line.received_frames == RM_END)
			CHECK(v92_upstream_data_rx_reset_control(&receiver) == V92_UPSTREAM_DATA_RX_OK,
			      "stream RM control handoff failed");
		enum v92_upstream_data_tx_result tx_result = v92_upstream_data_tx_next(
			&transmitter, source, &source_frames, &sample);
		CHECK(tx_result == V92_UPSTREAM_DATA_TX_OK ||
		      (rm && tx_result == V92_UPSTREAM_DATA_TX_RM_COMPLETE), "data transmitter stopped");
		CHECK(receive_compared(&receiver, wire_sample(&channel, sample), &frame, &result),
		      "owned stream diverged");
		CHECK(result == (symbol % V92_PCM_MAPPING_FRAME_SYMBOLS == 11u ?
			      V92_UPSTREAM_DATA_RX_FRAME : V92_UPSTREAM_DATA_RX_OK),
		      "law %u rate %u distortion %u symbol %u stopped: %s",
		      (unsigned int)law, rate, distorted, symbol,
		      v92_upstream_data_rx_result_name(result));
		CHECK(receiver.line.input_samples == handoff.line.input_samples + symbol + 1u &&
		      v92_upstream_data_rx_b1u_complete(&receiver) == (symbol + 1u >= V92_B1U_SYMBOLS),
		      "physical B1u boundary changed");
		if (result != V92_UPSTREAM_DATA_RX_FRAME) {
			CHECK(memcmp(&frame, &saved, sizeof(frame)) == 0, "buffering wrote output");
			continue;
		}
		if (decisions < V92_B1U_FRAMES)
			b1u_error += frame.squared_error;
		enum v92_pcm_frame_rm expected_rm = !rm || decisions < RM_START +
			V92_PCM_RM_QUALIFY_FRAMES - 1u || decisions >= RM_END ? V92_PCM_FRAME_RM_NONE :
			decisions < RM_PRIME ? V92_PCM_FRAME_RM_SIGNAL :
			decisions == RM_PRIME ? V92_PCM_FRAME_RM_PRIME : V92_PCM_FRAME_RM_COMPLETE;
		CHECK(frame.rm == expected_rm, "law %u rate %u frame %u RM stage %u != %u",
		      law, rate, decisions, frame.rm, expected_rm);
		unsigned output_frame = decisions - V92_PCM_RX_DELAY_FRAMES;
		bool request_slot = rm && output_frame >= RM_START && output_frame < RM_END;
		if (!request_slot)
			CHECK((frame.bit_count != 0u) == (decisions >= V92_PCM_RX_DELAY_FRAMES),
			      "payload delay changed");
		if (rm && decisions >= RM_START + V92_PCM_RM_QUALIFY_FRAMES - 1u &&
		    output_frame < RM_END)
			CHECK(frame.bit_count == 0u, "qualified RM leaked delayed data");
		decisions++;
		if (decisions == V92_B1U_FRAMES) {
			bool retained = false;
			for (size_t path = 0u; path < receiver.decoder.path_count; ++path) {
				const struct v92_pcm_frame_rx_path *state = &receiver.decoder.paths[path];
				retained |= state->trellis.state == b1u_reference.state.trellis.state &&
					state->differential == b1u_reference.state.differential &&
					state->descrambler.history == b1u_reference.state.scrambler.history &&
					memcmp(&state->filter, &b1u_reference.state.filter,
						sizeof(state->filter)) == 0;
			}
			CHECK(retained, "law %u rate %u distortion %u lost known B1u history",
			      (unsigned int)law, rate, distorted);
		}
		if (frame.bit_count == 0u || request_slot)
			continue;
		CHECK(frame.b1u == (delivered < V92_B1U_FRAMES), "delayed B1u misclassified");
		uint8_t expected[V92_PCM_MAPPING_MAX_BITS];

		if (frame.b1u)
			memset(expected, 1, mapping.data_bits_per_frame);
		else
			payload(expected, mapping.data_bits_per_frame, delivered - V92_B1U_FRAMES);
		CHECK(frame.bit_count == mapping.data_bits_per_frame && !frame.ambiguous &&
		      memcmp(expected, frame.bits, frame.bit_count) == 0,
		      "law %u rate %u distortion %u frame %u changed payload (ambiguous %u residual %.3f)",
		      (unsigned int)law, rate, distorted, delivered, frame.ambiguous,
		      frame.squared_error);
		delivered++;
	}
	CHECK(delivered == V92_B1U_FRAMES + DATA_FRAMES &&
	      (postcursor == 0.0 || b1u_error > 0u), "delivery or distortion evidence changed");
	return true;
}

struct fast_control_source {
	struct v92_upstream_control control;
	uint8_t record[V92_CPU_MAX_FRAME_BYTES];
	size_t bits, index;
	unsigned data_frames;
	bool active;
};

static bool fast_control_source(void *context, uint8_t *bits, size_t count)
{
	struct fast_control_source *source_state = context;
	if (!source_state->active)
		return source(&source_state->data_frames, bits, count);
	if (source_state->control.record_complete) {
		/* Only flush receive lookahead after the final control record. This
		 * fixture does not claim to exercise E2u/FB1u or a complete FPE. */
		if (v92_upstream_control_can_finish(&source_state->control)) {
			memset(bits, 1, count);
			return true;
		}
		if (!v92_upstream_control_start(&source_state->control, count,
			    source_state->record, sizeof(source_state->record), &source_state->bits))
			return false;
		source_state->index = 0;
	}
	if (count > source_state->bits - source_state->index)
		return false;
	for (size_t bit = 0; bit < count; ++bit)
		bits[bit] = packed_msb_bit(source_state->record, source_state->index++);
	return true;
}

static bool test_fast_control(enum g711_law law, uint8_t rate)
{
	struct channel channel = { .gain = 1.0, .law = law, .polarity = 1 };
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct v92_phase4_record_tx e2u_tx;
	struct v92_e2u_rx e2u_rx;
	struct v92_upstream_data_tx tx;
	struct v92_upstream_data_rx rx;
	struct v92_control_rx parser;
	struct fast_control_source input = { 0 };
	struct v92_suvu_parameters suvu = { .prefilter_level_db_q2_2 = -16 };
	struct v92_cpu_parameters cpu = {
		.law = law, .trn1d_ratio_q3_13 = 0x2000, .data_rate_number = 21,
		.sign_redundancy = 1, .spectral_lookahead_frames = 1,
		.constellation_count = 1,
		.transmit_constellations = { { .ucode_mask = { 2, 0, 0, 128 } } },
	};
	CHECK(v92_cpd_direct_init(&cpd, law, rate, false) == V92_CPD_OK &&
	      v92_pcm_mapping_init(&mapping, law, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_upstream_data_tx_init(&tx, &mapping, TEST_LU) == V92_UPSTREAM_DATA_TX_OK &&
	      v92_upstream_control_init(&input.control, &suvu, &cpu) &&
	      v92_control_rx_init_upstream(&parser, mapping.data_bits_per_frame) == V92_CONTROL_RX_OK &&
	      prepare_e2u(&channel, false, &e2u_tx, &e2u_rx), "fast control fixture failed");
	while (!v92_e2u_rx_is_complete(&e2u_rx)) {
		int16_t sample;
		if (!v92_phase4_record_tx_is_complete(&e2u_tx))
			CHECK(v92_phase4_record_tx_next(&e2u_tx, &sample) == V92_PHASE4_RECORD_TX_OK,
			      "fast control E2u failed");
		else
			CHECK(v92_upstream_data_tx_next(&tx, fast_control_source, &input, &sample) ==
			      V92_UPSTREAM_DATA_TX_OK, "fast control B1u failed");
		enum v92_e2u_rx_result result = v92_e2u_rx_sample(&e2u_rx, wire_sample(&channel, sample));
		CHECK(result == V92_E2U_RX_OK || result == V92_E2U_RX_COMPLETE, "E2u receive failed");
	}
	CHECK(v92_upstream_data_rx_init(&rx, &e2u_rx.signal, &mapping, &direct_plan) ==
	      V92_UPSTREAM_DATA_RX_OK, "fast control data handoff failed");
	bool requested = false, receiving = false;
	unsigned suv_count = 0, cpu_count = 0, ack_count = 0;
	for (unsigned i = 0; i < 6000; ++i) {
		if (!requested && input.data_frames == 16 && !tx.queue_count) {
			CHECK(v92_upstream_data_tx_start_rm(&tx) == V92_UPSTREAM_DATA_TX_OK, "RM failed");
			requested = true;
		}
		int16_t sample;
		enum v92_upstream_data_tx_result result = v92_upstream_data_tx_next(
			&tx, fast_control_source, &input, &sample);
		CHECK(result == V92_UPSTREAM_DATA_TX_OK || result == V92_UPSTREAM_DATA_TX_RM_COMPLETE,
		      "fast control transmitter stopped");
		if (result == V92_UPSTREAM_DATA_TX_RM_COMPLETE)
			input.active = true;
		if (input.active && !input.control.record_complete &&
		    input.index == input.bits && !tx.queue_count)
			CHECK(v92_upstream_control_complete(&input.control), "control wire completion failed");
		struct v92_upstream_data_frame frame;
		enum v92_upstream_data_rx_result received = v92_upstream_data_rx_sample(
			&rx, wire_sample(&channel, sample), &frame);
		CHECK(received == V92_UPSTREAM_DATA_RX_OK || received == V92_UPSTREAM_DATA_RX_FRAME,
		      "fast control receiver stopped");
		if (received != V92_UPSTREAM_DATA_RX_FRAME)
			continue;
		if (frame.rm == V92_PCM_FRAME_RM_COMPLETE) {
			CHECK(v92_upstream_data_rx_reset_control(&rx) == V92_UPSTREAM_DATA_RX_OK,
			      "RM did not preserve the control receive boundary");
			receiving = true;
		}
		if (!receiving || !frame.bit_count)
			continue;
		CHECK(!frame.ambiguous && !frame.b1u, "control did not retain the data coding epoch");
		for (size_t bit = 0; bit < frame.bit_count; ++bit) {
			struct v92_control_event event;
			CHECK(v92_control_rx_receive_bit(&parser, frame.bits[bit], &event) == V92_CONTROL_RX_OK,
			      "fast control parser stopped");
			if (event.type == V92_CONTROL_EVENT_SUVU) {
				CHECK(!event.record.suvu.parameters.request_silence, "FPE SUVu requested silence");
				ack_count += event.record.suvu.parameters.acknowledges_cpd;
				if (!suv_count++) {
					struct v92_suvd_parameters suvd = { 0 };
					CHECK(v92_upstream_control_note_suvd(&input.control, &suvd), "SUVd failed");
				}
			} else if (event.type == V92_CONTROL_EVENT_CPU) {
				CHECK(!cpu_count++ && event.record.cpu.parameters.data_rate_number == cpu.data_rate_number &&
				      !event.record.cpu.parameters.acknowledges_cpd &&
				      v92_upstream_control_note_cpd(&input.control, true), "CPu/CPd exchange failed");
			} else {
				CHECK(event.type == V92_CONTROL_EVENT_NONE, "control wire record rejected: %u", event.type);
			}
		}
		if (ack_count && input.control.record_complete && v92_upstream_control_can_finish(&input.control)) {
			CHECK(cpu_count == 1 && input.control.completed_cpu_records == 1 &&
			      input.data_frames == 16 && memcmp(&tx.encoder.mapping, &mapping, sizeof(mapping)) == 0,
			      "fast control changed old mapping, repeated CPu, or fetched host payload");
			return true;
		}
	}
	CHECK(false, "law %u rate %u fast control did not finish: SUVu=%u CPu=%u ack=%u",
	      law, rate, suv_count, cpu_count, ack_count);
}

static bool test_owned_boundaries(void)
{
	struct channel channel = { .gain = 1.0, .law = G711_LAW_PCMU, .polarity = 1 };
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct v92_phase4_record_tx e2u_tx;
	struct v92_e2u_rx e2u_rx;
	struct v92_upstream_data_rx initial, receiver;
	struct v92_upstream_data_frame frame, sentinel;

	CHECK(prepare_e2u(&channel, false, &e2u_tx, &e2u_rx) &&
	      v92_cpd_direct_init(&cpd, channel.law, 1u, false) == V92_CPD_OK &&
	      v92_pcm_mapping_init(&mapping, channel.law, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_upstream_data_rx_init(&initial, &e2u_rx.signal, &mapping, &direct_plan) ==
		      V92_UPSTREAM_DATA_RX_OK, "owned boundary fixture failed");
	memset(&sentinel, 0xa5, sizeof(sentinel));
	for (unsigned fault = 0; fault < 4; ++fault) {
		receiver = initial;
		frame = sentinel;
		enum v92_upstream_data_rx_result expected, result;

		if (fault == 0) {
			receiver.failed = true;
			expected = V92_UPSTREAM_DATA_RX_FAILED;
		} else if (fault == 1) {
			receiver.line.input_samples = UINT64_MAX;
			expected = V92_UPSTREAM_DATA_RX_INVALID_STATE;
		} else {
			expected = V92_UPSTREAM_DATA_RX_NUMERIC_FAILURE;
			if (fault == 2) {
				receiver.line.equalizer.history[0].in_phase = DBL_MAX;
				receiver.line.equalizer.coefficients[1].in_phase = 2.0;
			} else {
				for (unsigned i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS - 1; ++i)
					CHECK(v92_upstream_data_rx_sample(&receiver, 0, &frame) ==
						      V92_UPSTREAM_DATA_RX_OK, "owned late-failure buffering failed");
				receiver.line.observations[0].history[0].in_phase = DBL_MAX;
			}
		}
		struct v92_upstream_data_rx saved = receiver;
		if (fault >= 2)
			saved.failed = true;
		CHECK(receive_compared(&receiver, fault == 2 ? INT16_MAX : 0, &frame, &result) &&
		      result == expected && memcmp(&receiver, &saved, sizeof(saved)) == 0 &&
		      memcmp(&frame, &sentinel, sizeof(frame)) == 0,
		      "owned failure consumed input or wrote output: %u", fault);
	}
	receiver = initial;
	CHECK(v92_upstream_data_rx_sample_owned(NULL, 0, &frame) ==
		      V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      v92_upstream_data_rx_sample_owned(&receiver, 0, NULL) ==
		      V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      v92_upstream_data_rx_sample_owned(&receiver, 0, (void *)&receiver) ==
		      V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      memcmp(&receiver, &initial, sizeof(receiver)) == 0 &&
	      memcmp(&frame, &sentinel, sizeof(frame)) == 0,
	      "owned arguments or aliased output mutated state");
	return true;
}

static bool test_rejection_and_failure(void)
{
	struct channel channel = { .gain = 1.0, .law = G711_LAW_PCMU, .polarity = 1 };
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct v92_phase4_record_tx e2u_tx;
	struct v92_e2u_rx e2u_rx;
	struct v92_upstream_data_rx receiver;
	struct v92_upstream_data_rx saved;
	struct v92_upstream_data_frame frame;
	struct v92_upstream_data_frame saved_frame;

	CHECK(prepare_e2u(&channel, false, &e2u_tx, &e2u_rx) &&
	      v92_cpd_direct_init(&cpd, channel.law, 1u, false) == V92_CPD_OK &&
	      v92_pcm_mapping_init(&mapping, channel.law, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping, &direct_plan) ==
		      V92_UPSTREAM_DATA_RX_OK, "rejection fixture failed");
	const double slower_steps[] = { 0.0, 0.001 };
	for (size_t index = 0u; index < sizeof(slower_steps) / sizeof(slower_steps[0]); ++index) {
		struct v92_upstream_rx slower = e2u_rx.signal;
		struct v92_upstream_data_rx initialized;

		slower.line.adaptation = slower_steps[index];
		CHECK(v92_upstream_data_rx_init(&initialized, &slower, &mapping, &direct_plan) ==
			      V92_UPSTREAM_DATA_RX_OK &&
		      initialized.line.adaptation == slower_steps[index] &&
		      memcmp(&initialized.line.equalizer, &slower.line.equalizer,
			      sizeof(slower.line.equalizer)) == 0,
		      "data handoff increased a slower step or changed the learned FIR");
	}
	saved = receiver;
	memset(&frame, 0xa5, sizeof(frame));
	saved_frame = frame;
	CHECK(!v92_upstream_data_rx_is_valid(NULL) &&
	      !v92_upstream_data_rx_b1u_complete(NULL) &&
	      v92_upstream_data_rx_reset_control(NULL) == V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      v92_upstream_data_rx_reset_control(&receiver) == V92_UPSTREAM_DATA_RX_INVALID_STATE &&
	      v92_upstream_data_rx_init(NULL, &e2u_rx.signal, &mapping, &direct_plan) ==
		      V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      v92_upstream_data_rx_init(&receiver, NULL, &mapping, &direct_plan) ==
		      V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, NULL, &direct_plan) ==
		      V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping, NULL) ==
		      V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping, &receiver.line.plan) ==
		      V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &receiver.decoder.mapping, &direct_plan) ==
		      V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      v92_upstream_data_rx_sample(NULL, 0, &frame) ==
		      V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      v92_upstream_data_rx_sample(&receiver, 0, NULL) ==
		      V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      v92_upstream_data_rx_sample(&receiver, 0, (void *)&receiver) ==
		      V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      memcmp(&frame, &saved_frame, sizeof(frame)) == 0,
	      "invalid call changed state or output");
	e2u_rx.signal.training_tail_remaining = 1u;
	CHECK(v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping, &direct_plan) ==
		      V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "unconsumed TRN2u tail accepted");
	e2u_rx.signal.training_tail_remaining = 0u;
	mapping.law = G711_LAW_PCMA;
	CHECK(v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping, &direct_plan) ==
		      V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS,
	      "conflicting PCM law accepted");
	receiver.line.input_samples = UINT64_MAX;
	CHECK(v92_upstream_data_rx_sample(&receiver, 0, &frame) ==
		      V92_UPSTREAM_DATA_RX_INVALID_STATE &&
	      memcmp(&frame, &saved_frame, sizeof(frame)) == 0,
	      "input clock overflow accepted");
	receiver = saved;
	receiver.failed = true;
	struct v92_upstream_data_rx failed = receiver;

	CHECK(v92_upstream_data_rx_reset_control(&receiver) == V92_UPSTREAM_DATA_RX_FAILED &&
	      memcmp(&receiver, &failed, sizeof(failed)) == 0,
	      "control reset revived a failed receiver");
	receiver.line.symbol_count = V92_PCM_MAPPING_FRAME_SYMBOLS;
	failed = receiver;
	CHECK(v92_upstream_data_rx_reset_control(&receiver) == V92_UPSTREAM_DATA_RX_INVALID_STATE &&
	      memcmp(&receiver, &failed, sizeof(failed)) == 0,
	      "control reset changed invalid line state");
	receiver = saved;
	receiver.line.equalizer.history[0].in_phase = DBL_MAX;
	receiver.line.equalizer.coefficients[1].in_phase = 2.0;
	saved = receiver;
	saved.failed = true;
	CHECK(v92_upstream_data_rx_is_valid(&receiver) &&
	      v92_upstream_data_rx_sample(&receiver, INT16_MAX, &frame) ==
		      V92_UPSTREAM_DATA_RX_NUMERIC_FAILURE &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      v92_upstream_data_rx_sample(&receiver, 0, &frame) ==
		      V92_UPSTREAM_DATA_RX_FAILED &&
	      memcmp(&frame, &saved_frame, sizeof(frame)) == 0,
	      "DSP failure advanced state, changed output, or did not latch");
	mapping.law = G711_LAW_PCMU;
	CHECK(v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping, &direct_plan) ==
		      V92_UPSTREAM_DATA_RX_OK, "late-failure initialization failed");
	for (size_t sample = 0u; sample < V92_PCM_MAPPING_FRAME_SYMBOLS - 1u; ++sample)
		CHECK(v92_upstream_data_rx_sample(&receiver, 0, &frame) ==
			      V92_UPSTREAM_DATA_RX_OK, "frame buffering failed");
	receiver.line.observations[0].history[0].in_phase = DBL_MAX;
	saved = receiver;
	saved.failed = true;
	CHECK(v92_upstream_data_rx_is_valid(&receiver) &&
	      v92_upstream_data_rx_sample(&receiver, 0, &frame) ==
		      V92_UPSTREAM_DATA_RX_NUMERIC_FAILURE &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      memcmp(&frame, &saved_frame, sizeof(frame)) == 0,
	      "late adaptation failure committed the decoded frame");
	CHECK(v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping, &direct_plan) ==
		      V92_UPSTREAM_DATA_RX_OK, "damaged-B1u initialization failed");
	double training_error = 0.0;

	for (size_t sample = 0u; sample < V92_B1U_SYMBOLS; ++sample) {
		enum v92_upstream_data_rx_result result =
			v92_upstream_data_rx_sample(&receiver, 0, &frame);

		CHECK(result == V92_UPSTREAM_DATA_RX_OK || result == V92_UPSTREAM_DATA_RX_FRAME,
		      "damaged training failed to report evidence");
		if (result == V92_UPSTREAM_DATA_RX_FRAME)
			training_error += frame.squared_error;
	}
	CHECK(training_error > (double)V92_B1U_SYMBOLS *
		      V92_DATA_MAX_RMS_ERROR * V92_DATA_MAX_RMS_ERROR &&
	      v92_upstream_data_rx_b1u_complete(&receiver),
	      "known B1u bits hid unusable samples or moved the boundary");
	CHECK(v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping, &direct_plan) ==
		      V92_UPSTREAM_DATA_RX_OK, "clipped-input initialization failed");
	memset(receiver.line.equalizer.coefficients, 0,
	       sizeof(receiver.line.equalizer.coefficients));
	receiver.line.equalizer.coefficients[0].in_phase = 4.0;
	for (size_t sample = 0u; sample < V92_PCM_MAPPING_FRAME_SYMBOLS; ++sample)
		CHECK(v92_upstream_data_rx_sample(&receiver, INT16_MAX, &frame) ==
			      (sample + 1u == V92_PCM_MAPPING_FRAME_SYMBOLS ?
				       V92_UPSTREAM_DATA_RX_FRAME : V92_UPSTREAM_DATA_RX_OK),
		      "finite equalizer excursion stopped reception");
	CHECK(frame.squared_error > 12.0 * 65535.0 * 65535.0,
	      "fixed-point clipping hid the equalizer residual");
	struct v92_upstream_plan plan = {
		.gain = 2.0, .delay = 60u, .observation = V92_PCM_FRAME_CONSTELLATION,
	};

	CHECK(v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping, &plan) ==
		      V92_UPSTREAM_DATA_RX_OK, "raw-plan initialization failed");
	saved = receiver;
	for (unsigned int mutation = 0u; mutation < 5u; ++mutation) {
		struct v92_upstream_plan bad = plan;

		switch (mutation) {
		case 0: bad.gain = NAN; break;
		case 1: bad.delay = 0u; break;
		case 2: bad.gain = 0.0; break;
		case 3: bad.delay = V92_B1U_SYMBOLS; break;
		case 4: bad.observation = V92_PCM_FRAME_FILTER_OUTPUT; break;
		}
		CHECK(v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping, &bad) ==
			      V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS &&
		      memcmp(&receiver, &saved, sizeof(saved)) == 0, "invalid raw plan changed receiver");
	}
	receiver.decoder.observation = V92_PCM_FRAME_FILTER_OUTPUT;
	CHECK(!v92_upstream_data_rx_is_valid(&receiver), "conflicting receive coordinates accepted");
	saved_frame = frame;
	for (unsigned int mutation = 0u; mutation < 8u; ++mutation) {
		receiver = saved;
		switch (mutation) {
		case 0: receiver.line.calibration.gain = NAN; break;
		case 1: receiver.line.calibration.minimum = 0.0; break;
		case 2: receiver.line.calibration.maximum = INFINITY; break;
		case 3: receiver.line.calibration.gain = plan.gain * 2.0; break;
		case 4: receiver.line.calibration.correlation = NAN; break;
		case 5: receiver.line.calibration.energy = -1.0; break;
		case 6: receiver.line.calibration.energy = 1.0; break;
		case 7: receiver.line.calibration.correlation = 1.0; break;
		}
		struct v92_upstream_data_rx invalid = receiver;

		CHECK(!v92_upstream_data_rx_is_valid(&receiver) &&
		      v92_upstream_data_rx_sample(&receiver, 0, &frame) == V92_UPSTREAM_DATA_RX_INVALID_STATE &&
		      memcmp(&receiver, &invalid, sizeof(invalid)) == 0 &&
		      memcmp(&frame, &saved_frame, sizeof(frame)) == 0,
		      "invalid gain calibration consumed a sample or changed output");
	}
	for (unsigned int damage = 0u; damage < 4u; ++damage) {
		uint32_t random = UINT32_C(873941);

		receiver = saved;
		training_error = 0.0;
		for (unsigned int i = 0u; i < (unsigned int)V92_B1U_SYMBOLS + plan.delay - V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY; ++i) {
			random = random * UINT32_C(1664525) + UINT32_C(1013904223);
			int16_t sample = damage == 0u ? 0 : damage == 1u ? 12000 :
				damage == 2u ? (int16_t)((int)(random >> 16) - 32768) : INT16_MAX;
			enum v92_upstream_data_rx_result result = v92_upstream_data_rx_sample(&receiver, sample, &frame);

			CHECK(result == V92_UPSTREAM_DATA_RX_OK || result == V92_UPSTREAM_DATA_RX_FRAME,
			      "damaged raw training stopped evidence collection");
			if (result == V92_UPSTREAM_DATA_RX_FRAME)
				training_error += frame.squared_error;
		}
		CHECK(v92_upstream_data_rx_b1u_complete(&receiver) &&
		      training_error > (double)V92_B1U_SYMBOLS * V92_DATA_MAX_RMS_ERROR * V92_DATA_MAX_RMS_ERROR &&
		      receiver.line.calibration.gain >= receiver.line.calibration.minimum &&
		      receiver.line.calibration.gain <= receiver.line.calibration.maximum,
		      "bounded raw calibration hid damaged B1u (%u)", damage);
		if (damage == 0u)
			CHECK(receiver.line.calibration.gain == plan.gain && receiver.line.calibration.energy == 0.0,
			      "silence changed nominal gain");
	}
	receiver = saved;
	receiver.line.polarity = -1;
	receiver.line.delay_remaining = 0u;
	CHECK(v92_upstream_data_rx_sample(&receiver, INT16_MIN, &frame) == V92_UPSTREAM_DATA_RX_OK &&
	      receiver.line.raw[0] == 32768 && receiver.line.samples[0] == INT16_MAX,
	      "polarity reversal overflowed or lost original raw excursion");
	for (unsigned int i = 1u; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i)
		CHECK(v92_upstream_data_rx_sample(&receiver, INT16_MIN, &frame) ==
			      (i + 1u == V92_PCM_MAPPING_FRAME_SYMBOLS ? V92_UPSTREAM_DATA_RX_FRAME : V92_UPSTREAM_DATA_RX_OK),
		      "raw excursion stopped receiving");
	CHECK(frame.squared_error > 12.0 * 20000.0 * 20000.0,
	      "raw decoder clipping hid the residual");
	return true;
}

/* E2u recognizes its last control symbol after already consuming ten B1u
 * samples. The channel-compensating filter has a different causal delay:
 * receive its remaining prefix exactly once, not as B1u or payload. */
static bool test_channel_plan(
	enum g711_law law, int polarity, bool extended, bool biased, bool conditioned)
{
	struct channel channel = {
		.gain = 1.0, .law = law, .polarity = polarity,
		.receive_gain = conditioned ? 0.75 : 0.0,
	};
	struct v92_upstream_channel measured = { 0 };
	struct v92_upstream_plan plan;
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct v92_phase4_record_tx e2u_tx;
	struct v92_e2u_rx e2u_rx;
	struct v92_upstream_data_tx transmitter;
	struct v92_upstream_data_rx receiver;
	struct v92_upstream_data_rx comparison_rx;
	struct v92_upstream_data_line trained;
	uint8_t expected[V92_PCM_MAPPING_MAX_BITS];
	unsigned int source_frames = 0u, leading = 0u, delivered = 0u;
	double b1_error = 0.0, rejected_error = 0.0;
	uint32_t random = UINT32_C(9376271);

	for (unsigned int i = 0u; i < V92_UPSTREAM_CHANNEL_SYMBOLS; ++i) {
		random = random * UINT32_C(1664525) + UINT32_C(1013904223);
		int16_t sample = (int16_t)((int)(random >> 19) - TEST_LU);

		CHECK(v92_upstream_channel_sample(&measured, sample,
			    polarity * wire_sample(&channel, sample)), "channel measurement failed");
	}
	CHECK(v92_cpd_direct_init(&cpd, law, 1u, extended) == V92_CPD_OK &&
	      v92_cpd_channel_design(&cpd, &measured, law, TEST_LU, &plan) &&
	      v92_pcm_mapping_init(&mapping, law, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_upstream_data_tx_init(&transmitter, &mapping, TEST_LU) == V92_UPSTREAM_DATA_TX_OK &&
	      prepare_e2u(&channel, extended, &e2u_tx, &e2u_rx), "channel-plan fixture failed");
	/* A sparse training-level estimate can bias the nominal inverse without
	 * changing the peer's CPd waveform. Known B1u must refine that scalar. */
	if (biased)
		plan.gain *= 1.02;
	while (!v92_e2u_rx_is_complete(&e2u_rx)) {
		int16_t sample;

		CHECK(leading < V92_B1U_SYMBOLS, "E2u did not complete before B1u ended");
		if (!v92_phase4_record_tx_is_complete(&e2u_tx)) {
			CHECK(v92_phase4_record_tx_next(&e2u_tx, &sample) == V92_PHASE4_RECORD_TX_OK,
			      "E2u transmit failed");
		} else {
			CHECK(v92_upstream_data_tx_next(&transmitter, source, &source_frames, &sample) ==
				      V92_UPSTREAM_DATA_TX_OK, "early B1u transmit failed");
			++leading;
		}
		enum v92_e2u_rx_result result = v92_e2u_rx_sample(&e2u_rx, wire_sample(&channel, sample));

		CHECK(result == V92_E2U_RX_OK || result == V92_E2U_RX_COMPLETE, "E2u receive failed");
	}
	CHECK(leading == V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY &&
	      v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping, &plan) == V92_UPSTREAM_DATA_RX_OK &&
	      receiver.line.delay_remaining == plan.delay - leading, "E2u/data delay lost");
	comparison_rx = receiver;
	if (biased)
		comparison_rx.line.calibration.minimum = comparison_rx.line.calibration.maximum = plan.gain;
	else
		comparison_rx.line.polarity = -polarity;
	for (unsigned int symbol = 0u; delivered < DATA_FRAMES &&
	     symbol < (V92_B1U_FRAMES + DATA_FRAMES + V92_PCM_RX_DELAY_FRAMES + 6u) *
		     V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		int16_t sample;
		struct v92_upstream_data_frame frame;

		CHECK(v92_upstream_data_tx_next(&transmitter, source, &source_frames, &sample) ==
			      V92_UPSTREAM_DATA_TX_OK, "channel-plan data transmit failed");
		int16_t input = wire_sample(&channel, sample);
		bool first_frame = receiver.line.received_frames == 0u &&
			receiver.line.symbol_count == V92_PCM_MAPPING_FRAME_SYMBOLS - 1u;
		struct v92_pcm_frame_decision predicted;
		int32_t raw[V92_PCM_MAPPING_FRAME_SYMBOLS];

		if (first_frame) {
			struct v92_pcm_frame_rx decoder = receiver.decoder;
			int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS];
			uint8_t bits[V92_PCM_MAPPING_MAX_BITS];

			memcpy(samples, receiver.line.samples, sizeof(samples));
			memcpy(raw, receiver.line.raw, sizeof(raw));
			raw[V92_PCM_MAPPING_FRAME_SYMBOLS - 1u] = polarity * (int32_t)input;
			samples[V92_PCM_MAPPING_FRAME_SYMBOLS - 1u] = (int16_t)lround(
				polarity * input * receiver.line.calibration.gain);
			CHECK(v92_pcm_frame_decode(&decoder, samples, true, bits,
					mapping.data_bits_per_frame, &predicted) ==
				      V92_PCM_FRAME_OK, "linear quality prediction failed");
		}
		enum v92_upstream_data_rx_result result;
		CHECK(receive_compared(&receiver, input, &frame, &result),
		      "owned raw-coordinate stream diverged");

		if (first_frame) {
			double error = 0.0;

			for (size_t i = 0u; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i) {
				double residual = raw[i] -
					predicted.predicted_samples[i] / receiver.line.calibration.gain;

				error += residual * residual;
			}
			CHECK(result == V92_UPSTREAM_DATA_RX_FRAME && fabs(frame.squared_error - error) < 1e-6,
			      "raw quality rounded, clipped or requantized its linear prediction");
		}

		if (comparison_rx.line.received_frames < V92_B1U_FRAMES) {
			struct v92_upstream_data_frame comparison;
			enum v92_upstream_data_rx_result comparison_result =
				v92_upstream_data_rx_sample(&comparison_rx, input, &comparison);

			CHECK(comparison_result == result, "B1u comparison lost frame timing");
			if (comparison_result == V92_UPSTREAM_DATA_RX_FRAME)
				rejected_error += comparison.squared_error;
		}

		CHECK(result == V92_UPSTREAM_DATA_RX_OK || result == V92_UPSTREAM_DATA_RX_FRAME,
		      "channel-plan receive failed: %d", result);
		CHECK(receiver.line.received_frames ==
			(symbol + 1u < plan.delay - leading ? 0u :
			 (symbol + 1u - (plan.delay - leading)) / V92_PCM_MAPPING_FRAME_SYMBOLS),
		      "channel-plan frame boundary moved");
		if (result != V92_UPSTREAM_DATA_RX_FRAME)
			continue;
		if (receiver.line.received_frames <= V92_B1U_FRAMES)
			b1_error += frame.squared_error;
		if (receiver.line.received_frames == V92_B1U_ACQUISITION_FRAMES)
			trained = receiver.line;
		else if (receiver.line.received_frames > V92_B1U_ACQUISITION_FRAMES)
			CHECK(memcmp(&receiver.line.calibration, &trained.calibration, sizeof(trained.calibration)) == 0,
			      "B1u validation or payload continued gain calibration");
		if (frame.bit_count == 0u || frame.b1u)
			continue;
		payload(expected, mapping.data_bits_per_frame, delivered);
		CHECK(!frame.ambiguous && frame.bit_count == mapping.data_bits_per_frame &&
		      memcmp(frame.bits, expected, frame.bit_count) == 0,
		      "channel-plan payload lost at frame %u", delivered);
		++delivered;
	}
	struct v92_upstream_data_line saved = receiver.line;

	CHECK(delivered == DATA_FRAMES, "channel-plan data did not arrive");
	CHECK(b1_error < V92_B1U_SYMBOLS * V92_DATA_MAX_RMS_ERROR * V92_DATA_MAX_RMS_ERROR,
	      "channel-plan B1u residual too large: %g", sqrt(b1_error / V92_B1U_SYMBOLS));
	CHECK(rejected_error > V92_B1U_SYMBOLS * V92_DATA_MAX_RMS_ERROR * V92_DATA_MAX_RMS_ERROR,
	      "fixed gain or wrong polarity accepted bad training: %g", sqrt(rejected_error / V92_B1U_SYMBOLS));
	if (biased)
		CHECK(receiver.line.calibration.gain < plan.gain &&
		      receiver.line.calibration.gain >= receiver.line.calibration.minimum,
		      "gain fixture did not recover rejected B1u within training bounds");
	CHECK(memcmp(&receiver.line.plan, &plan, sizeof(plan)) == 0,
	      "B1u calibration changed the negotiated receive plan");
	CHECK(v92_upstream_data_rx_reset_control(&receiver) == V92_UPSTREAM_DATA_RX_OK &&
	      memcmp(&saved, &receiver.line, sizeof(saved)) == 0,
	      "control reset changed channel plan or line timing");
	return true;
}

static bool test_rate_request_contracts(const struct v92_upstream_rx *handoff)
{
	struct v92_ru_line_rx receiver, saved;
	CHECK(v92_ru_line_rx_init_after_e2u(&receiver, handoff), "warm Ru handoff failed");
	saved = receiver;
	CHECK(!v92_ru_line_rx_init_after_e2u(NULL, handoff) &&
	      !v92_ru_line_rx_init_after_e2u(&receiver, NULL) &&
	      !v92_ru_line_rx_is_valid(NULL) &&
	      v92_ru_line_rx_sample(NULL, 0) == V92_RU_RX_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0, "null Ru contract changed state");
	for (unsigned i = 0; i < 3; ++i) {
		struct v92_upstream_rx invalid = *handoff;
		if (i == 0)
			invalid.failed = true;
		else if (i == 1)
			invalid.training_tail_remaining = 1;
		else
			invalid.line.equalizer.coefficients[0].in_phase = NAN;
		CHECK(!v92_ru_line_rx_init_after_e2u(&receiver, &invalid) &&
		      memcmp(&receiver, &saved, sizeof(saved)) == 0, "invalid E2u handoff changed Ru receiver");
	}
	for (unsigned i = 0; i < 5; ++i) {
		receiver = saved;
		switch (i) {
		case 0: receiver.input_samples = UINT64_MAX; break;
		case 1: receiver.equalizer.coefficients[0].in_phase = NAN; break;
		case 2: receiver.signal.phase = V92_RU_RX_FRAME_SYMBOLS; break;
		case 3: receiver.equalizer.decision_delay--; break;
		case 4: receiver.polarity = 0; break;
		}
		struct v92_ru_line_rx before = receiver;
		CHECK(v92_ru_line_rx_sample(&receiver, INT16_MIN) == V92_RU_RX_INVALID_STATE &&
		      memcmp(&receiver, &before, sizeof(before)) == 0, "invalid Ru sample changed state");
	}
	return true;
}

struct rate_exchange {
	struct v92_phase4_digital_station station;
	struct pcm_downstream_rx peer;
	unsigned zero_acknowledgements;
	unsigned withheld_transmit_samples;
	bool complete;
};

static bool rate_exchange_init(struct rate_exchange *exchange,
	const struct v92_ru_line_rx *ru, const struct v92_cpd_parameters *cpd,
	const struct v92_upstream_plan *plan)
{
	struct v92_cpt_parameters cpt = {
		.law = ru->law, .selected_rate_index = 9, .constellation_count = 1,
		.sign_redundancy = 1, .spectral_lookahead = 2,
	};
	struct v92_cpu_parameters cpu = {
		.law = ru->law, .data_rate_number = 11, .constellation_count = 1,
		.sign_redundancy = 1, .spectral_lookahead_frames = 2,
		.shaping_a1_q1_6 = 7, .acknowledges_cpd = true,
	};
	struct v92_cpd_parameters previous = *cpd;
	struct v90_pcm_mapping training, data, control;
	struct pcm_downstream_tx tx;

	for (unsigned ucode = 16; ucode < 112; ++ucode)
		cpu.transmit_constellations[0].ucode_mask[ucode / 8] |= (uint8_t)(1u << (ucode % 8));
	for (unsigned ucode = 24; ucode < 120; ucode += 12)
		cpt.transmit_constellations[0].ucode_mask[ucode / 8] |= (uint8_t)(1u << (ucode % 8));
	previous.acknowledges_cpu = true;
	/* A responder can still be sending Rd when Ru-bar ends. Keep receiving
	 * the peer's fresh TRN2u while the independent transmit clock waits. */
	exchange->withheld_transmit_samples = ru->constellation == V92_CONSTELLATION_8_POINT ? 384u : 0u;
	CHECK(v90_pcm_mapping_init_from_v92_cpt(&training, &cpt) == V90_PCM_MAPPING_OK &&
	      v90_pcm_mapping_init_from_v92_cpu(&data, &cpu) == V90_PCM_MAPPING_OK &&
	      v90_pcm_mapping_init_rate_renegotiation(&control, &training, &data) == V90_PCM_MAPPING_OK &&
	      pcm_downstream_tx_init(&tx, PCM_PROTOCOL_V92, &control) == PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_rx_init(&exchange->peer, PCM_PROTOCOL_V92, &control,
		      V90_PCM_MAPPING_TRANSMIT) == PCM_DOWNSTREAM_RX_OK,
	      "rate exchange downstream mapping failed");
	struct v92_phase4_digital_station *station = &exchange->station;
	memset(station, 0xa5, sizeof(*station));
	struct v92_phase4_digital_station saved = *station;
	for (unsigned invalid = 0; invalid < 5; ++invalid) {
		struct v92_ru_line_rx candidate_ru = *ru;
		struct v92_upstream_plan candidate_plan = *plan;
		struct v92_cpu_parameters candidate_cpu = cpu;
		struct v92_cpd_parameters candidate_cpd = previous;
		switch (invalid) {
		case 0: candidate_ru.signal.bar_symbols--; break;
		case 1: candidate_plan.delay = UINT16_MAX; break;
		case 2: candidate_cpu.data_rate_number = 0; break;
		case 3: candidate_cpd.data_rate_number = 0; break;
		case 4: candidate_cpu.law = (enum g711_law)99; break;
		}
		CHECK(v92_phase4_digital_station_init_after_ru(station, &candidate_ru, &tx,
			      &candidate_cpd, &candidate_plan, &candidate_cpu, 161) != V92_PHASE4_DIGITAL_STATION_OK &&
		      memcmp(station, &saved, sizeof(saved)) == 0,
		      "invalid rate exchange input %u changed station", invalid);
	}
	CHECK(v92_phase4_digital_station_init_after_ru(station, ru, &tx,
		      &previous, plan, &cpu, 161) == V92_PHASE4_DIGITAL_STATION_OK &&
	      v92_phase4_digital_rx_is_renegotiating(&station->receive) &&
	      !station->cpu_received && !station->suvu_received &&
	      !station->cpu.acknowledges_cpd && !station->peer_acknowledged_cpd &&
	      !station->transmit_acknowledges_cpu && !station->cleardown.active &&
	      !v92_phase4_digital_station_is_ed_ready(station) &&
	      station->transmit.control_configured &&
	      memcmp(&station->upstream_plan, plan, sizeof(*plan)) == 0,
	      "rate exchange reused acknowledgements or lost the receive plan");
	cpu.acknowledges_cpd = false;
	CHECK(memcmp(&station->cpu, &cpu, sizeof(cpu)) == 0 &&
	      v92_cpd_decode(station->initial_cpd.sequence, sizeof(station->initial_cpd.sequence),
		      station->initial_cpd.sequence_bits, station->initial_cpd.frame_bits, &previous) == V92_CPD_OK &&
	      !previous.acknowledges_cpu, "rate exchange lost prior CPu or reused old CPd acknowledgement");
	return true;
}

static bool rate_exchange_clock(struct rate_exchange *exchange, const int16_t *sample)
{
	struct v92_phase4_digital_station *station = &exchange->station;
	if (sample) {
		enum v92_phase4_digital_station_result r = v92_phase4_digital_station_receive(station, *sample);
		CHECK(r == V92_PHASE4_DIGITAL_STATION_OK || r == V92_PHASE4_DIGITAL_STATION_EVENT,
		      "post-Ru station receive failed: %s", v92_phase4_digital_station_result_name(r));
	}
	if (exchange->withheld_transmit_samples) {
		exchange->withheld_transmit_samples--;
		return true;
	}
	uint8_t octet;
	enum v92_phase4_digital_station_result r = v92_phase4_digital_station_transmit(station, &octet);
	if (r == V92_PHASE4_DIGITAL_STATION_CLEARDOWN_COMPLETE) {
		exchange->complete = true;
		CHECK(exchange->zero_acknowledgements && !station->transmit.ed_requested &&
		      station->transmit.transmitted_symbols >= station->cleardown.acknowledgement_end &&
		      station->transmit.transmitted_symbols >= station->cleardown.cp_end + 881u,
		      "cleardown completed without wire acknowledgement/deadline");
		return true;
	}
	CHECK(r == V92_PHASE4_DIGITAL_STATION_OK, "post-Ru station transmit failed: %s",
	      v92_phase4_digital_station_result_name(r));
	struct pcm_downstream_event event;
	enum pcm_downstream_rx_result received = pcm_downstream_rx_receive(&exchange->peer, octet, &event);
	CHECK(received == PCM_DOWNSTREAM_RX_BUFFERING || received == PCM_DOWNSTREAM_RX_TRN2D_FRAME ||
	      received == PCM_DOWNSTREAM_RX_SUVD || received == PCM_DOWNSTREAM_RX_CPD,
	      "post-Ru downstream receiver failed: %s", pcm_downstream_rx_result_name(received));
	if (received == PCM_DOWNSTREAM_RX_CPD && !event.control.record.cpd.parameters.data_rate_number) {
		CHECK(event.control.record.cpd.frame_result == V92_CPD_OK &&
		      event.control.record.cpd.parameters.acknowledges_cpu,
		      "zero-rate CPd did not acknowledge peer cleardown");
		exchange->zero_acknowledgements++;
	}
	return true;
}

static bool test_rate_request_line(enum g711_law law, int polarity,
				   bool extended, bool compensated,
				   enum v92_constellation_size constellation)
{
	struct channel channel = { .gain = 0.75, .postcursor = 0.10,
		.law = law, .polarity = polarity };
	struct v92_upstream_plan plan = { 0 };
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct v92_phase4_record_tx record;
	struct v92_e2u_rx e2u;
	struct v92_upstream_data_tx data_tx;
	struct v92_upstream_data_rx data_rx;
	struct v92_ru_line_rx ru;
	struct v92_pcm_phase4_trn2u training_tx;
	struct v92_pcm_phase4_trn2u_line_rx training_rx, before;
	struct v92_pcm_phase4_trn2u_line_handoff handoff;
	struct v92_upstream_control_line_rx control;
	struct rate_exchange exchange = { 0 };
	unsigned source_frames = 0, delivered = 0;
	CHECK(v92_cpd_direct_init(&cpd, law, 1, extended) == V92_CPD_OK,
	      "rate-request mapping failed");
	if (compensated) {
		struct v92_upstream_channel measured = { 0 };
		uint32_t random = 9376271;
		for (unsigned i = 0; i < V92_UPSTREAM_CHANNEL_SYMBOLS; ++i) {
			random = random * UINT32_C(1664525) + UINT32_C(1013904223);
			int16_t sample = (int16_t)((int)(random >> 19) - TEST_LU);
			CHECK(v92_upstream_channel_sample(&measured, sample,
				    polarity * wire_sample(&channel, sample)), "measurement failed");
		}
		CHECK(v92_cpd_channel_design(&cpd, &measured, law, TEST_LU, &plan),
		      "channel design failed");
	}
	CHECK(v92_pcm_mapping_init(&mapping, law, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_upstream_data_tx_init(&data_tx, &mapping, TEST_LU) == V92_UPSTREAM_DATA_TX_OK &&
	      prepare_e2u_at_level(&channel, extended, &record, &e2u,
		TEST_LU, (int16_t)lround(TEST_LU * channel.gain), 0.002, constellation),
	      "rate-request fixture failed");
	while (!v92_e2u_rx_is_complete(&e2u)) {
		int16_t sample;
		if (!v92_phase4_record_tx_is_complete(&record))
			CHECK(v92_phase4_record_tx_next(&record, &sample) ==
			      V92_PHASE4_RECORD_TX_OK, "E2u transmit failed");
		else
			CHECK(v92_upstream_data_tx_next(&data_tx, source, &source_frames, &sample) ==
			      V92_UPSTREAM_DATA_TX_OK, "B1u transmit failed");
		enum v92_e2u_rx_result r = v92_e2u_rx_sample(&e2u, wire_sample(&channel, sample));
		CHECK(r == V92_E2U_RX_OK || r == V92_E2U_RX_COMPLETE, "E2u failed");
	}
	CHECK(v92_upstream_data_rx_init(&data_rx, &e2u.signal, &mapping, &plan) ==
		V92_UPSTREAM_DATA_RX_OK && v92_ru_line_rx_init_after_e2u(&ru, &e2u.signal),
	      "parallel data/control-channel handoff failed");
	CHECK(test_rate_request_contracts(&e2u.signal), "rate-request contracts failed");
	memset(&training_rx, 0xa5, sizeof(training_rx));
	before = training_rx;
	CHECK(v92_pcm_phase4_trn2u_line_rx_init_after_ru(NULL, &ru) ==
		V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase4_trn2u_line_rx_init_after_ru(&training_rx, NULL) ==
		V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase4_trn2u_line_rx_init_after_ru(&training_rx, &ru) ==
		V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_STATE &&
	      memcmp(&training_rx, &before, sizeof(before)) == 0,
	      "unobserved Ru-bar manufactured a training boundary");
	/* Flush the decoder and either line plan before switching the transmitter
	 * at a physical frame boundary. The raw Ru FIR has its own shorter delay. */
	unsigned flush_frames = V92_PCM_RX_DELAY_FRAMES +
		(plan.delay + ru.equalizer.decision_delay + V92_PCM_MAPPING_FRAME_SYMBOLS - 1u) /
		V92_PCM_MAPPING_FRAME_SYMBOLS;
	while (source_frames < DATA_FRAMES + flush_frames || data_tx.queue_count) {
		int16_t sample;
		struct v92_upstream_data_frame frame;
		CHECK(v92_upstream_data_tx_next(&data_tx, source, &source_frames, &sample) ==
		      V92_UPSTREAM_DATA_TX_OK, "data transmit failed");
		sample = wire_sample(&channel, sample);
		CHECK(v92_ru_line_rx_sample(&ru, sample) == V92_RU_RX_BUFFERING,
		      "data caused a false rate request");
		enum v92_upstream_data_rx_result r = v92_upstream_data_rx_sample(&data_rx, sample, &frame);
		CHECK(r == V92_UPSTREAM_DATA_RX_OK || r == V92_UPSTREAM_DATA_RX_FRAME,
		      "data receive failed before rate request");
		if (r == V92_UPSTREAM_DATA_RX_FRAME && frame.bit_count && !frame.b1u) {
			uint8_t expected[V92_PCM_MAPPING_MAX_BITS];
			payload(expected, mapping.data_bits_per_frame, delivered++);
			CHECK(!frame.ambiguous && frame.bit_count == mapping.data_bits_per_frame &&
			      memcmp(frame.bits, expected, frame.bit_count) == 0,
			      "PCM payload changed before rate request");
		}
	}
	CHECK(delivered >= DATA_FRAMES &&
	      memcmp(ru.equalizer.coefficients, e2u.signal.line.equalizer.coefficients,
		     sizeof(ru.equalizer.coefficients)) == 0,
	      "lost data (%u frames, law=%u polarity=%d extended=%u compensated=%u) or adapted the raw control channel",
	      delivered, law, polarity, extended, compensated);
	CHECK(v92_pcm_phase4_trn2u_init(&training_tx, ru.constellation, 0, TEST_LU) ==
	      V92_PCM_PHASE4_TRN2U_OK, "new training epoch failed");
	unsigned delay = ru.equalizer.decision_delay, detected = 0;
	for (unsigned i = 0; i < 408 + delay; ++i) {
		int16_t sample = i % 6u < 3u ? TEST_LU : -TEST_LU;
		if (i >= 384 && i < 408)
			sample = -sample;
		if (i >= 408)
			CHECK(v92_pcm_phase4_trn2u_next(&training_tx, &sample) ==
			      V92_PCM_PHASE4_TRN2U_OK, "early TRN2u failed");
		enum v92_ru_rx_result r = v92_ru_line_rx_sample(&ru, wire_sample(&channel, sample));
		if (r == V92_RU_RX_DETECTED)
			detected++;
		CHECK(r == V92_RU_RX_BUFFERING || r == V92_RU_RX_DETECTED ||
		      (r == V92_RU_RX_BAR_STARTED && i == 384 + delay) ||
		      (r == V92_RU_RX_COMPLETE && i == 407 + delay),
		      "lost delayed Ru boundary at %u: %u", i, r);
	}
	struct v92_ru_line_rx completed = ru;
	CHECK(detected == 1 && ru.signal.bar_symbols == 24 &&
	      v92_ru_line_rx_sample(&ru, 123) == V92_RU_RX_COMPLETE &&
	      memcmp(&ru, &completed, sizeof(ru)) == 0 &&
	      v92_pcm_phase4_trn2u_line_rx_init_after_ru(&training_rx, &ru) ==
		V92_PCM_PHASE4_TRN2U_LINE_RX_OK &&
	      memcmp(&training_rx.equalizer, &ru.equalizer, sizeof(ru.equalizer)) == 0 &&
	      training_rx.demodulator.descrambler.history == 0 &&
	      training_rx.demodulator.previous_encoded_sign == 0 && training_rx.renegotiation,
	      "Ru-bar handoff lost filter history or failed to reset coding");
	CHECK(rate_exchange_init(&exchange, &ru, &cpd, &plan), "rate exchange did not start");
	struct v92_cpu_parameters retained_cpu = exchange.station.cpu;
	while (!v92_pcm_phase4_trn2u_line_rx_can_handoff(&training_rx)) {
		int16_t sample;
		CHECK(training_tx.emitted_symbols < 12 &&
		      v92_pcm_phase4_trn2u_next(&training_tx, &sample) == V92_PCM_PHASE4_TRN2U_OK,
		      "early control boundary missed");
		sample = wire_sample(&channel, sample);
		enum v92_pcm_phase4_trn2u_line_rx_result r = v92_pcm_phase4_trn2u_line_rx_sample(&training_rx, sample);
		CHECK(rate_exchange_clock(&exchange, &sample), "rate exchange training failed");
		CHECK(r == V92_PCM_PHASE4_TRN2U_LINE_RX_OK ||
		      r == V92_PCM_PHASE4_TRN2U_LINE_RX_ALIGNED, "new TRN2u receive failed");
	}
	CHECK(v92_pcm_phase4_trn2u_line_rx_handoff(&training_rx, &handoff) ==
		V92_PCM_PHASE4_TRN2U_LINE_RX_OK && handoff.renegotiation &&
	      handoff.training_symbols == 12 &&
	      v92_upstream_control_line_rx_init(&control, &handoff) == V92_UPSTREAM_CONTROL_LINE_RX_OK,
	      "renegotiation control handoff failed");
	handoff.renegotiation = false;
	CHECK(!v92_pcm_phase4_trn2u_line_handoff_is_valid(&handoff),
	      "renegotiation weakened the initial training minimum");
	/* Exercise early SUVd-triggered termination and the ordinary 2400T train.
	 * The transmitter, not the control hunter, owns that policy. */
	unsigned train_symbols = compensated ? 1632 : 2400;
	while (training_tx.emitted_symbols < train_symbols) {
		int16_t sample;
		struct v92_control_event event;
		CHECK(v92_pcm_phase4_trn2u_next(&training_tx, &sample) == V92_PCM_PHASE4_TRN2U_OK,
		      "rate exchange training stopped");
		sample = wire_sample(&channel, sample);
		CHECK(v92_upstream_control_line_rx_sample(&control, sample, &event) == V92_UPSTREAM_CONTROL_LINE_RX_OK &&
		      rate_exchange_clock(&exchange, &sample), "training became a control record");
	}
	CHECK(exchange.station.receive.channel.symbols == 12 &&
	      v92_phase4_digital_station_select_upstream(&exchange.station, &cpd, &plan) ==
		      V92_PHASE4_DIGITAL_STATION_INVALID_STATE,
	      "short RRN training was treated as a complete channel measurement");
	const struct v92_suvu_parameters suvu = { .prefilter_level_db_q2_2 = -16 };
	struct v92_cpus_parameters cpus = { 0 };
	uint8_t frame[V92_CPU_MAX_FRAME_BYTES];
	size_t bits;
	unsigned mapping_bits = V92_PCM_MAPPING_FRAME_SYMBOLS *
		v92_phase4_modulator_bits_per_symbol(constellation);
	unsigned suvu_count = 0, cpu_count = 0;
	CHECK(v92_suvu_encode(&suvu, mapping_bits, frame, sizeof(frame), &bits) == V92_SUVU_OK &&
	      v92_phase4_record_tx_init(&record, &training_tx.modulator, frame, sizeof(frame), bits) ==
		V92_PHASE4_RECORD_TX_OK, "SUVu encode failed");
	for (unsigned which = 0; which < 7; ++which) {
		if (which) {
			cpus.data_rate_number = which < 5 ? 10 : 0;
			cpus.acknowledges_cpd = which >= 3;
			CHECK(v92_cpus_encode(&cpus, mapping_bits, frame, sizeof(frame), &bits) == V92_CPU_OK &&
			      v92_phase4_record_tx_start_next(&record, frame, sizeof(frame), bits) ==
				V92_PHASE4_RECORD_TX_OK, "CPus encode failed");
		}
		while (!v92_phase4_record_tx_is_complete(&record)) {
			int16_t sample;
			struct v92_control_event event;
			CHECK(v92_phase4_record_tx_next(&record, &sample) == V92_PHASE4_RECORD_TX_OK,
			      "control transmit stopped");
			sample = wire_sample(&channel, sample);
			enum v92_upstream_control_line_rx_result r = v92_upstream_control_line_rx_sample(
				&control, sample, &event);
			CHECK(rate_exchange_clock(&exchange, &sample), "rate exchange control failed");
			CHECK(r == V92_UPSTREAM_CONTROL_LINE_RX_OK || r == V92_UPSTREAM_CONTROL_LINE_RX_EVENT,
			      "control receive stopped");
			if (r != V92_UPSTREAM_CONTROL_LINE_RX_EVENT)
				continue;
			if (event.type == V92_CONTROL_EVENT_SUVU) {
				CHECK(event.record.suvu.frame_result == V92_SUVU_OK &&
				      event.record.suvu.parameters.prefilter_level_db_q2_2 == -16,
				      "SUVu CRC/parameters did not survive rate request");
				suvu_count++;
			} else {
				CHECK(event.type == V92_CONTROL_EVENT_CPUS && event.record.cpus.frame_result == V92_CPU_OK &&
				      (event.record.cpus.parameters.data_rate_number == 0 ||
				       event.record.cpus.parameters.data_rate_number == 10),
				      "CPus did not survive rate request");
				cpu_count++;
			}
		}
		if (which == 2 || which == 4) {
			struct v92_cpu_parameters selected;
			retained_cpu.data_rate_number = 10;
			retained_cpu.acknowledges_cpd = which == 4;
			CHECK(v92_phase4_digital_station_cpu_parameters(&exchange.station, &selected) &&
			      memcmp(&selected, &retained_cpu, sizeof(selected)) == 0 &&
			      exchange.station.peer_acknowledged_cpd == (which == 4) &&
			      !exchange.station.cleardown.active,
			      "CPus lost the retained mapping or reused an old acknowledgement");
		}
	}
	CHECK(suvu_count == 1 && cpu_count >= 1, "missing post-Ru status/cleardown record");
	CHECK(exchange.station.cleardown.active && exchange.station.cleardown.peer_requested &&
	      !v92_phase4_digital_station_is_ed_ready(&exchange.station), "zero CPus did not select cleardown");
	for (unsigned i = 0; i < 4000 && !exchange.complete; ++i)
		CHECK(rate_exchange_clock(&exchange, NULL), "rate exchange cleardown stopped");
	CHECK(exchange.complete, "post-Ru zero-rate exchange did not release in bounded time");
	return true;
}

/* Two codec conversions with channel memory and post-codec host gain. Keep
 * wire octets: requantizing the .75-scaled samples produces different cells. */
static bool test_codec_model(int polarity, bool rm)
{
	enum { FRAMES = V92_B1U_FRAMES + 36 + V92_PCM_RX_DELAY_FRAMES, PREFIX = 6 };
	enum { RM_START = V92_B1U_FRAMES + 16, RM_PRIME = RM_START + 32, RM_END = RM_PRIME + 2 };
	unsigned frames = FRAMES + (rm ? 34u : 0u);
	struct channel training = { .gain = 1.0, .law = G711_LAW_PCMU, .polarity = polarity };
	struct v92_upstream_channel measured = { 0 };
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct v92_upstream_plan plan;
	struct v92_pcm_frame_tx transmitter;
	struct v92_phase4_record_tx e2u_tx;
	struct v92_e2u_rx e2u_rx;
	struct v92_upstream_data_rx receiver, initial;
	struct v92_observation frozen[2];
	uint8_t codes[(FRAMES + 34 + PREFIX) * V92_PCM_MAPPING_FRAME_SYMBOLS];
	uint32_t random = 0x35791bdu;
	int16_t previous = 0, second = 0;
	unsigned delivered = 0u;

	for (unsigned i = 0u; i < V92_UPSTREAM_CHANNEL_SYMBOLS; ++i) {
		random = random * UINT32_C(1664525) + UINT32_C(1013904223);
		int16_t sample = (int16_t)((int)(random >> 19) - TEST_LU);
		int16_t q = g711_decode_pcmu(g711_encode_pcmu(sample));
		int16_t observed = g711_decode_pcmu(g711_encode_pcmu(
			(int16_t)lround(.9 * q + .08 * previous + .02 * second)));

		CHECK(v92_upstream_channel_sample(&measured, sample,
			(int16_t)lround(.75 * observed)), "codec channel measurement failed");
		second = previous;
		previous = q;
	}
	CHECK(v92_cpd_direct_select(&cpd, G711_LAW_PCMU, 19u, TEST_LU, false) == V92_CPD_OK &&
	      v92_cpd_channel_design(&cpd, &measured, G711_LAW_PCMU, TEST_LU, &plan) &&
	      v92_pcm_mapping_init(&mapping, G711_LAW_PCMU, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_pcm_frame_tx_init(&transmitter, &mapping) == V92_PCM_FRAME_OK &&
	      prepare_e2u(&training, false, &e2u_tx, &e2u_rx) &&
	      v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping, &plan) ==
		V92_UPSTREAM_DATA_RX_OK && receiver.model.acquiring,
	      "codec model initialization failed");
	initial = receiver;
	previous = second = 0;
	for (unsigned f = 0u; f < frames + PREFIX; ++f) {
		uint8_t bits[V92_PCM_MAPPING_MAX_BITS];
		struct v92_pcm_frame frame;

		if (f < V92_B1U_FRAMES)
			memset(bits, 1, mapping.data_bits_per_frame);
		else
			payload(bits, mapping.data_bits_per_frame,
				f - V92_B1U_FRAMES - (rm && f >= RM_END ? 34u : 0u));
		/* Both coding hypotheses must retain two closely-spaced control
		 * epochs even while older payload is still in traceback. */
		if (rm ? f == RM_END : f == V92_B1U_FRAMES + 16u || f == V92_B1U_FRAMES + 18u) {
			transmitter.state.scrambler.history = 0u;
			transmitter.state.differential = 0u;
		}
		CHECK((rm && f >= RM_START && f < RM_END ?
			v92_pcm_frame_encode_rm(&transmitter, f >= RM_PRIME, &frame) :
			v92_pcm_frame_encode(&transmitter, bits, mapping.data_bits_per_frame, &frame)) ==
			V92_PCM_FRAME_OK, "codec frame encode failed");
		for (unsigned s = 0u; s < V92_PCM_MAPPING_FRAME_SYMBOLS; ++s) {
			int16_t sample;

			CHECK(v92_pcm_frame_scale(&mapping, TEST_LU, frame.v[s], &sample) ==
				V92_PCM_FRAME_OK, "codec frame scale failed");
			int16_t q = g711_decode_pcmu(g711_encode_pcmu(sample));
			codes[f * V92_PCM_MAPPING_FRAME_SYMBOLS + s] = g711_encode_pcmu(
				(int16_t)lround(polarity * (.9 * q + .08 * previous + .02 * second)));
			second = previous;
			previous = q;
		}
	}
	for (unsigned i = V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY;
	     receiver.line.received_frames < frames; ++i) {
		struct v92_upstream_data_frame frame = { 0 }, compared = { 0 };
		struct v92_upstream_data_rx owned = receiver;
		uint8_t code = codes[i];
		int16_t sample = (int16_t)lround(.75 * g711_decode_pcmu(code));

		if (receiver.line.symbol_count == 0u &&
		    (rm ? receiver.line.received_frames == RM_END :
		     receiver.line.received_frames == V92_B1U_FRAMES + 16u ||
		     receiver.line.received_frames == V92_B1U_FRAMES + 18u)) {
			CHECK(receiver.model.valid == 3u &&
			      v92_upstream_data_rx_reset_control(&receiver) == V92_UPSTREAM_DATA_RX_OK,
			      "paired codec control reset failed");
			owned = receiver;
		}
		enum v92_upstream_data_rx_result result = v92_upstream_data_rx_octet(
			&receiver, code, sample, true, &frame);

		CHECK(result == V92_UPSTREAM_DATA_RX_OK || result == V92_UPSTREAM_DATA_RX_FRAME,
		      "codec receiver stopped: %u at sample %u", result, i);
		CHECK(v92_upstream_data_rx_octet_owned(&owned, code, sample, true, &compared) == result &&
		      memcmp(&receiver, &owned, sizeof(owned)) == 0 &&
		      memcmp(&frame, &compared, sizeof(frame)) == 0 &&
		      v92_upstream_data_rx_is_valid(&receiver), "codec owned/checked disagreement");
		if (result != V92_UPSTREAM_DATA_RX_FRAME)
			continue;
		if (receiver.line.received_frames == V92_B1U_ACQUISITION_FRAMES) {
			CHECK(receiver.model.valid == 3u && !receiver.model.acquiring,
			      "codec B1u did not fit both hypotheses");
			memcpy(frozen, receiver.model.model, sizeof(frozen));
		} else if (receiver.line.received_frames > V92_B1U_ACQUISITION_FRAMES) {
			CHECK(memcmp(frozen, receiver.model.model, sizeof(frozen)) == 0,
			      "held-out B1u or payload retrained the codec models");
		}
		unsigned f = (unsigned)receiver.line.received_frames - 1u;
		enum v92_pcm_frame_rm expected_rm = !rm || f < RM_START +
			V92_PCM_RM_QUALIFY_FRAMES - 1u || f >= RM_END ? V92_PCM_FRAME_RM_NONE :
			f < RM_PRIME ? V92_PCM_FRAME_RM_SIGNAL :
			f == RM_PRIME ? V92_PCM_FRAME_RM_PRIME : V92_PCM_FRAME_RM_COMPLETE;
		CHECK(frame.rm == expected_rm, "codec RM stage differs at %u: %u/%u", f, frame.rm, expected_rm);
		if (rm && f >= RM_START + V92_PCM_RM_QUALIFY_FRAMES - 1u &&
		    f < RM_END + V92_PCM_RX_DELAY_FRAMES)
			CHECK(!frame.bit_count, "codec RM leaked a delayed word");
		if (!frame.bit_count || (rm && f >= RM_START + V92_PCM_RX_DELAY_FRAMES &&
			f < RM_END + V92_PCM_RX_DELAY_FRAMES))
			continue;
		uint8_t expected[V92_PCM_MAPPING_MAX_BITS];

		if (delivered < V92_B1U_FRAMES)
			memset(expected, 1, mapping.data_bits_per_frame);
		else
			payload(expected, mapping.data_bits_per_frame, delivered - V92_B1U_FRAMES);
		CHECK(!frame.ambiguous && frame.bit_count == mapping.data_bits_per_frame &&
		      memcmp(frame.bits, expected, frame.bit_count) == 0,
		      "codec payload frame %u changed (ambiguous=%u)", delivered, frame.ambiguous);
		++delivered;
	}
	CHECK(delivered == FRAMES - V92_PCM_RX_DELAY_FRAMES, "codec payload count changed");
	struct v92_upstream_data_rx saved = receiver;
	/* With no new likelihood evidence, equally ranked codec hypotheses can
	 * disagree about an old non-payload slot. Never compare unwritten bits
	 * from the suppressed branch or release a placeholder from the winner. */
	for (unsigned suppressed = 0u; suppressed < 2u; ++suppressed) {
		struct v92_upstream_data_rx paired = saved;
		struct v92_upstream_data_frame frame = { 0 };

		paired.model.primary = 0u;
		paired.model.cost[0] = paired.model.cost[1] = 0u;
		paired.model.alternate = paired.decoder;
		struct v92_pcm_frame_rx *branch = suppressed ? &paired.model.alternate : &paired.decoder;
		for (unsigned i = 0u; i < branch->path_count; ++i)
			branch->paths[i].nondata_frames = 1u;
		CHECK(v92_upstream_data_rx_is_valid(&paired), "paired non-payload fixture invalid");
		for (unsigned i = 0u; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i)
			CHECK(v92_upstream_data_rx_octet(&paired, 0u, 0, false, &frame) ==
				(i == V92_PCM_MAPPING_FRAME_SYMBOLS - 1u ? V92_UPSTREAM_DATA_RX_FRAME :
				 V92_UPSTREAM_DATA_RX_OK), "paired non-payload decoding failed");
		CHECK(paired.model.valid == 3u && paired.model.primary == 0u &&
			paired.model.cost[0] == 0u && paired.model.cost[1] == 0u &&
			(suppressed ? frame.bit_count == mapping.data_bits_per_frame && frame.ambiguous :
			 frame.bit_count == 0u) && v92_upstream_data_rx_is_valid(&paired),
			"tied codec models mishandled branch %u's non-payload slot", suppressed);
	}
	struct v92_upstream_data_frame output, unchanged;
	memset(&output, 0xa5, sizeof(output));
	unchanged = output;
	CHECK(v92_upstream_data_rx_sample(&receiver, 0, &output) == V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      memcmp(&saved, &receiver, sizeof(saved)) == 0 &&
	      memcmp(&unchanged, &output, sizeof(output)) == 0,
	      "active codec model invented a wire code");
	/* Erasures have no likelihood evidence, but must still report raw
	 * residual. They cannot silently disappear from the quality guard. */
	for (unsigned i = 0u; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i)
		CHECK(v92_upstream_data_rx_octet(&receiver, 0u, INT16_MAX, false, &output) ==
		      (i == V92_PCM_MAPPING_FRAME_SYMBOLS - 1u ? V92_UPSTREAM_DATA_RX_FRAME :
			V92_UPSTREAM_DATA_RX_OK), "erased codec frame stopped");
	CHECK(output.squared_error > 1e9, "erased media hid its raw residual");
	/* Finite model parameters can still overflow at a codec boundary. A
	 * failed frame must not partly advance either coding hypothesis. */
	receiver = saved;
	for (unsigned i = 0u; i < 2u; ++i) {
		receiver.model.model[i].quantizer_gain = DBL_MAX;
		for (unsigned j = 0u; j < V92_OBSERVATION_INTERVALS; ++j)
			receiver.model.model[i].intervals[j].main = DBL_MAX;
	}
	CHECK(v92_upstream_data_rx_is_valid(&receiver), "numeric-failure fixture is invalid");
	for (unsigned i = 0u; i < V92_PCM_MAPPING_FRAME_SYMBOLS - 1u; ++i)
		CHECK(v92_upstream_data_rx_octet(&receiver, 0u, 100, true, &output) ==
		      V92_UPSTREAM_DATA_RX_OK, "numeric-failure prefix stopped");
	saved = receiver;
	unchanged = output;
	CHECK(v92_upstream_data_rx_octet(&receiver, 0u, 100, true, &output) ==
	      V92_UPSTREAM_DATA_RX_NO_PATH, "unrepresentable codec frame did not fail");
	saved.failed = true;
	CHECK(memcmp(&saved, &receiver, sizeof(saved)) == 0 &&
	      memcmp(&unchanged, &output, sizeof(output)) == 0,
	      "failed codec frame advanced state or wrote payload");
	/* No retained fit may cross a mapping/retrain epoch. */
	CHECK(v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping, &plan) ==
	      V92_UPSTREAM_DATA_RX_OK && memcmp(&initial, &receiver, sizeof(initial)) == 0,
	      "new training epoch retained codec state");
	for (unsigned missing = 0u; missing < 2u; ++missing) {
		receiver = initial;
		while (receiver.line.received_frames == 0u) {
			enum v92_upstream_data_rx_result result = missing ?
				v92_upstream_data_rx_octet(&receiver, 0xffu, 0, false, &output) :
				v92_upstream_data_rx_sample(&receiver, 0, &output);
			CHECK(result == V92_UPSTREAM_DATA_RX_OK || result == V92_UPSTREAM_DATA_RX_FRAME,
			      "missing acquisition input stopped linear fallback");
		}
		CHECK(!receiver.model.acquiring && !receiver.model.valid &&
		      v92_upstream_data_rx_is_valid(&receiver), "missing media trained a codec model");
	}
	/* A multi-tap prefilter is outside this model, not a one-tap fit with
	 * silently ignored coefficients. Keep the existing linear receiver. */
	cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD].coefficient[plan.delay + 1u] = 1;
	CHECK(v92_pcm_mapping_init(&mapping, G711_LAW_PCMU, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping, &plan) ==
	      V92_UPSTREAM_DATA_RX_OK && !receiver.model.acquiring && !receiver.model.valid,
	      "unsupported prefilter enabled the codec model");
	return true;
}

int main(void)
{
	if (!test_codec_model(1, false) || !test_codec_model(-1, false) ||
	    !test_codec_model(1, true) || !test_codec_model(-1, true))
		return 1;
	fprintf(stderr, "[ RUN      ] retained PCM data-to-Ru control channel\n");
	for (unsigned law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (int polarity = -1; polarity <= 1; polarity += 2)
			for (unsigned extended = 0; extended < 2; ++extended)
				for (unsigned compensated = 0; compensated < 2; ++compensated)
					for (unsigned constellation = V92_CONSTELLATION_4_POINT;
					     constellation <= V92_CONSTELLATION_8_POINT; constellation *= 2u)
						if (!test_rate_request_line(law, polarity, extended, compensated, constellation))
							return 1;
	fprintf(stderr, "[       OK ] retained PCM data-to-Ru control channel\n");
	for (unsigned int law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (int polarity = -1; polarity <= 1; polarity += 2)
			for (unsigned int extended = 0u; extended < 2u; ++extended)
				for (unsigned int fixture = 0u; fixture < 3u; ++fixture)
					if (!test_channel_plan((enum g711_law)law, polarity, extended,
							fixture == 1u, fixture == 2u))
						return 1;
	if (!test_quantized_level_tracking(G711_LAW_PCMU, 19u, false, TEST_LU, false) ||
	    !test_quantized_level_tracking(G711_LAW_PCMA, 19u, false, TEST_LU, false) ||
	    !test_quantized_level_tracking(G711_LAW_PCMU, 13u, false, TEST_LU, false) ||
	    !test_quantized_level_tracking(G711_LAW_PCMA, 13u, false, TEST_LU, false) ||
	    !test_quantized_level_tracking(G711_LAW_PCMU, 8u, true, TEST_LU, false) ||
	    !test_quantized_level_tracking(G711_LAW_PCMU, 19u, false, 7164, true) ||
	    !test_quantized_level_tracking(G711_LAW_PCMA, 19u, false, 7040, true))
		return 1;
	const struct {
		uint8_t rate;
		double gain;
		double postcursor;
	} cases[] = {
		{ 1u, 1.0, 0.0 }, { 8u, 1.0, 0.0 }, { 13u, 1.0, 0.0 },
		{ 1u, 0.75, 0.0 }, { 8u, 0.75, 0.0 }, { 13u, 0.75, 0.0 },
		/* Channel memory at the two lower rates; dense CPd channel
		 * selection is not established by these line tests. */
		{ 1u, 0.75, 0.10 }, { 8u, 0.75, 0.10 },
	};

	for (unsigned int law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (int polarity = -1; polarity <= 1; polarity += 2)
			for (unsigned int extended = 0u; extended < 2u; ++extended)
				for (size_t index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index)
					if (!run_stream((enum g711_law)law, polarity, extended,
							cases[index].rate, cases[index].gain,
							cases[index].postcursor, STREAM_DATA))
						return 1;
	for (unsigned int law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned int distorted = 0u; distorted < 2u; ++distorted)
			if (!run_stream((enum g711_law)law, -1, false, 8u,
					distorted ? 0.75 : 1.0, distorted ? 0.10 : 0.0, STREAM_RESET))
				return 1;
	const uint8_t rm_rates[] = { 1u, 16u, 19u };
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned i = 0; i < ARRAY_LENGTH(rm_rates); ++i)
			if (!test_fast_control(law, rm_rates[i])) return 1;
	for (unsigned law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (int polarity = -1; polarity <= 1; polarity += 2)
			for (unsigned i = 0u; i < ARRAY_LENGTH(rm_rates); ++i)
				if (!run_stream(law, polarity, false, rm_rates[i], 1.0, 0.0, STREAM_RM)) return 1;
	return test_owned_boundaries() && test_rejection_and_failure() ? 0 : 1;
}
