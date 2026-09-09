// SPDX-License-Identifier: BSD-3-Clause

#include <v92_pcm_frame.h>
#include <v92_cpd_direct.h>
#include <v92_cpu.h>

#include "test.h"

#include <limits.h>
#include <math.h>
#include <string.h>

enum { UPSTREAM_LEVEL = 4096, DATA_FRAMES = 1000 };

static int16_t quantize(enum g711_law law, int16_t value)
{
	return law == G711_LAW_PCMU ?
		g711_decode_pcmu(g711_encode_pcmu(value)) :
		g711_decode_pcma(g711_encode_pcma(value));
}

static bool mapping_init(
	struct v92_pcm_mapping *mapping, enum g711_law law, uint8_t rate,
	bool filtered)
{
	struct v92_cpd_parameters cpd;

	if (v92_cpd_direct_init(&cpd, law, rate, false) != V92_CPD_OK)
		return false;
	if (filtered) {
		cpd.trellis = V92_CPD_TRELLIS_64_STATE;
		memset(cpd.filter, 0, sizeof(cpd.filter));
		cpd.filter[V92_CPD_PRECODER_FEED_FORWARD] =
			(struct v92_cpd_filter){ .count = 1u,
				.coefficient = { 16384 } };
		cpd.filter[V92_CPD_PRECODER_FEEDBACK] =
			(struct v92_cpd_filter){ .count = 1u,
				.coefficient = { -4096 } };
		cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD] =
			(struct v92_cpd_filter){ .count = 2u,
				.coefficient = { 24576, 8192 } };
		cpd.filter[V92_CPD_PREFILTER_FEEDBACK] =
			(struct v92_cpd_filter){ .count = 1u,
				.coefficient = { 4096 } };
	}
	return v92_pcm_mapping_init(mapping, law, &cpd) == V92_PCM_MAPPING_OK;
}

static void source_bits(uint8_t *bits, size_t count, unsigned int frame)
{
	uint32_t random = UINT32_C(0x62d71a95) ^ (frame - V92_B1U_FRAMES);

	for (size_t bit = 0u; bit < count; ++bit) {
		random = random * UINT32_C(1664525) + UINT32_C(1013904223);
		bits[bit] = frame < V92_B1U_FRAMES ? 1u : (uint8_t)(random >> 31);
	}
}

static bool waveform(
	struct v92_pcm_frame_tx *transmitter, const uint8_t *bits,
	int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS], int16_t level)
{
	struct v92_pcm_frame frame;

	if (v92_pcm_frame_encode(transmitter, bits,
			transmitter->mapping.data_bits_per_frame, &frame) !=
	    V92_PCM_FRAME_OK)
		return false;
	for (size_t symbol = 0u; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		int16_t sample;

		if (v92_pcm_frame_scale(&transmitter->mapping, level,
				frame.v[symbol], &sample) != V92_PCM_FRAME_OK)
			return false;
		samples[symbol] = quantize(transmitter->mapping.law, sample);
	}
	return true;
}

