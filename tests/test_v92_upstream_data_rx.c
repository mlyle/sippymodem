// SPDX-License-Identifier: BSD-3-Clause

#include <v92_cpd_direct.h>
#include <v92_data_quality.h>
#include <v92_e2u_rx.h>
#include <v92_phase4_record_tx.h>
#include <v92_upstream_data_rx.h>
#include <v92_upstream_data_tx.h>

#include "test.h"

#include <float.h>
#include <math.h>
#include <string.h>

enum { TEST_LU = 4096, DATA_FRAMES = 80 };

struct channel {
	double gain;
	double postcursor;
	int16_t previous;
	enum g711_law law;
	int8_t polarity;
};

static int16_t wire_sample(struct channel *channel, int16_t sample)
{
	int16_t mixed = (int16_t)lround(channel->gain * sample +
		channel->postcursor * channel->previous);

	channel->previous = sample;
	return channel->polarity * (channel->law == G711_LAW_PCMU ?
		g711_decode_pcmu(g711_encode_pcmu(mixed)) :
		g711_decode_pcma(g711_encode_pcma(mixed)));
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

/* A trained FIR with the real causal delay, not a zero-delay stand-in. Feed
 * E2u's first delayed samples before exporting its receive-side boundary. */
static bool prepare_e2u_at_level(
	struct channel *channel, bool extended,
	struct v92_phase4_record_tx *transmitter, struct v92_e2u_rx *receiver,
	int16_t measured_lu, double adaptation)
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

	CHECK(v92_phase4_modulator_init(&modulator, V92_CONSTELLATION_4_POINT,
			0u, TEST_LU) == V92_PHASE4_MODULATOR_OK &&
	      v92_phase4_demodulator_init(&handoff.demodulator,
			V92_CONSTELLATION_4_POINT, 0u, measured_lu) == V92_PHASE4_MODULATOR_OK &&
	      v92_phase4_record_tx_init_e2u(transmitter, &modulator, extended) ==
		      V92_PHASE4_RECORD_TX_OK &&
	      qam_equalizer_init_geometry(&handoff.equalizer, &reference, &reference,
			V92_PCM_PHASE4_TRN2U_LINE_EQUALIZER_TAPS,
			V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY) == QAM_EQUALIZER_OK,
	      "E2u fixture initialization failed");
	/* Finite inverse of the known two-tap channel; B1u continues adaptation. */
	double coefficient = 1.0;
	for (size_t tap = V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY;
	     tap < handoff.equalizer.tap_count; ++tap) {
		handoff.equalizer.coefficients[tap].in_phase = coefficient;
		coefficient *= -channel->postcursor / channel->gain;
	}
	for (size_t index = 0u; index <= V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY; ++index) {
		int16_t sample = TEST_LU;
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
	      v92_phase4_modulator_level(V92_CONSTELLATION_4_POINT, measured_lu, 0u, 0u,
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
		(int16_t)lround(TEST_LU * channel->gain), 0.002);
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

static bool test_quantized_level_tracking(enum g711_law law, uint8_t rate, bool drift)
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
		g711_decode_pcmu(g711_encode_pcmu(TEST_LU)) :
		g711_decode_pcma(g711_encode_pcma(TEST_LU));

	/* TRN1u estimates LU from quantized samples, not the transmitter's
	 * exact scalar. Carry the fast training step through real E2u, then
	 * require usable B1u and clean idle/payload. Dense 40k/48k cases check
	 * quantization stability; the lower-rate case checks ongoing tracking. */
	CHECK(v92_cpd_direct_init(&cpd, law, rate, false) == V92_CPD_OK &&
	      v92_pcm_mapping_init(&mapping, law, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_upstream_data_tx_init(&transmitter, &mapping, TEST_LU) ==
		      V92_UPSTREAM_DATA_TX_OK &&
	      prepare_e2u_at_level(&channel, false, &e2u_tx, &e2u_rx,
			measured_lu, 0.05), "tracking fixture failed");
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
	      v92_upstream_data_rx_init(&receiver, &handoff, &mapping) ==
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
	CHECK(trained_minimum <= TEST_LU && trained_maximum >= TEST_LU,
	      "training excluded the transmitted LU");
	CHECK(b1u_error <= (double)V92_B1U_SYMBOLS *
		      V92_DATA_MAX_RMS_ERROR * V92_DATA_MAX_RMS_ERROR,
	      "law %u rate %u rejected clean quantized B1u: RMS %.2f",
	      (unsigned int)law, rate, sqrt(b1u_error / V92_B1U_SYMBOLS));
	return true;
}

static bool run_stream(enum g711_law law, int8_t polarity, bool extended,
		       uint8_t rate, double gain, double postcursor, bool control)
{
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
	      v92_upstream_data_rx_init(&receiver, &handoff, &mapping) ==
		      V92_UPSTREAM_DATA_RX_OK &&
	      transmitter.b1u_symbols_remaining == V92_B1U_SYMBOLS -
		      V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY &&
	      memcmp(&receiver.line.equalizer, &handoff.line.equalizer,
		      sizeof(handoff.line.equalizer)) == 0,
	      "E2u lost pending B1u inputs or changed the trained FIR");
	for (unsigned int symbol = 0u;
	     symbol < (V92_B1U_FRAMES + DATA_FRAMES + V92_PCM_RX_DELAY_FRAMES) *
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
		if (control && transmitter.queue_count == 0u &&
		    (source_frames == 16u || source_frames == 18u)) {
			struct v92_upstream_data_tx expected = transmitter;

			expected.encoder.state.scrambler.history = 0u;
			expected.encoder.state.differential = 0u;
			CHECK(v92_upstream_data_tx_reset_control(&transmitter) ==
				      V92_UPSTREAM_DATA_TX_OK &&
			      memcmp(&transmitter, &expected, sizeof(expected)) == 0,
			      "transmit control reset changed retained state");
		}
		if (control && receiver.line.symbol_count == 0u &&
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
		if (control && receiver.line.received_frames == V92_B1U_FRAMES + 16u &&
		    receiver.line.symbol_count != 0u) {
			struct v92_upstream_data_rx expected = receiver;

			CHECK(v92_upstream_data_rx_reset_control(&receiver) ==
				      V92_UPSTREAM_DATA_RX_INVALID_STATE &&
			      memcmp(&receiver, &expected, sizeof(expected)) == 0,
			      "partial equalized frame accepted a control reset");
		}
		if (control && source_frames == 17u && transmitter.queue_count != 0u) {
			struct v92_upstream_data_tx expected = transmitter;

			CHECK(v92_upstream_data_tx_reset_control(&transmitter) ==
				      V92_UPSTREAM_DATA_TX_INVALID_STATE &&
			      memcmp(&transmitter, &expected, sizeof(expected)) == 0,
			      "partial transmit frame accepted a control reset");
		}
		CHECK(v92_upstream_data_tx_next(&transmitter, source, &source_frames,
				&sample) == V92_UPSTREAM_DATA_TX_OK, "data transmitter stopped");
		result = v92_upstream_data_rx_sample(&receiver,
			wire_sample(&channel, sample), &frame);
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
		CHECK((frame.bit_count != 0u) == (decisions >= V92_PCM_RX_DELAY_FRAMES),
		      "payload delay changed");
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
		if (frame.bit_count == 0u)
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
	      v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping) ==
		      V92_UPSTREAM_DATA_RX_OK, "rejection fixture failed");
	const double slower_steps[] = { 0.0, 0.001 };
	for (size_t index = 0u; index < sizeof(slower_steps) / sizeof(slower_steps[0]); ++index) {
		struct v92_upstream_rx slower = e2u_rx.signal;
		struct v92_upstream_data_rx initialized;

		slower.line.adaptation = slower_steps[index];
		CHECK(v92_upstream_data_rx_init(&initialized, &slower, &mapping) ==
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
	      v92_upstream_data_rx_init(NULL, &e2u_rx.signal, &mapping) ==
		      V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      v92_upstream_data_rx_init(&receiver, NULL, &mapping) ==
		      V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, NULL) ==
		      V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &receiver.decoder.mapping) ==
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
	CHECK(v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping) ==
		      V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "unconsumed TRN2u tail accepted");
	e2u_rx.signal.training_tail_remaining = 0u;
	mapping.law = G711_LAW_PCMA;
	CHECK(v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping) ==
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
	CHECK(v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping) ==
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
	CHECK(v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping) ==
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
	CHECK(v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping) ==
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
	return true;
}

int main(void)
{
	if (!test_quantized_level_tracking(G711_LAW_PCMU, 19u, false) ||
	    !test_quantized_level_tracking(G711_LAW_PCMA, 19u, false) ||
	    !test_quantized_level_tracking(G711_LAW_PCMU, 13u, false) ||
	    !test_quantized_level_tracking(G711_LAW_PCMA, 13u, false) ||
	    !test_quantized_level_tracking(G711_LAW_PCMU, 8u, true))
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
							cases[index].postcursor, false))
						return 1;
	for (unsigned int law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned int distorted = 0u; distorted < 2u; ++distorted)
			if (!run_stream((enum g711_law)law, -1, false, 8u,
					distorted ? 0.75 : 1.0, distorted ? 0.10 : 0.0, true))
				return 1;
	return test_rejection_and_failure() ? 0 : 1;
}