static bool run_stream(enum g711_law law, uint8_t rate, bool filtered)
{
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx transmitter;
	struct v92_pcm_frame_tx reencoded;
	struct v92_pcm_frame_rx receiver;
	int16_t history[V92_PCM_RX_DELAY_FRAMES + 1][V92_PCM_MAPPING_FRAME_SYMBOLS];
	unsigned int delivered = 0u;
	unsigned int ambiguous = 0u;
	unsigned int different = 0u;

	CHECK(mapping_init(&mapping, law, rate, filtered) &&
	      v92_pcm_frame_tx_init(&transmitter, &mapping) == V92_PCM_FRAME_OK &&
	      v92_pcm_frame_rx_init(&receiver, &mapping, UPSTREAM_LEVEL) ==
		      V92_PCM_FRAME_OK, "receive initialization failed");
	reencoded = transmitter;
	for (unsigned int frame = 0u;
	     frame < V92_B1U_FRAMES + DATA_FRAMES + V92_PCM_RX_DELAY_FRAMES; ++frame) {
		struct v92_pcm_frame_decision decision;
		uint8_t bits[V92_PCM_MAPPING_MAX_BITS];
		uint8_t decoded[V92_PCM_MAPPING_MAX_BITS];
		int16_t *samples = history[frame % (V92_PCM_RX_DELAY_FRAMES + 1u)];
		int16_t replay[V92_PCM_MAPPING_FRAME_SYMBOLS];
		unsigned int output_frame;

		source_bits(bits, mapping.data_bits_per_frame, frame);
		memset(decoded, 0xa5, sizeof(decoded));
		CHECK(waveform(&transmitter, bits, samples, UPSTREAM_LEVEL) &&
		      v92_pcm_frame_decode(&receiver, samples, frame < V92_B1U_FRAMES,
				decoded, mapping.data_bits_per_frame, &decision) ==
			      V92_PCM_FRAME_OK &&
		      v92_pcm_frame_rx_is_valid(&receiver),
		      "law %u rate %u filter %u frame %u did not decode",
		      (unsigned int)law, rate, filtered, frame);
		CHECK(decision.bits_ready == (frame >= V92_PCM_RX_DELAY_FRAMES),
		      "frame %u changed the bounded delivery delay", frame);
		if (!decision.bits_ready) {
			CHECK(!decision.ambiguous, "buffered frame was ambiguous");
			for (size_t bit = 0u; bit < sizeof(decoded); ++bit)
				CHECK(decoded[bit] == 0xa5, "buffering wrote payload");
			continue;
		}
		delivered++;
		ambiguous += decision.ambiguous;
		output_frame = frame - V92_PCM_RX_DELAY_FRAMES;
		source_bits(bits, mapping.data_bits_per_frame, output_frame);
		for (size_t bit = 0u; bit < mapping.data_bits_per_frame; ++bit) {
			different += bits[bit] != decoded[bit];
			CHECK(bits[bit] == decoded[bit] || decision.ambiguous,
			      "law %u rate %u filter %u frame %u bit %zu unreported error",
			      (unsigned int)law, rate, filtered, output_frame, bit);
		}
		/* Even an ambiguous delivered sequence must describe the received
		 * waveform. This pins the zero-distance CPd collision, rather than
		 * treating arbitrary errors as acceptable training noise. */
		CHECK(waveform(&reencoded, decoded, replay, UPSTREAM_LEVEL) &&
		      memcmp(replay, history[output_frame % (V92_PCM_RX_DELAY_FRAMES + 1u)],
			      sizeof(replay)) == 0,
		      "law %u rate %u filter %u frame %u changed the wire waveform",
		      (unsigned int)law, rate, filtered, output_frame);
		for (size_t bit = mapping.data_bits_per_frame; bit < sizeof(decoded); ++bit)
			CHECK(decoded[bit] == 0xa5, "decoder exceeded payload width");
	}
	CHECK(delivered == V92_B1U_FRAMES + DATA_FRAMES, "payload delivery count changed");
	if (!filtered && law == G711_LAW_PCMA && rate == 16u)
		CHECK(ambiguous != 0u && different != 0u,
		      "dense A-law CPd collision was not identified");
	else
		CHECK(different == 0u, "clean channel changed payload");
	return true;
}

static bool test_control_epoch(enum g711_law law, uint8_t rate, bool filtered)
{
	enum { CONTROL_FRAME = V92_B1U_FRAMES + 16, RECORDS = 24 };
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx transmitter;
	struct v92_pcm_frame_rx receiver;
	const struct v92_cpus_parameters parameters = {
		.data_rate_number = 22u, .acknowledges_cpd = true,
	};
	uint8_t record[V92_CPUS_MAX_FRAME_BYTES];
	size_t record_bits;
	unsigned int delivered = 0u;

	CHECK(mapping_init(&mapping, law, rate, filtered) &&
	      v92_pcm_frame_tx_init(&transmitter, &mapping) == V92_PCM_FRAME_OK &&
	      v92_pcm_frame_rx_init(&receiver, &mapping, UPSTREAM_LEVEL) == V92_PCM_FRAME_OK &&
	      v92_cpus_encode(&parameters, mapping.data_bits_per_frame,
		      record, sizeof(record), &record_bits) == V92_CPU_OK,
	      "control fixture failed");
	unsigned int end = CONTROL_FRAME + RECORDS * record_bits / mapping.data_bits_per_frame;

	for (unsigned int frame = 0u; frame < end + V92_PCM_RX_DELAY_FRAMES; ++frame) {
		uint8_t bits[V92_PCM_MAPPING_MAX_BITS];
		uint8_t decoded[V92_PCM_MAPPING_MAX_BITS];
		int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS];
		struct v92_pcm_frame_decision decision;

		if (frame == CONTROL_FRAME) {
			struct v92_pcm_frame_tx expected_tx = transmitter;
			struct v92_pcm_frame_rx expected_rx = receiver;

			expected_tx.state.scrambler.history = 0u;
			expected_tx.state.differential = 0u;
			for (size_t path = 0u; path < expected_rx.path_count; ++path) {
				expected_rx.paths[path].descrambler.history = 0u;
				expected_rx.paths[path].differential = 0u;
			}
			CHECK(receiver.pending_frames == V92_PCM_RX_DELAY_FRAMES &&
			      v92_pcm_frame_tx_reset_control(&transmitter) == V92_PCM_FRAME_OK &&
			      v92_pcm_frame_rx_reset_control(&receiver) == V92_PCM_FRAME_OK &&
			      memcmp(&transmitter, &expected_tx, sizeof(transmitter)) == 0 &&
			      memcmp(&receiver, &expected_rx, sizeof(receiver)) == 0,
			      "control reset changed mapping, histories, metrics, or pending bits");
		}
		if (frame < CONTROL_FRAME) {
			source_bits(bits, mapping.data_bits_per_frame, frame);
		} else {
			for (size_t bit = 0u; bit < mapping.data_bits_per_frame; ++bit) {
				size_t position = ((frame - CONTROL_FRAME) *
					mapping.data_bits_per_frame + bit) % record_bits;

				bits[bit] = (record[position / 8u] >> (7u - position % 8u)) & 1u;
			}
		}
		CHECK(waveform(&transmitter, bits, samples, UPSTREAM_LEVEL) &&
		      v92_pcm_frame_decode(&receiver, samples, frame < V92_B1U_FRAMES,
			      decoded, mapping.data_bits_per_frame, &decision) == V92_PCM_FRAME_OK,
		      "control decoding failed");
		CHECK(decision.bits_ready == (frame >= V92_PCM_RX_DELAY_FRAMES),
		      "control reset dropped pending delivery");
		if (!decision.bits_ready)
			continue;
		unsigned int output = frame - V92_PCM_RX_DELAY_FRAMES;

		if (output < CONTROL_FRAME) {
			source_bits(bits, mapping.data_bits_per_frame, output);
		} else {
			for (size_t bit = 0u; bit < mapping.data_bits_per_frame; ++bit) {
				size_t position = ((output - CONTROL_FRAME) *
					mapping.data_bits_per_frame + bit) % record_bits;

				bits[bit] = (record[position / 8u] >> (7u - position % 8u)) & 1u;
			}
		}
		CHECK(!decision.ambiguous && memcmp(bits, decoded, mapping.data_bits_per_frame) == 0,
		      "law %u rate %u filter %u output %u changed across control reset",
		      (unsigned int)law, rate, filtered, output);
		delivered++;
	}
	CHECK(delivered == end, "control reset lost or duplicated payload");
	return true;
}

static bool test_constellation_coordinates(
	enum g711_law law, uint8_t rate, enum v92_cpd_trellis trellis)
{
	enum { CONTROL_FRAME = V92_B1U_FRAMES + 32, FRAMES = V92_B1U_FRAMES + 128 };
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx transmitter;
	struct v92_pcm_frame_rx receiver;
	unsigned int delivered = 0u;

	CHECK(mapping_init(&mapping, law, rate, true), "filtered mapping failed");
	struct v92_cpd_parameters cpd = mapping.cpd;

	cpd.trellis = trellis;
	cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD] = (struct v92_cpd_filter){
		.count = 62u, .coefficient = { [60] = 24576, [61] = 8192 },
	};
	/* CPd specifies linear points, not G.711 codewords. Moving off the codec
	 * grid catches accidental re-quantization of compensated observations. */
	for (size_t set = 0u; set < V92_CPD_MAX_CONSTELLATIONS; ++set) {
		for (size_t point = 0u; point < cpd.constellation[set].count; ++point)
			cpd.constellation[set].positive_linear[point]++;
	}
	CHECK(v92_pcm_mapping_init(&mapping, law, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_pcm_frame_tx_init(&transmitter, &mapping) == V92_PCM_FRAME_OK &&
	      v92_pcm_frame_rx_init_constellation(&receiver, &mapping) == V92_PCM_FRAME_OK &&
	      receiver.observation == V92_PCM_FRAME_CONSTELLATION &&
	      v92_pcm_frame_rx_is_valid(&receiver), "constellation initialization failed");
	for (unsigned int frame = 0u; frame < FRAMES + V92_PCM_RX_DELAY_FRAMES; ++frame) {
		struct v92_pcm_frame encoded;
		struct v92_pcm_frame_decision decision;
		uint8_t bits[V92_PCM_MAPPING_MAX_BITS], decoded[V92_PCM_MAPPING_MAX_BITS];
		int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS];

		if (frame == CONTROL_FRAME) {
			struct v92_pcm_frame_rx expected = receiver;

			for (size_t path = 0u; path < expected.path_count; ++path) {
				expected.paths[path].descrambler.history = 0u;
				expected.paths[path].differential = 0u;
			}
			CHECK(v92_pcm_frame_tx_reset_control(&transmitter) == V92_PCM_FRAME_OK &&
			      v92_pcm_frame_rx_reset_control(&receiver) == V92_PCM_FRAME_OK &&
			      memcmp(&receiver, &expected, sizeof(receiver)) == 0,
			      "control reset changed coordinates, filters, or pending payload");
		}
		source_bits(bits, mapping.data_bits_per_frame, frame);
		CHECK(v92_pcm_frame_encode(&transmitter, bits, mapping.data_bits_per_frame,
			      &encoded) == V92_PCM_FRAME_OK, "constellation encoding failed");
		/* This is the decoder boundary AFTER channel compensation and delay
		 * removal, not a simulation of a complete line receiver. */
		for (size_t symbol = 0u; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol)
			samples[symbol] = (int16_t)(encoded.u[symbol] + (symbol % 2u ? 1 : -1));
		memset(decoded, 0xa5, sizeof(decoded));
		CHECK(v92_pcm_frame_decode(&receiver, samples, frame < V92_B1U_FRAMES,
			      decoded, mapping.data_bits_per_frame, &decision) == V92_PCM_FRAME_OK &&
		      v92_pcm_frame_rx_is_valid(&receiver), "constellation decoding failed");
		CHECK(decision.squared_error == V92_PCM_MAPPING_FRAME_SYMBOLS &&
		      memcmp(decision.predicted_samples, encoded.u, sizeof(encoded.u)) == 0 &&
		      memcmp(decision.frame.u, encoded.u, sizeof(encoded.u)) == 0 &&
		      memcmp(decision.frame.x, encoded.x, sizeof(encoded.x)) == 0 &&
		      memcmp(decision.frame.v, encoded.v, sizeof(encoded.v)) == 0,
		      "law %u rate %u trellis %u frame %u changed observations or filter history",
		      law, rate, trellis, frame);
		CHECK(decision.bits_ready == (frame >= V92_PCM_RX_DELAY_FRAMES) && !decision.ambiguous,
		      "constellation delivery delay or ambiguity changed");
		if (decision.bits_ready) {
			source_bits(bits, mapping.data_bits_per_frame, frame - V92_PCM_RX_DELAY_FRAMES);
			CHECK(memcmp(bits, decoded, mapping.data_bits_per_frame) == 0,
			      "constellation payload changed across training/control");
			delivered++;
		}
		for (size_t bit = decision.bits_ready ? mapping.data_bits_per_frame : 0u;
		     bit < sizeof(decoded); ++bit)
			CHECK(decoded[bit] == 0xa5, "constellation decoder wrote unowned payload");
	}
	CHECK(delivered == FRAMES, "constellation decoder lost or duplicated payload");
	return true;
}

static bool test_noise_training(bool proportional)
{
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx transmitter;
	struct v92_pcm_frame_rx receiver;
	struct v92_pcm_frame_noise trained = { 0 };
	uint64_t energy = 0u;

	CHECK(mapping_init(&mapping, G711_LAW_PCMU, 1u, false) &&
	      v92_pcm_frame_tx_init(&transmitter, &mapping) == V92_PCM_FRAME_OK &&
	      v92_pcm_frame_rx_init_constellation(&receiver, &mapping) == V92_PCM_FRAME_OK,
	      "noise fixture initialization failed");
	for (unsigned frame = 0u; frame < V92_B1U_FRAMES + 32u; ++frame) {
		struct v92_pcm_frame encoded;
		struct v92_pcm_frame_decision decision;
		uint8_t bits[V92_PCM_MAPPING_MAX_BITS], decoded[V92_PCM_MAPPING_MAX_BITS];
		int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS];
		uint64_t error = 0u;

		source_bits(bits, mapping.data_bits_per_frame, frame);
		CHECK(v92_pcm_frame_encode(&transmitter, bits, mapping.data_bits_per_frame, &encoded) ==
		      V92_PCM_FRAME_OK, "noise fixture encoding failed");
		for (unsigned i = 0u; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i) {
			/* Exclude the acquisition half, then provide an exact variance
			 * slope of 1/1024 or amplitude-independent unit residual. */
			int residual = frame < V92_B1U_FRAMES / 2u ? 0 :
				proportional ? encoded.u[i] / 32 : 1;

			samples[i] = (int16_t)(encoded.u[i] + residual);
			error += (uint64_t)((int64_t)residual * residual);
			if (frame >= V92_B1U_FRAMES / 2u && frame < V92_B1U_FRAMES)
				energy += (uint64_t)((int64_t)encoded.u[i] * encoded.u[i]);
		}
		CHECK(v92_pcm_frame_decode(&receiver, samples, frame < V92_B1U_FRAMES, decoded,
			mapping.data_bits_per_frame, &decision) == V92_PCM_FRAME_OK &&
		      v92_pcm_frame_rx_is_valid(&receiver) && decision.squared_error == error,
		      "noise weighting changed raw residual evidence");
		if (decision.bits_ready) {
			source_bits(bits, mapping.data_bits_per_frame, frame - V92_PCM_RX_DELAY_FRAMES);
			CHECK(!decision.ambiguous && memcmp(bits, decoded, mapping.data_bits_per_frame) == 0,
			      "noise weighting changed payload");
		}
		if (frame + 1u == V92_B1U_FRAMES) {
			double expected = proportional ? (double)(V92_B1U_SYMBOLS / 2u) / energy : 0.0;

			CHECK(fabs(receiver.noise.relative_variance - expected) < 1e-12 &&
			      receiver.noise.frames == V92_B1U_FRAMES && receiver.noise.signal_energy == 0.0 &&
			      receiver.noise.signal_fourth == 0.0 && receiver.noise.error_energy == 0.0 &&
			      receiver.noise.error_signal_energy == 0.0,
			      "B1u noise fit or completed accumulator cleanup failed");
			trained = receiver.noise;
		} else if (frame >= V92_B1U_FRAMES) {
			CHECK(memcmp(&receiver.noise, &trained, sizeof(trained)) == 0,
			      "payload trained the noise model");
		} else {
			CHECK(receiver.noise.relative_variance == 0.0, "partial B1u enabled weighting");
		}
	}
	CHECK(v92_pcm_frame_rx_reset_control(&receiver) == V92_PCM_FRAME_OK &&
	      memcmp(&receiver.noise, &trained, sizeof(trained)) == 0,
	      "control reset lost the trained noise model");
	return true;
}

static bool test_compensated_channel(enum g711_law law)
{
	enum { DELAY = 59, FRAMES = V92_B1U_FRAMES + 128 + V92_PCM_RX_DELAY_FRAMES };
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx transmitter;
	struct v92_pcm_frame_rx receiver;
	int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS];
	int32_t previous = 0;
	unsigned int received_frames = 0u;

	CHECK(mapping_init(&mapping, law, 13u, false), "channel mapping failed");
	struct v92_cpd_parameters cpd = mapping.cpd;

	/* H(z) = 2 + z^-1/2, P1(z) = -z^-1/4, Z2(z) = z^-59/2.
	 * Thus H Z2 / (1 - P1) = z^-59. Receive the actual filtered output
	 * through H, discard its decision delay, then decode recovered u(n).
	 * This isolates filter/channel composition, without a codec or timing
	 * loop; the line owner must also handle those in a real connection. */
	memset(cpd.filter, 0, sizeof(cpd.filter));
	cpd.filter[V92_CPD_PRECODER_FEEDBACK] = (struct v92_cpd_filter){
		.count = 1u, .coefficient = { -4096 },
	};
	cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD] = (struct v92_cpd_filter){
		.count = DELAY + 1u, .coefficient = { [DELAY] = 16384 },
	};
	CHECK(v92_pcm_mapping_init(&mapping, law, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_pcm_frame_tx_init(&transmitter, &mapping) == V92_PCM_FRAME_OK &&
	      v92_pcm_frame_rx_init_constellation(&receiver, &mapping) == V92_PCM_FRAME_OK,
	      "channel initialization failed");
	for (unsigned int frame = 0u; received_frames < FRAMES; ++frame) {
		struct v92_pcm_frame encoded;
		uint8_t bits[V92_PCM_MAPPING_MAX_BITS];

		source_bits(bits, mapping.data_bits_per_frame, frame);
		CHECK(v92_pcm_frame_encode(&transmitter, bits, mapping.data_bits_per_frame,
			      &encoded) == V92_PCM_FRAME_OK, "channel encoding failed");
		for (unsigned int symbol = 0u;
		     symbol < V92_PCM_MAPPING_FRAME_SYMBOLS && received_frames < FRAMES; ++symbol) {
			unsigned int time = frame * V92_PCM_MAPPING_FRAME_SYMBOLS + symbol;
			int32_t observed = 2 * encoded.v[symbol] + previous / 2;

			previous = encoded.v[symbol];
			if (time < DELAY)
				continue;
			unsigned int position = (time - DELAY) % V92_PCM_MAPPING_FRAME_SYMBOLS;

			CHECK(observed >= INT16_MIN && observed <= INT16_MAX, "channel clipped u(n)");
			samples[position] = (int16_t)observed;
			if (position != V92_PCM_MAPPING_FRAME_SYMBOLS - 1u)
				continue;
			struct v92_pcm_frame_decision decision;
			uint8_t decoded[V92_PCM_MAPPING_MAX_BITS];

			CHECK(v92_pcm_frame_decode(&receiver, samples, received_frames < V92_B1U_FRAMES,
				      decoded, mapping.data_bits_per_frame, &decision) == V92_PCM_FRAME_OK &&
			      decision.squared_error <= 9u * V92_PCM_MAPPING_FRAME_SYMBOLS &&
			      decision.bits_ready == (received_frames >= V92_PCM_RX_DELAY_FRAMES) &&
			      !decision.ambiguous, "compensated channel decoding failed");
			if (decision.bits_ready) {
				source_bits(bits, mapping.data_bits_per_frame,
					received_frames - V92_PCM_RX_DELAY_FRAMES);
				CHECK(memcmp(decoded, bits, mapping.data_bits_per_frame) == 0,
				      "compensated channel changed payload");
			}
			received_frames++;
		}
	}
	return true;
}

static bool test_uncertain_level(enum g711_law law, uint8_t rate, int16_t level)
{
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx transmitter;
	struct v92_pcm_frame_rx receiver;
	int16_t measured = quantize(law, level);
	int16_t low = measured, high = measured;

	while (low > 1 && quantize(law, (int16_t)(low - 1)) == measured)
		low--;
	while (high < INT16_MAX && quantize(law, (int16_t)(high + 1)) == measured)
		high++;
	CHECK(mapping_init(&mapping, law, rate, false) &&
	      v92_pcm_frame_tx_init(&transmitter, &mapping) == V92_PCM_FRAME_OK &&
	      v92_pcm_frame_rx_init(&receiver, &mapping, measured) == V92_PCM_FRAME_OK,
	      "uncertain-level initialization failed");
	for (unsigned int frame = 0u; frame < V92_B1U_FRAMES + 128u + V92_PCM_RX_DELAY_FRAMES; ++frame) {
		struct v92_pcm_frame_decision decision;
		uint8_t bits[V92_PCM_MAPPING_MAX_BITS], decoded[V92_PCM_MAPPING_MAX_BITS];
		int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS];
		bool b1u = frame < V92_B1U_FRAMES;

		source_bits(bits, mapping.data_bits_per_frame, frame);
		CHECK(waveform(&transmitter, bits, samples, level) &&
		      v92_pcm_frame_decode(&receiver, samples, b1u, decoded,
			      mapping.data_bits_per_frame, &decision) == V92_PCM_FRAME_OK,
		      "uncertain-level frame did not decode");
		if (b1u) {
			struct v92_pcm_frame_rx saved = receiver;

			CHECK(v92_pcm_frame_rx_refine_level(&receiver, &decision.frame,
				      samples, low, high) == V92_PCM_FRAME_OK &&
			      receiver.minimum_level >= low && receiver.maximum_level <= high &&
			      receiver.minimum_level <= level && receiver.maximum_level >= level,
			      "law %u rate %u level %d frame %u lost feasible LU", law, rate, level, frame);
			saved.upstream_level = receiver.upstream_level;
			saved.minimum_level = low = receiver.minimum_level;
			saved.maximum_level = high = receiver.maximum_level;
			CHECK(memcmp(&receiver, &saved, sizeof(receiver)) == 0,
			      "refinement changed coding history");
		} else {
			CHECK(receiver.minimum_level == low && receiver.maximum_level == high,
			      "unknown payload changed the level interval");
			CHECK(decision.squared_error == 0u &&
			      memcmp(decision.predicted_samples, samples, sizeof(samples)) == 0,
			      "law %u rate %u level %d frame %u invented a codeword", law, rate, level, frame);
		}
		if (decision.bits_ready) {
			source_bits(bits, mapping.data_bits_per_frame, frame - V92_PCM_RX_DELAY_FRAMES);
			CHECK(!decision.ambiguous && memcmp(bits, decoded, mapping.data_bits_per_frame) == 0,
			      "law %u rate %u level %d frame %u changed data", law, rate, level, frame);
		}
	}
	return true;
}

static bool test_rejection_and_training_evidence(void)
{
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_rx receiver;
	struct v92_pcm_frame_rx saved;
	struct v92_pcm_frame_decision decision;
	struct v92_pcm_frame_decision saved_decision;
	int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS] = { 0 };
	uint8_t bits[V92_PCM_MAPPING_MAX_BITS];
	uint8_t saved_bits[V92_PCM_MAPPING_MAX_BITS];

	CHECK(mapping_init(&mapping, G711_LAW_PCMU, 1u, false) &&
	      v92_pcm_frame_rx_init(&receiver, &mapping, UPSTREAM_LEVEL) ==
		      V92_PCM_FRAME_OK, "initialization failed");
	saved = receiver;
	memset(&decision, 0xa5, sizeof(decision));
	saved_decision = decision;
	memset(bits, 0x5a, sizeof(bits));
	memcpy(saved_bits, bits, sizeof(bits));
	CHECK(!v92_pcm_frame_rx_is_valid(NULL) &&
	      v92_pcm_frame_rx_reset_control(NULL) == V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_rx_init(NULL, &mapping, UPSTREAM_LEVEL) ==
		      V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_rx_init(&receiver, NULL, UPSTREAM_LEVEL) ==
		      V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_rx_init(&receiver, &mapping, 0) ==
		      V92_PCM_FRAME_INVALID_PARAMETERS &&
	      v92_pcm_frame_decode(NULL, samples, false, bits, 36u, &decision) ==
		      V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_decode(&receiver, NULL, false, bits, 36u, &decision) ==
		      V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_decode(&receiver, samples, false, NULL, 36u, &decision) ==
		      V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_decode(&receiver, samples, false, bits, 36u, NULL) ==
		      V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_decode(&receiver, samples, false, bits, 35u, &decision) ==
		      V92_PCM_FRAME_OUT_OF_RANGE &&
	      v92_pcm_frame_decode(&receiver, samples, false,
		      receiver.paths[0].pending[0], 36u, &decision) ==
		      V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_decode(&receiver, samples, false,
		      (uint8_t *)&decision, 36u, &decision) ==
		      V92_PCM_FRAME_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0 &&
	      memcmp(&decision, &saved_decision, sizeof(decision)) == 0 &&
	      memcmp(bits, saved_bits, sizeof(bits)) == 0,
	      "rejected call changed state or output");
	receiver.paths[0].pending[0][0] = 1u;
	CHECK(!v92_pcm_frame_rx_is_valid(&receiver), "unowned payload accepted");
	struct v92_pcm_frame_rx invalid = receiver;

	CHECK(v92_pcm_frame_rx_reset_control(&receiver) == V92_PCM_FRAME_INVALID_PARAMETERS &&
	      memcmp(&receiver, &invalid, sizeof(receiver)) == 0,
	      "invalid reset changed receiver");
	receiver = saved;
	receiver.minimum_level = 0;
	CHECK(!v92_pcm_frame_rx_is_valid(&receiver), "nonpositive LU interval accepted");
	receiver = saved;
	receiver.minimum_level++;
	CHECK(!v92_pcm_frame_rx_is_valid(&receiver), "LU below its interval accepted");
	receiver = saved;
	receiver.maximum_level--;
	CHECK(!v92_pcm_frame_rx_is_valid(&receiver), "LU above its interval accepted");
	receiver = saved;
	receiver.paths[0].ambiguous_frames = 1u;
	CHECK(!v92_pcm_frame_rx_is_valid(&receiver), "unowned ambiguity accepted");
	receiver = saved;
	receiver.paths[0].metric = 1u;
	CHECK(!v92_pcm_frame_rx_is_valid(&receiver), "unnormalized metric accepted");
	receiver = saved;
	receiver.path_count = 2u;
	receiver.paths[1] = receiver.paths[0];
	receiver.paths[1].metric = UINT64_MAX;
	CHECK(v92_pcm_frame_decode(&receiver, samples, true, bits, 36u, &decision) ==
		      V92_PCM_FRAME_OK && decision.squared_error > 0u &&
	      v92_pcm_frame_rx_is_valid(&receiver) &&
	      !decision.bits_ready && memcmp(bits, saved_bits, sizeof(bits)) == 0,
	      "B1u hid damaged training or saturated metrics corrupted state");
	CHECK(strcmp(v92_pcm_frame_result_name(V92_PCM_FRAME_NO_PATH), "no path") == 0,
	      "missing no-path result name");
	CHECK(v92_pcm_frame_rx_init_constellation(&receiver, &mapping) == V92_PCM_FRAME_OK,
	      "constellation initialization failed");
	saved = receiver;
	struct v92_pcm_mapping invalid_mapping = mapping;

	invalid_mapping.data_bits_per_frame = 0u;
	CHECK(v92_pcm_frame_rx_init_constellation(NULL, &mapping) == V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_rx_init_constellation(&receiver, NULL) == V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_rx_init_constellation(&receiver, &invalid_mapping) ==
		      V92_PCM_FRAME_INVALID_PARAMETERS &&
	      v92_pcm_frame_rx_refine_level(&receiver, &decision.frame, samples, 1, 2) ==
		      V92_PCM_FRAME_INVALID_PARAMETERS &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "constellation receiver accepted level refinement or changed on rejection");
	receiver.observation = (enum v92_pcm_frame_observation)-1;
	CHECK(!v92_pcm_frame_rx_is_valid(&receiver), "unknown observation coordinates accepted");
	receiver = saved;
	receiver.upstream_level = 1;
	CHECK(!v92_pcm_frame_rx_is_valid(&receiver), "constellation receiver accepted a gain");
	receiver = saved;
	receiver.minimum_level = 1;
	CHECK(!v92_pcm_frame_rx_is_valid(&receiver), "constellation receiver accepted minimum LU");
	receiver = saved;
	receiver.maximum_level = 1;
	CHECK(!v92_pcm_frame_rx_is_valid(&receiver), "constellation receiver accepted maximum LU");
	receiver = saved;
	receiver.noise.frames = V92_B1U_FRAMES + 1u;
	CHECK(!v92_pcm_frame_rx_is_valid(&receiver), "noise accepted excess training");
	receiver = saved;
	receiver.noise.relative_variance = NAN;
	CHECK(!v92_pcm_frame_rx_is_valid(&receiver), "noise accepted NaN");
	receiver = saved;
	receiver.noise.signal_energy = 1.0;
	CHECK(!v92_pcm_frame_rx_is_valid(&receiver), "noise accepted unowned moments");
	receiver = saved;
	receiver.noise.frames = V92_B1U_FRAMES;
	receiver.noise.relative_variance = -1.0;
	CHECK(!v92_pcm_frame_rx_is_valid(&receiver), "noise accepted a negative weight");
	receiver = saved;
	CHECK(v92_pcm_frame_decode(&receiver, samples, true, bits, 36u, &decision) ==
		      V92_PCM_FRAME_OK && decision.squared_error > 0u && !decision.bits_ready,
	      "constellation B1u suppressed damaged-training evidence");
	return true;
}

int main(void)
{
	static const int16_t levels[] = { 4096, 4104, 4224, 4351 };

	for (unsigned int law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		static const uint8_t rates[] = { 1u, 13u, 19u };

		if (!test_compensated_channel((enum g711_law)law))
			return 1;
		for (size_t rate = 0u; rate < sizeof(rates) / sizeof(rates[0]); ++rate) {
			for (enum v92_cpd_trellis trellis = V92_CPD_TRELLIS_16_STATE;
			     trellis <= V92_CPD_TRELLIS_64_STATE; ++trellis) {
				if (!test_constellation_coordinates((enum g711_law)law, rates[rate], trellis))
					return 1;
			}
		}
		for (size_t level = 0u; level < sizeof(levels) / sizeof(levels[0]); ++level) {
			if (!test_uncertain_level((enum g711_law)law, 13u, levels[level]) ||
			    !test_uncertain_level((enum g711_law)law, 19u, levels[level]))
				return 1;
		}
		for (uint8_t rate = 1u; rate <= V92_CPD_MAX_DATA_RATE_NUMBER; ++rate) {
			if (!run_stream((enum g711_law)law, rate, false) ||
			    !test_control_epoch((enum g711_law)law, rate, false))
				return 1;
		}
		if (!run_stream((enum g711_law)law, 13u, true) ||
		    !test_control_epoch((enum g711_law)law, 13u, true))
			return 1;
	}
	return test_noise_training(false) && test_noise_training(true) &&
		test_rejection_and_training_evidence() ? 0 : 1;
}
