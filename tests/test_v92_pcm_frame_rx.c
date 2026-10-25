// SPDX-License-Identifier: BSD-3-Clause

#include "wide_uint.h"

#include <v92_pcm_frame.h>
#include <v92_cpd_direct.h>
#include <v92_cpu.h>
#include <v92_observation.h>

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

		if (v92_pcm_frame_scale(transmitter->mapping.cpd.four_times_gain_q0_16, level,
				frame.v[symbol], &sample) != V92_PCM_FRAME_OK)
			return false;
		samples[symbol] = quantize(transmitter->mapping.law, sample);
	}
	return true;
}

static bool run_stream(enum g711_law law, uint8_t rate, bool filtered, bool collision)
{
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx transmitter;
	struct v92_pcm_frame_tx reencoded;
	struct v92_pcm_frame_rx receiver;
	int16_t history[V92_PCM_RX_DELAY_FRAMES + 1][V92_PCM_MAPPING_FRAME_SYMBOLS];
	unsigned int delivered = 0u;
	unsigned int ambiguous = 0u;
	unsigned int different = 0u;

	CHECK(mapping_init(&mapping, law, rate, filtered), "mapping initialization failed");
	if (collision) {
		/* Preserve a valid but non-injective peer CPd independently of our
		 * direct selector: six modulus-64/six modulus-32 intervals, the first
		 * 64 positive A-law levels, and the original normalized B1u gain. */
		struct v92_cpd_parameters cpd = mapping.cpd;

		CHECK(law == G711_LAW_PCMA && rate == 16u && !filtered, "invalid collision fixture");
		cpd.four_times_gain_q0_16 = 708u;
		for (unsigned symbol = 0u; symbol < V92_CPD_MODULUS_COUNT; ++symbol)
			cpd.modulus[symbol] = symbol < 6u ? 64u : 32u;
		cpd.constellation[0].count = 64u;
		for (unsigned point = 0u; point < 64u; ++point)
			CHECK(g711_ucode_sample(law, (uint8_t)(point + 1u), true,
				&cpd.constellation[0].positive_linear[point]), "collision point failed");
		CHECK(v92_pcm_mapping_init(&mapping, law, &cpd) == V92_PCM_MAPPING_OK,
			"collision mapping failed");
	}
	CHECK(v92_pcm_frame_tx_init(&transmitter, &mapping) == V92_PCM_FRAME_OK &&
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
	if (collision)
		CHECK(ambiguous != 0u && different != 0u,
		      "dense A-law CPd collision was not identified");
	else if (law == G711_LAW_PCMA && rate == 17u && !filtered) {
		/* This level-neutral template also admits indistinguishable
		 * payload histories. Both choices must report ambiguity and
		 * reproduce the exact wire waveform, as checked above. */
		CHECK(ambiguous != 0u, "45.3k template collision was not identified");
	} else
		CHECK(different == 0u, "clean channel changed payload");
	return true;
}

static bool test_peer_point_selection(enum g711_law law, enum v92_cpd_trellis trellis)
{
	enum { FRAMES = V92_B1U_FRAMES + 32, SHIFT = 32 };
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx tx;
	struct v92_pcm_frame_rx rx;

	CHECK(v92_cpd_direct_init(&cpd, law, 1u, false) == V92_CPD_OK,
		"peer point CPd failed");
	cpd.trellis = trellis;
	memset(cpd.filter, 0, sizeof(cpd.filter));
	cpd.constellation[0].count = 64u;
	for (unsigned point = 0; point < 64u; point++)
		cpd.constellation[0].positive_linear[point] = (int16_t)((2u * point + 1u) * 128u);
	CHECK(v92_pcm_mapping_init(&mapping, law, &cpd) == V92_PCM_MAPPING_OK &&
		v92_pcm_frame_tx_init(&tx, &mapping) == V92_PCM_FRAME_OK &&
		v92_pcm_frame_rx_init_constellation(&rx, &mapping) == V92_PCM_FRAME_OK,
		"peer point initialization failed");
	for (unsigned f = 0; f < FRAMES + V92_PCM_RX_DELAY_FRAMES; f++) {
		struct v92_pcm_frame frame;
		struct v92_pcm_frame_decision decision;
		uint8_t bits[V92_PCM_MAPPING_MAX_BITS], decoded[V92_PCM_MAPPING_MAX_BITS];
		int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS];

		source_bits(bits, mapping.data_bits_per_frame, f);
		CHECK(v92_pcm_frame_encode(&tx, bits, mapping.data_bits_per_frame, &frame) ==
			V92_PCM_FRAME_OK, "peer point encoding failed");
		for (unsigned i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; i++) {
			/* V.92 6.4.2 permits any point in E(Ki). Our minimum-|x|
			 * transmitter policy is not a receive constraint. This shift
			 * preserves both the digit and eta modulo 4 (the inverse map),
			 * so the next trellis state is unchanged. There are no filters. */
			unsigned step = cpd.modulus[i] * (i % 4u == 3u ? 2u : 1u);
			int eta = frame.signed_index[i];
			eta += eta < 0 ? -SHIFT : SHIFT;
			CHECK(SHIFT % step == 0u && eta >= -64 && eta < 64,
				"peer point left its equivalence class");
			int16_t u = cpd.constellation[0].positive_linear[eta < 0 ? -eta - 1 : eta];
			samples[i] = eta < 0 ? (int16_t)-u : u;
			CHECK((int32_t)samples[i] * samples[i] > (int32_t)frame.u[i] * frame.u[i],
				"peer point did not exercise a higher-power choice");
		}
		CHECK(v92_pcm_frame_decode(&rx, samples, f < V92_B1U_FRAMES, decoded,
			mapping.data_bits_per_frame, &decision) == V92_PCM_FRAME_OK &&
			decision.squared_error == 0u && !decision.ambiguous &&
			decision.bits_ready == (f >= V92_PCM_RX_DELAY_FRAMES) &&
			!memcmp(decision.frame.u, samples, sizeof(samples)),
			"law %u trellis %u frame %u rejected peer point selection", law, trellis, f);
		if (decision.bits_ready) {
			source_bits(bits, mapping.data_bits_per_frame, f - V92_PCM_RX_DELAY_FRAMES);
			CHECK(!memcmp(bits, decoded, mapping.data_bits_per_frame),
				"peer point selection changed payload");
		}
	}
	return true;
}

static bool test_direct_44k_level(enum g711_law law, int16_t level)
{
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx transmitter;
	struct v92_pcm_frame_rx receiver;

	CHECK(v92_cpd_direct_select(&cpd, law, 16u, level, false) == V92_CPD_OK &&
	      v92_pcm_mapping_init(&mapping, law, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_pcm_frame_tx_init(&transmitter, &mapping) == V92_PCM_FRAME_OK &&
	      v92_pcm_frame_rx_init(&receiver, &mapping, level) == V92_PCM_FRAME_OK,
	      "44k law %u LU %d initialization failed", law, level);
	for (unsigned frame = 0u;
	     frame < V92_B1U_FRAMES + 2u * DATA_FRAMES + V92_PCM_RX_DELAY_FRAMES; ++frame) {
		uint8_t bits[V92_PCM_MAPPING_MAX_BITS], decoded[V92_PCM_MAPPING_MAX_BITS];
		int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS];
		struct v92_pcm_frame_decision decision;

		source_bits(bits, mapping.data_bits_per_frame, frame);
		CHECK(waveform(&transmitter, bits, samples, level) &&
		      v92_pcm_frame_decode(&receiver, samples, frame < V92_B1U_FRAMES,
			      decoded, mapping.data_bits_per_frame, &decision) == V92_PCM_FRAME_OK &&
		      decision.bits_ready == (frame >= V92_PCM_RX_DELAY_FRAMES) && !decision.ambiguous,
		      "44k law %u LU %d frame %u did not decode unambiguously", law, level, frame);
		if (decision.bits_ready) {
			source_bits(bits, mapping.data_bits_per_frame, frame - V92_PCM_RX_DELAY_FRAMES);
			CHECK(memcmp(bits, decoded, mapping.data_bits_per_frame) == 0,
			      "44k law %u LU %d frame %u changed bits", law, level, frame);
		}
	}
	return true;
}

static bool test_precodec_noise(bool uncertain)
{
	/* A compact CPd with a(eta)=30000*(x+7*x^3)/8, x=(2eta+1)/77.
	 * At LU 7777, input 8711 encodes to 8960; noise -8 yields 8448.
	 * Decoded distance incorrectly favors input 7965 (codeword 8064).
	 * Keep this peer mapping independent of our constellation selector. */
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx tx;
	struct v92_pcm_frame_rx rx;
	const int16_t level = 7777;
	uint32_t random = UINT32_C(0xac62e5b9);
	uint64_t residual = 0;

	CHECK(v92_cpd_direct_init(&cpd, G711_LAW_PCMA, 19, false) == V92_CPD_OK,
		"CPd init failed");
	cpd.four_times_gain_q0_16 = 20;
	cpd.constellation[0].count = 39;
	memset(cpd.constellation[0].positive_linear, 0, sizeof(cpd.constellation[0].positive_linear));
	for (unsigned i = 0; i < 12; ++i)
		cpd.modulus[i] = i % 4 == 3 ? 39 : 78;
	for (unsigned i = 0; i < 39; ++i) {
		uint64_t x = 2 * i + 1, width = 77;
		uint64_t denominator = 8 * width * width * width;
		uint64_t numerator = 30000 * x * (width * width + 7 * x * x);
		cpd.constellation[0].positive_linear[i] = (int16_t)((numerator + denominator / 2) / denominator);
	}
	CHECK(v92_pcm_mapping_init(&mapping, G711_LAW_PCMA, &cpd) == V92_PCM_MAPPING_OK &&
		v92_pcm_frame_tx_init(&tx, &mapping) == V92_PCM_FRAME_OK &&
		v92_pcm_frame_rx_init(&rx, &mapping, level) == V92_PCM_FRAME_OK, "init failed");
	for (unsigned f = 0; f < V92_B1U_FRAMES + 400 + V92_PCM_RX_DELAY_FRAMES; ++f) {
		uint8_t bits[V92_PCM_MAPPING_MAX_BITS], decoded[V92_PCM_MAPPING_MAX_BITS];
		int16_t samples[12];
		struct v92_pcm_frame frame;
		struct v92_pcm_frame_decision decision;
		bool b1u = f < V92_B1U_FRAMES;

		if (f == V92_B1U_FRAMES && uncertain) {
			rx.upstream_level = 7788;
			rx.minimum_level = 7771;
			rx.maximum_level = 7788;
		}
		source_bits(bits, mapping.data_bits_per_frame, f);
		CHECK(v92_pcm_frame_encode(&tx, bits, mapping.data_bits_per_frame, &frame) == V92_PCM_FRAME_OK,
			"encode failed");
		for (unsigned i = 0; i < 12; ++i) {
			int16_t sample;
			CHECK(v92_pcm_frame_scale(mapping.cpd.four_times_gain_q0_16, level, frame.v[i], &sample) == V92_PCM_FRAME_OK,
				"scale failed");
			random = random * UINT32_C(1664525) + UINT32_C(1013904223);
			int noise = b1u ? 0 : (int)((random >> 16) % 17) - 8;
			samples[i] = quantize(G711_LAW_PCMA, (int16_t)(sample + noise));
		}
		CHECK(v92_pcm_frame_decode(&rx, samples, b1u, decoded, mapping.data_bits_per_frame,
			&decision) == V92_PCM_FRAME_OK, "decode failed");
		uint64_t error = 0;
		for (unsigned i = 0; i < 12; ++i) {
			int64_t e = (int64_t)samples[i] - decision.predicted_samples[i];
			uint64_t nearest = UINT64_MAX;
			for (int lu = rx.minimum_level; lu <= rx.maximum_level; ++lu) {
				int16_t predicted;
				CHECK(v92_pcm_frame_scale(mapping.cpd.four_times_gain_q0_16, (int16_t)lu, decision.frame.v[i],
					&predicted) == V92_PCM_FRAME_OK, "prediction failed");
				int64_t difference = (int64_t)samples[i] - quantize(G711_LAW_PCMA, predicted);
				uint64_t cost = (uint64_t)(difference * difference);
				if (cost < nearest) nearest = cost;
			}
			CHECK((uint64_t)(e * e) == nearest, "prediction left reachable codec levels");
			error += (uint64_t)(e * e);
		}
		CHECK(error == decision.squared_error, "likelihood hid physical residual");
		residual += error;
		if (decision.bits_ready) {
			source_bits(bits, mapping.data_bits_per_frame, f - V92_PCM_RX_DELAY_FRAMES);
			CHECK(!decision.ambiguous && !memcmp(bits, decoded, mapping.data_bits_per_frame),
				"uncertain %u frame %u changed data", uncertain, f);
		}
	}
	CHECK(residual > 0, "test did not perturb codec output");
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
	enum g711_law law, uint8_t rate, enum v92_cpd_trellis trellis, bool uneven)
{
	enum { CONTROL_FRAME = V92_B1U_FRAMES + 32, FRAMES = V92_B1U_FRAMES + 128 };
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx transmitter;
	struct v92_pcm_frame_rx receiver;
	unsigned int delivered = 0u;

	CHECK(mapping_init(&mapping, law, rate, true), "filtered mapping failed");
	struct v92_cpd_parameters cpd = mapping.cpd;

	cpd.trellis = trellis;
	if (uneven) {
		/* Cross odd residues and a singleton fourth-symbol class with
		 * six different full-size constellations and each trellis. */
		for (unsigned symbol = 0u; symbol < 12u; ++symbol)
			cpd.modulus[symbol] = 8u;
		cpd.modulus[0] = 255u;
		cpd.modulus[3] = 1u;
		cpd.modulus[6] = 3u;
		memset(cpd.constellation, 0, sizeof(cpd.constellation));
		for (unsigned phase = 0u; phase < 6u; ++phase) {
			cpd.frame_constellation[phase] = (uint8_t)phase;
			cpd.constellation[phase].count = 128u;
			for (unsigned point = 0u; point < 128u; ++point)
				cpd.constellation[phase].positive_linear[point] =
					(int16_t)((2u * point + 1u) * (16u + phase));
		}
	}
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

static bool test_trellis_polarity(void)
{
	const enum v34_trellis_selection selections[] = {
		V34_TRELLIS_16_STATE, V34_TRELLIS_32_STATE, V34_TRELLIS_64_STATE,
	};
	for (unsigned mode = 0u; mode < ARRAY_LENGTH(selections); ++mode) {
		uint8_t mask = selections[mode] == V34_TRELLIS_64_STATE ? 32u : 0u;
		for (uint8_t first = 0u; first < V34_SUBSET_LABEL_COUNT; ++first) {
			for (uint8_t second = 0u; second < V34_SUBSET_LABEL_COUNT; ++second) {
				struct v34_convolutional_input normal, inverted;
				/* Negating both odd grid coordinates complements label bit 1. */
				CHECK(v34_convolutional_input_from_subset_labels(first, second, &normal) == V34_TRELLIS_OK &&
					v34_convolutional_input_from_subset_labels(first ^ 2u, second ^ 2u, &inverted) == V34_TRELLIS_OK,
					"polarity subset conversion failed");
				for (uint8_t state = 0u; state < v34_trellis_state_count(selections[mode]); ++state) {
					struct v34_convolutional_encoder a = { .state = state, .selection = selections[mode] };
					struct v34_convolutional_encoder b = { .state = state ^ mask, .selection = selections[mode] };
					uint8_t a0, b0;
					CHECK(v34_convolutional_encoder_output(&a, &a0) == V34_TRELLIS_OK &&
						v34_convolutional_encoder_output(&b, &b0) == V34_TRELLIS_OK && a0 == b0 &&
						v34_convolutional_encoder_advance(&a, &normal) == V34_TRELLIS_OK &&
						v34_convolutional_encoder_advance(&b, &inverted) == V34_TRELLIS_OK &&
						b.state == (a.state ^ mask), "polarity trellis transform failed");
				}
			}
		}
	}
	return true;
}

static bool polarity_mapping(struct v92_pcm_mapping *mapping,
	enum g711_law law, uint8_t rate, enum v92_cpd_trellis trellis, bool balanced)
{
	struct v92_cpd_parameters cpd;

	CHECK(v92_cpd_direct_init(&cpd, law, rate, false) == V92_CPD_OK,
		"polarity CPd initialization failed");
	if (balanced) {
		/* Include spare mixed-radix words, not only power-of-two maps.
		 * Choose the minimum N with (2N)^9 * N^3 >= 2^K. */
		unsigned n;
		for (n = 1u; n <= 64u; ++n) {
			struct wide_uint capacity = wide_from_u64(1);
			for (unsigned i = 0u; i < V92_CPD_MODULUS_COUNT; ++i)
				wide_multiply_u32(capacity, i % 4u == 3u ? n : 2u * n, &capacity);
			if (wide_compare(capacity, wide_shift_left(wide_from_u64(1), pcm_upstream_frame_bits(rate))) >= 0)
				break;
		}
		CHECK(n <= 64u, "polarity modulus exceeds fixture constellation");
		for (unsigned i = 0u; i < V92_CPD_MODULUS_COUNT; ++i)
			cpd.modulus[i] = (uint8_t)(i % 4u == 3u ? n : 2u * n);
	}
	cpd.trellis = trellis;
	/* Exact linear coordinates isolate coding from codec and gain errors. */
	cpd.constellation[0].count = 64u;
	for (unsigned i = 0u; i < 64u; ++i)
		cpd.constellation[0].positive_linear[i] = (int16_t)((2u * i + 1u) * 128u);
	CHECK(v92_pcm_mapping_init(mapping, law, &cpd) == V92_PCM_MAPPING_OK,
		"polarity mapping initialization failed");
	return true;
}

static bool test_constellation_polarity(enum g711_law law, uint8_t rate,
	enum v92_cpd_trellis trellis, bool balanced, int polarity)
{
	enum { PAYLOAD_FRAMES = 100 };
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx tx;
	struct v92_pcm_frame_rx rx;
	unsigned delivered = 0u;

	CHECK(polarity_mapping(&mapping, law, rate, trellis, balanced) &&
		v92_pcm_frame_tx_init(&tx, &mapping) == V92_PCM_FRAME_OK &&
		v92_pcm_frame_rx_init_constellation(&rx, &mapping) == V92_PCM_FRAME_OK,
		"polarity frame pair initialization failed");
	for (unsigned f = 0u; f < V92_B1U_FRAMES + PAYLOAD_FRAMES + V92_PCM_RX_DELAY_FRAMES; ++f) {
		uint8_t bits[V92_PCM_MAPPING_MAX_BITS], decoded[V92_PCM_MAPPING_MAX_BITS];
		struct v92_pcm_frame frame;
		struct v92_pcm_frame_decision decision;
		int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS];

		source_bits(bits, mapping.data_bits_per_frame, f);
		/* Polarity acquisition must not hide a genuine held-out B1u error. */
		if (f == V92_B1U_FRAMES - 1u) bits[0] = 0u;
		CHECK(v92_pcm_frame_encode(&tx, bits, mapping.data_bits_per_frame, &frame) ==
			V92_PCM_FRAME_OK, "polarity frame encoding failed");
		for (unsigned i = 0u; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i)
			samples[i] = (int16_t)(polarity * frame.u[i]);
		memset(decoded, 0xa5, sizeof(decoded));
		CHECK(v92_pcm_frame_decode(&rx, samples, f < V92_B1U_FRAMES,
			decoded, mapping.data_bits_per_frame, &decision) == V92_PCM_FRAME_OK &&
			v92_pcm_frame_rx_is_valid(&rx) && decision.squared_error == 0u &&
			!memcmp(decision.frame.u, samples, sizeof(samples)),
			"polarity decode: law=%u rate=%u trellis=%u balanced=%u polarity=%d frame=%u",
			law, rate, trellis, balanced, polarity, f);
		CHECK(decision.bits_ready == (f >= V92_PCM_RX_DELAY_FRAMES),
			"polarity acquisition changed delivery delay");
		for (size_t i = decision.bits_ready ? mapping.data_bits_per_frame : 0u;
		     i < sizeof(decoded); ++i)
			CHECK(decoded[i] == 0xa5, "polarity decoder wrote unowned payload");
		if (!decision.bits_ready) continue;
		unsigned original = f - V92_PCM_RX_DELAY_FRAMES;
		source_bits(bits, mapping.data_bits_per_frame, original);
		if (original == V92_B1U_FRAMES - 1u) bits[0] = 0u;
		/* Only the initial inverted acquisition window may contain errors. */
		if (polarity > 0 || original >= V92_B1U_ACQUISITION_FRAMES)
			CHECK(!decision.ambiguous && !memcmp(decoded, bits, mapping.data_bits_per_frame),
				"polarity payload: law=%u rate=%u trellis=%u balanced=%u polarity=%d frame=%u",
				law, rate, trellis, balanced, polarity, original);
		delivered += original >= V92_B1U_FRAMES;
	}
	CHECK(delivered == PAYLOAD_FRAMES, "polarity decoder shortened payload");
	return true;
}

static bool test_polarity_acquisition_boundary(void)
{
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx tx;
	struct v92_pcm_frame_rx fresh;
	struct v92_pcm_frame frame;
	uint8_t bits[V92_PCM_MAPPING_MAX_BITS];
	int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS];

	CHECK(polarity_mapping(&mapping, G711_LAW_PCMU, 2u, V92_CPD_TRELLIS_16_STATE, true) &&
		v92_pcm_frame_tx_init(&tx, &mapping) == V92_PCM_FRAME_OK &&
		v92_pcm_frame_rx_init_constellation(&fresh, &mapping) == V92_PCM_FRAME_OK,
		"polarity boundary initialization failed");
	memset(bits, 1, mapping.data_bits_per_frame);
	CHECK(v92_pcm_frame_encode(&tx, bits, mapping.data_bits_per_frame, &frame) ==
		V92_PCM_FRAME_OK, "polarity boundary encoding failed");
	for (unsigned i = 0u; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i)
		samples[i] = (int16_t)-frame.u[i];
	for (unsigned scenario = 0u; scenario < 5u; ++scenario) {
		struct v92_pcm_frame_rx rx = fresh;
		struct v92_pcm_frame_decision decision, saved;
		memset(&decision, 0xa5, sizeof(decision)); saved = decision;
		memset(bits, 0xa5, sizeof(bits));
		if (scenario == 2u) rx.noise.frames = 1u;
		if (scenario == 3u) rx.pending_frames = 1u;
		struct v92_pcm_frame_rx original = rx;
		enum v92_pcm_frame_result result = scenario == 4u ?
			v92_pcm_frame_acquire_model(&rx, samples, bits, mapping.data_bits_per_frame, &decision) :
			v92_pcm_frame_decode(&rx, samples, scenario != 1u,
				bits, mapping.data_bits_per_frame, &decision);
		if (scenario == 0u)
			CHECK(result == V92_PCM_FRAME_OK && !decision.bits_ready && decision.squared_error == 0u,
				"initial polarity not acquired");
		else if (scenario == 4u)
			CHECK(result == V92_PCM_FRAME_OK && !decision.bits_ready && decision.squared_error > 0u,
				"known history lost its constraint");
		else
			CHECK(result == V92_PCM_FRAME_NO_PATH && !memcmp(&rx, &original, sizeof(rx)) &&
				!memcmp(&decision, &saved, sizeof(decision)),
				"polarity retry escaped acquisition boundary %u", scenario);
		for (size_t i = 0u; i < sizeof(bits); ++i)
			CHECK(bits[i] == 0xa5, "buffered or rejected polarity decode wrote bits");
	}
	return true;
}

static bool test_b1u_validation(enum g711_law law)
{
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx tx;
	struct v92_pcm_frame_rx rx;
	unsigned bad_bits = 0u;

	CHECK(mapping_init(&mapping, law, 19u, false) &&
	      v92_pcm_frame_tx_init(&tx, &mapping) == V92_PCM_FRAME_OK &&
	      v92_pcm_frame_rx_init_constellation(&rx, &mapping) == V92_PCM_FRAME_OK,
	      "B1u validation fixture failed");
	for (unsigned frame = 0u; frame < V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES; ++frame) {
		uint8_t bits[V92_PCM_MAPPING_MAX_BITS], decoded[V92_PCM_MAPPING_MAX_BITS];
		struct v92_pcm_frame encoded;
		struct v92_pcm_frame_decision decision;

		memset(bits, 1, mapping.data_bits_per_frame);
		/* A valid coded waveform carrying one wrong training bit has zero
		 * residual. It must not be silently replaced by the expected one. */
		if (frame == V92_B1U_FRAMES - 1u)
			bits[0] = 0u;
		CHECK(v92_pcm_frame_encode(&tx, bits, mapping.data_bits_per_frame, &encoded) == V92_PCM_FRAME_OK &&
		      v92_pcm_frame_decode(&rx, encoded.u, frame < V92_B1U_FRAMES, decoded,
			      mapping.data_bits_per_frame, &decision) == V92_PCM_FRAME_OK &&
		      decision.squared_error == 0u && !decision.ambiguous,
		      "B1u validation damaged a valid waveform at frame %u", frame);
		if (!decision.bits_ready)
			continue;
		for (size_t bit = 0u; bit < mapping.data_bits_per_frame; ++bit) {
			bool wrong = frame == V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES - 1u && bit == 0u;

			CHECK(decoded[bit] == (wrong ? 0u : 1u), "B1u hid or invented a training error");
			bad_bits += decoded[bit] != 1u;
		}
	}
	CHECK(bad_bits == 1u, "B1u did not expose its delayed final-bit error");
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
	struct v92_pcm_level_fit fit = { 0 };
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

			CHECK(v92_pcm_frame_rx_refine_level(&receiver, &fit, &decision.frame,
				      samples) == V92_PCM_FRAME_OK &&
			      receiver.minimum_level >= fit.minimum && receiver.maximum_level <= fit.maximum &&
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

static bool test_rm_receive(enum g711_law law, uint8_t rate,
	enum v92_cpd_trellis trellis, unsigned scenario)
{
	enum { START = 64, PRIME = START + 32, CONTROL = PRIME + 2, END = CONTROL + 14,
		FRAMES = END + V92_PCM_RX_DELAY_FRAMES };
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx tx;
	struct v92_pcm_frame_rx rx;
	uint8_t payload[FRAMES][V92_PCM_MAPPING_MAX_BITS] = { { 0 } };
	unsigned delivered_tail = 0u, delivered_control = 0u;

	CHECK(mapping_init(&mapping, law, rate, trellis == V92_CPD_TRELLIS_64_STATE),
		"RM mapping failed");
	mapping.cpd.trellis = trellis;
	CHECK(v92_pcm_frame_tx_init(&tx, &mapping) == V92_PCM_FRAME_OK &&
		v92_pcm_frame_rx_init_constellation(&rx, &mapping) == V92_PCM_FRAME_OK,
		"RM receiver initialization failed");
	for (unsigned f = 0u; f < FRAMES; ++f) {
		bool request = scenario != 1u && f >= START && f < CONTROL &&
			!(scenario == 2u && f == START + 16u);
		struct v92_pcm_frame frame;
		struct v92_pcm_frame_decision decision;
		uint8_t decoded[V92_PCM_MAPPING_MAX_BITS];

		source_bits(payload[f], mapping.data_bits_per_frame, f);
		if (scenario == 1u && f == START) {
			/* A single genuine payload word can equal RM. Construct it
			 * through inverse mapping; it must not alter coding/routing. */
			uint8_t digits[12], differential = tx.state.differential;
			struct v34_scrambler descrambler = tx.state.scrambler;

			for (unsigned i = 0u; i < 12u; ++i)
				digits[i] = i % 4u < 2u ? (uint8_t)(mapping.cpd.modulus[i] - 1u) : 0u;
			CHECK(v92_pcm_mapping_decode(&mapping, digits, &differential, decoded,
				mapping.data_bits_per_frame) == V92_PCM_MAPPING_OK, "RM collision has no payload");
			for (unsigned i = 0u; i < mapping.data_bits_per_frame; ++i)
				CHECK(v34_descramble_group(&descrambler, decoded[i], 1u, &payload[f][i]) ==
					V34_FRAMING_OK, "collision descramble failed");
		}
		if (f == CONTROL) {
			struct v92_pcm_frame_rx expected = rx;
			for (unsigned p = 0u; p < expected.path_count; ++p) {
				expected.paths[p].descrambler.history = 0u;
				expected.paths[p].differential = 0u;
				expected.paths[p].rm_frames = 0u;
				expected.paths[p].rm_prime_frames = 0u;
			}
			CHECK(v92_pcm_frame_tx_reset_control(&tx) == V92_PCM_FRAME_OK &&
				v92_pcm_frame_rx_reset_control(&rx) == V92_PCM_FRAME_OK &&
				memcmp(&rx, &expected, sizeof(rx)) == 0,
				"control reset lost delayed tags, trellis, filters or payload");
		}
		CHECK((request ? v92_pcm_frame_encode_rm(&tx.mapping, &tx.state, f >= PRIME, &frame) :
			v92_pcm_frame_encode(&tx, payload[f], mapping.data_bits_per_frame, &frame)) ==
			V92_PCM_FRAME_OK, "RM frame generation failed");
		memset(decoded, 0xa5, sizeof(decoded));
		CHECK(v92_pcm_frame_decode(&rx, frame.u, f < V92_B1U_FRAMES, decoded,
			mapping.data_bits_per_frame, &decision) == V92_PCM_FRAME_OK &&
			v92_pcm_frame_rx_is_valid(&rx) && !decision.squared_error &&
			memcmp(frame.digit, decision.frame.digit, sizeof(frame.digit)) == 0,
			"RM coding failed law %u rate %u trellis %u scenario %u frame %u",
			law, rate, trellis, scenario, f);
		enum v92_pcm_frame_rm expected = V92_PCM_FRAME_RM_NONE;
		if (scenario == 0u && f >= START + V92_PCM_RM_QUALIFY_FRAMES - 1u && f < CONTROL)
			expected = f < PRIME ? V92_PCM_FRAME_RM_SIGNAL :
				f == PRIME ? V92_PCM_FRAME_RM_PRIME : V92_PCM_FRAME_RM_COMPLETE;
		CHECK(decision.rm == expected, "RM qualification differs at frame %u: %u/%u",
			f, decision.rm, expected);
		CHECK(rx.pending_frames == (f < V92_PCM_RX_DELAY_FRAMES ? f + 1u :
			V92_PCM_RX_DELAY_FRAMES), "suppression changed the lookahead depth");
		if (scenario == 0u && f >= START + V92_PCM_RM_QUALIFY_FRAMES - 1u &&
			f < CONTROL + V92_PCM_RX_DELAY_FRAMES)
			CHECK(!decision.bits_ready, "qualified/delayed RM leaked payload at %u", f);
		if (!decision.bits_ready) {
			for (unsigned i = 0u; i < sizeof(decoded); ++i)
				CHECK(decoded[i] == 0xa5, "non-payload slot overwrote caller bits");
		}
		if (f < V92_PCM_RX_DELAY_FRAMES) continue;
		unsigned original = f - V92_PCM_RX_DELAY_FRAMES;
		if (scenario == 1u || original < START || original >= CONTROL) {
			CHECK(decision.bits_ready && !decision.ambiguous &&
				memcmp(decoded, payload[original], mapping.data_bits_per_frame) == 0,
				"RM changed real payload/control at %u", original);
			delivered_tail += original >= V92_B1U_FRAMES && original < START;
			delivered_control += original >= CONTROL;
		}
	}
	CHECK(delivered_tail == START - V92_B1U_FRAMES && delivered_control == END - CONTROL,
		"RM lost the preceding payload tail or following control");
	return true;
}

static enum v92_pcm_frame_result decode_confidence(
	struct v92_pcm_frame_rx *receiver, const int16_t samples[12],
	const struct v92_pcm_frame_cell *cells, uint8_t *bits,
	struct v92_pcm_frame_decision *decision)
{
	if (!cells)
		return v92_pcm_frame_decode(receiver, samples, false, bits,
			receiver->mapping.data_bits_per_frame, decision);
	struct v92_pcm_frame_channel channel = { .cells = cells };
	struct v92_pcm_frame_likelihood likelihood;
	return v92_pcm_frame_decode_observed(receiver, samples, false, bits,
		receiver->mapping.data_bits_per_frame, decision, &channel, &likelihood);
}

static bool test_input_confidence(bool conditioned)
{
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx transmitter;
	struct v92_pcm_frame frame;
	uint8_t ones[V92_PCM_MAPPING_MAX_BITS];
	int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS];
	struct v92_pcm_frame_cell cells[V92_PCM_MAPPING_FRAME_SYMBOLS];
	uint64_t cost = 0;

	CHECK(mapping_init(&mapping, G711_LAW_PCMU, 1u, false) &&
		v92_pcm_frame_tx_init(&transmitter, &mapping) == V92_PCM_FRAME_OK,
		"confidence fixture initialization failed");
	memset(ones, 1, mapping.data_bits_per_frame);
	CHECK(v92_pcm_frame_encode(&transmitter, ones, mapping.data_bits_per_frame, &frame) ==
		V92_PCM_FRAME_OK, "confidence fixture encoding failed");
	for (unsigned i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i) {
		int16_t source = frame.u[i];
		struct g711_interval cell;

		if (!conditioned)
			CHECK(v92_pcm_frame_scale(mapping.cpd.four_times_gain_q0_16, UPSTREAM_LEVEL, frame.v[i], &source) ==
				V92_PCM_FRAME_OK, "confidence sample failed");
		samples[i] = quantize(mapping.law, (int16_t)(source + 128));
		CHECK(g711_quantization_interval(mapping.law, samples[i], &cell), "invalid cell");
		int64_t error = source < cell.lower ? cell.lower - source :
			source > cell.upper ? source - cell.upper : 0;
		cells[i] = (struct v92_pcm_frame_cell){ cell.lower, cell.upper };
		cost += (uint64_t)(error * error) * (conditioned ? UINT64_C(65536) : 1u);
	}
	uint64_t resolution = cost / (V92_PCM_MAPPING_FRAME_SYMBOLS / 3u);
	const uint64_t gaps[] = { 1u, 0u, resolution, resolution + 1u };

	CHECK(resolution > 1u, "confidence fixture has no residual");
	for (unsigned pending = 0; pending < V92_PCM_RX_DELAY_FRAMES;
	     pending += V92_PCM_RX_DELAY_FRAMES - 1u)
		for (unsigned g = 0; g < ARRAY_LENGTH(gaps); ++g) {
			struct v92_pcm_frame_rx receiver;
			struct v92_pcm_frame_decision decision;
			uint8_t decoded[V92_PCM_MAPPING_MAX_BITS];

			enum v92_pcm_frame_result initialized = conditioned ?
				v92_pcm_frame_rx_init_constellation(&receiver, &mapping) :
				v92_pcm_frame_rx_init(&receiver, &mapping, UPSTREAM_LEVEL);
			CHECK(initialized == V92_PCM_FRAME_OK, "confidence receiver failed");
			if (!conditioned)
				receiver.minimum_level = receiver.maximum_level = UPSTREAM_LEVEL;
			receiver.pending_frames = V92_PCM_RX_DELAY_FRAMES;
			receiver.path_count = 2u;
			receiver.paths[1] = receiver.paths[0];
			receiver.paths[1].metric = gaps[g];
			receiver.paths[1].pending[pending][0] = 1u;
			CHECK(decode_confidence(&receiver, samples, conditioned ? cells : NULL, decoded,
				&decision) == V92_PCM_FRAME_OK &&
				v92_pcm_frame_rx_is_valid(&receiver) && decision.bits_ready &&
				memcmp(decision.frame.digit, frame.digit, sizeof(frame.digit)) == 0,
				"confidence changed symbol decisions");
			CHECK(decision.uncertain == (gaps[g] <= resolution) &&
				decision.ambiguous == (gaps[g] == 0u && pending == 0u),
				"gap %llu at pending %u conflated confidence and ambiguity",
				(unsigned long long)gaps[g], pending);
			/* Exact ties may select either disputed bit; every undisputed bit stays intact. */
			for (unsigned bit = 0; bit < mapping.data_bits_per_frame; ++bit)
				CHECK(decoded[bit] == 0 || (gaps[g] == 0u && pending == 0u && bit == 0u),
					"confidence changed payload: cell %u gap %llu pending %u bit %u",
					conditioned, (unsigned long long)gaps[g], pending, bit);
			if (gaps[g]) {
				struct v92_pcm_frame_tx next = transmitter;
				int16_t clean[V92_PCM_MAPPING_FRAME_SYMBOLS];
				struct v92_pcm_frame_cell clean_cells[V92_PCM_MAPPING_FRAME_SYMBOLS];

				if (conditioned) {
					struct v92_pcm_frame encoded;
					CHECK(v92_pcm_frame_encode(&next, ones, mapping.data_bits_per_frame,
						&encoded) == V92_PCM_FRAME_OK, "clean cell frame failed");
					for (unsigned i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i) {
						clean[i] = encoded.u[i];
						clean_cells[i] = (struct v92_pcm_frame_cell){ clean[i], clean[i] };
					}
				} else {
					CHECK(waveform(&next, ones, clean, UPSTREAM_LEVEL), "clean waveform failed");
				}
				CHECK(decode_confidence(&receiver, clean, conditioned ? clean_cells : NULL, decoded,
					&decision) == V92_PCM_FRAME_OK &&
					!decision.uncertain && !decision.ambiguous,
					"weak evidence persisted as an erasure on a later clean frame");
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
	receiver.paths[0].nondata_frames = 1u;
	CHECK(!v92_pcm_frame_rx_is_valid(&receiver), "unowned non-payload tag accepted");
	receiver = saved;
	receiver.paths[0].rm_frames = V92_PCM_RM_QUALIFY_FRAMES + 1u;
	CHECK(!v92_pcm_frame_rx_is_valid(&receiver), "unbounded RM run accepted");
	receiver = saved;
	receiver.paths[0].rm_prime_frames = 1u;
	CHECK(!v92_pcm_frame_rx_is_valid(&receiver), "prime without qualified RM accepted");
	receiver.paths[0].rm_frames = V92_PCM_RM_QUALIFY_FRAMES;
	receiver.paths[0].rm_prime_frames = 3u;
	CHECK(!v92_pcm_frame_rx_is_valid(&receiver), "overlong prime accepted");
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
	struct v92_pcm_level_fit fit = { 0 };

	invalid_mapping.data_bits_per_frame = 0u;
	CHECK(v92_pcm_frame_rx_init_constellation(NULL, &mapping) == V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_rx_init_constellation(&receiver, NULL) == V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_rx_init_constellation(&receiver, &invalid_mapping) ==
		      V92_PCM_FRAME_INVALID_PARAMETERS &&
	      v92_pcm_frame_rx_refine_level(&receiver, &fit, &decision.frame, samples) ==
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
	receiver.post_b1_frames = 1u;
	CHECK(!v92_pcm_frame_rx_is_valid(&receiver), "post-B1u counter exceeded traceback history");
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

static unsigned failed_prediction, prediction_count;
double __real_v92_observation_predict(const struct v92_observation *, const int32_t *, double);
double __wrap_v92_observation_predict(const struct v92_observation *model, const int32_t *x, double offset)
{
	if (++prediction_count == failed_prediction) return NAN;
	return __real_v92_observation_predict(model, x, offset);
}

static void channel_samples(enum g711_law law, unsigned taps, const struct v92_pcm_frame *frame,
	int16_t memory[3], int16_t levels[12], int16_t samples[12])
{
	for (unsigned i = 0; i < 12; i++) {
		int16_t q = quantize(law, (int16_t)round(.5 * frame->x[i]));
		levels[i] = quantize(law, (int16_t)round(1.8 * q + .16 * memory[0] +
			.04 * memory[1] + (taps == 4 ? .03 * memory[2] : 0.)));
		samples[i] = (int16_t)round(.75 * levels[i]);
		memory[2] = memory[1]; memory[1] = memory[0]; memory[0] = q;
	}
}

static bool observed_rejection(struct v92_pcm_frame_rx *rx,
	struct v92_observation *model, const int16_t samples[12], const int16_t levels[12])
{
	struct v92_pcm_frame_rx initial = *rx;
	struct v92_pcm_frame_decision decision, saved_decision;
	struct v92_pcm_frame_likelihood likelihood, saved_likelihood;
	uint8_t bits[V92_PCM_MAPPING_MAX_BITS], saved_bits[sizeof(bits)];
	memset(&decision, 0xa5, sizeof(decision)); saved_decision = decision;
	memset(&likelihood, 0xa5, sizeof(likelihood)); saved_likelihood = likelihood;
	memset(bits, 0xa5, sizeof(bits)); memcpy(saved_bits, bits, sizeof(bits));
	struct v92_pcm_frame_channel channel = { .model = model, .levels = levels };
	const unsigned failures[] = { 1, 6, 12, 0 };
	for (unsigned i = 0; i < ARRAY_LENGTH(failures); i++) {
		double gain = model->quantizer_gain;
		if (i == 3) model->quantizer_gain = 0.;
		failed_prediction = failures[i]; prediction_count = 0;
		enum v92_pcm_frame_result rc = v92_pcm_frame_decode_observed(rx, samples, false,
			bits, rx->mapping.data_bits_per_frame, &decision, &channel, &likelihood);
		model->quantizer_gain = gain; failed_prediction = 0;
		/* Zero host gain is now an invalid final model, not an intermediate
		 * point-observation model that can fail later during prediction. */
		CHECK(rc == (i == 3 ? V92_PCM_FRAME_INVALID_PARAMETERS : V92_PCM_FRAME_NO_PATH) &&
			!memcmp(rx, &initial, sizeof(initial)) &&
			!memcmp(bits, saved_bits, sizeof(bits)) &&
			!memcmp(&decision, &saved_decision, sizeof(decision)) &&
			!memcmp(&likelihood, &saved_likelihood, sizeof(likelihood)),
			"prediction failure %u changed output or receiver (result %d)", failures[i], rc);
	}
	int16_t invalid_levels[12]; memcpy(invalid_levels, levels, sizeof(invalid_levels));
	invalid_levels[11] = 1;
	channel.levels = invalid_levels;
	CHECK(v92_pcm_frame_decode_observed(rx, samples, false, bits,
		rx->mapping.data_bits_per_frame, &decision, &channel, &likelihood) ==
		V92_PCM_FRAME_INVALID_PARAMETERS, "invalid final wire level accepted");
	channel.levels = levels;
	double offsets[12] = { 0 };
	for (unsigned fault = 0; fault < 5; fault++) {
		channel.offsets = offsets;
		channel.levels = fault == 3 ? NULL : levels;
		offsets[11] = fault == 0 ? NAN : fault == 1 ? INFINITY : fault == 2 ? -INFINITY : 0.;
		if (fault == 4) channel.offsets = (const double *)&likelihood;
		CHECK(v92_pcm_frame_decode_observed(rx, samples, false, bits,
			rx->mapping.data_bits_per_frame, &decision, &channel, &likelihood) ==
			(fault == 4 ? V92_PCM_FRAME_INVALID_ARGUMENT : V92_PCM_FRAME_INVALID_PARAMETERS),
			"invalid or aliased host correction accepted");
		CHECK(!memcmp(rx, &initial, sizeof(initial)) && !memcmp(bits, saved_bits, sizeof(bits)) &&
			!memcmp(&decision, &saved_decision, sizeof(decision)) &&
			!memcmp(&likelihood, &saved_likelihood, sizeof(likelihood)),
			"invalid host correction changed outputs or receiver");
	}
	channel.offsets = NULL;
	channel.levels = levels;
	CHECK(v92_pcm_frame_decode_observed(rx, samples, false, bits,
		rx->mapping.data_bits_per_frame, &decision, &channel,
		(struct v92_pcm_frame_likelihood *)&decision) == V92_PCM_FRAME_INVALID_ARGUMENT,
		"overlapping outputs accepted");
	CHECK(v92_pcm_frame_decode_observed(rx, samples, false, (uint8_t *)rx,
		rx->mapping.data_bits_per_frame, &decision, &channel, &likelihood) ==
		V92_PCM_FRAME_INVALID_ARGUMENT, "overlapping state and bits accepted");
	CHECK(v92_pcm_frame_decode_observed(rx, samples, false, bits,
		rx->mapping.data_bits_per_frame, &decision, NULL, &likelihood) ==
		V92_PCM_FRAME_INVALID_ARGUMENT, "missing channel accepted");
	CHECK(v92_pcm_frame_decode_observed(rx, samples, false, bits,
		rx->mapping.data_bits_per_frame, &decision, &channel, NULL) ==
		V92_PCM_FRAME_INVALID_ARGUMENT, "missing likelihood accepted");
	channel.model = NULL;
	CHECK(v92_pcm_frame_decode_observed(rx, samples, false, bits,
		rx->mapping.data_bits_per_frame, &decision, &channel, &likelihood) ==
		V92_PCM_FRAME_INVALID_ARGUMENT, "missing model accepted");
	channel.model = model;
	enum g711_law law = model->law;
	model->law = law == G711_LAW_PCMU ? G711_LAW_PCMA : G711_LAW_PCMU;
	CHECK(v92_pcm_frame_decode_observed(rx, samples, false, bits,
		rx->mapping.data_bits_per_frame, &decision, &channel, &likelihood) ==
		V92_PCM_FRAME_INVALID_PARAMETERS, "wrong model law accepted");
	model->law = law;
	CHECK(v92_pcm_frame_decode_observed(rx, samples, false, bits,
		rx->mapping.data_bits_per_frame - 1, &decision, &channel, &likelihood) ==
		V92_PCM_FRAME_OUT_OF_RANGE, "wrong bit count accepted");
	CHECK(!memcmp(rx, &initial, sizeof(initial)) && !memcmp(bits, saved_bits, sizeof(bits)) &&
		!memcmp(&decision, &saved_decision, sizeof(decision)) &&
		!memcmp(&likelihood, &saved_likelihood, sizeof(likelihood)),
		"rejected observation changed outputs or receiver");
	return true;
}

static bool observed_merged_payload(const struct v92_pcm_frame_rx *before,
	const struct v92_observation *model, const int16_t samples[12], const int16_t levels[12])
{
	/* Future-equivalent histories cannot obtain more evidence about their
	 * old payload. Cover the roughly 27:1 disagreement from a noisy 48k
	 * call, as well as both sides of the receiver's confidence boundary. */
	const uint64_t gaps[] = { 0u, 6700000u,
		V92_PCM_RX_AMBIGUITY_COST, V92_PCM_RX_AMBIGUITY_COST + 1u };
	for (unsigned i = 0; i < ARRAY_LENGTH(gaps); ++i) {
		struct v92_pcm_frame_rx rx = *before;
		struct v92_pcm_frame_decision decision;
		struct v92_pcm_frame_likelihood likelihood;
		struct v92_pcm_frame_channel channel = { .model = model, .levels = levels };
		uint8_t decoded[V92_PCM_MAPPING_MAX_BITS];
		unsigned best = 0;
		for (unsigned p = 1; p < rx.path_count; ++p)
			if (rx.paths[p].metric < rx.paths[best].metric) best = p;
		rx.paths[0] = rx.paths[best];
		rx.paths[0].metric = 0;
		rx.paths[1] = rx.paths[0];
		rx.paths[1].metric = gaps[i];
		rx.paths[1].pending[0][0] ^= 1u;
		rx.paths[1].pending[V92_PCM_RX_DELAY_FRAMES - 1u][1] ^= 1u;
		rx.path_count = 2;
		bool disputed = i + 1u < ARRAY_LENGTH(gaps);
		CHECK(v92_pcm_frame_decode_observed(&rx, samples, false, decoded,
			rx.mapping.data_bits_per_frame, &decision, &channel, &likelihood) == V92_PCM_FRAME_OK &&
			decision.bits_ready && decision.ambiguous == disputed,
			"merged payload lost confidence at gap %llu", (unsigned long long)gaps[i]);
		best = 0;
		for (unsigned p = 1; p < rx.path_count; ++p)
			if (rx.paths[p].metric < rx.paths[best].metric) best = p;
		CHECK(((rx.paths[best].ambiguous_frames >> (V92_PCM_RX_DELAY_FRAMES - 2u)) & 1u) == disputed,
			"merged history lost its delayed payload dispute");
	}
	return true;
}

static bool test_observed_channel(enum g711_law law, unsigned taps, bool reorder)
{
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	CHECK(v92_cpd_direct_init(&cpd, law, 1, false) == V92_CPD_OK, "CPd failed");
	cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD].count = taps;
	CHECK(v92_pcm_mapping_init(&mapping, law, &cpd) == V92_PCM_MAPPING_OK, "mapping failed");
	struct v92_pcm_frame_tx tx;
	struct v92_pcm_frame_rx rx;
	CHECK(v92_pcm_frame_tx_init(&tx, &mapping) == V92_PCM_FRAME_OK &&
		v92_pcm_frame_rx_init_constellation(&rx, &mapping) == V92_PCM_FRAME_OK, "init failed");
	int32_t x[V92_OBSERVATION_TRAINING];
	int16_t u[V92_OBSERVATION_TRAINING], y[V92_OBSERVATION_TRAINING];
	int16_t wire[V92_OBSERVATION_TRAINING], memory[3] = { 0 };
	uint8_t bits[V92_PCM_MAPPING_MAX_BITS], decoded[V92_PCM_MAPPING_MAX_BITS];
	memset(bits, 1, mapping.data_bits_per_frame);
	for (unsigned f = 0; f < V92_B1U_ACQUISITION_FRAMES; f++) {
		struct v92_pcm_frame frame;
		CHECK(v92_pcm_frame_encode(&tx, bits, mapping.data_bits_per_frame, &frame) ==
			V92_PCM_FRAME_OK, "training encode failed");
		memcpy(x + 12 * f, frame.x, sizeof(frame.x));
		memcpy(u + 12 * f, frame.u, sizeof(frame.u));
		channel_samples(law, taps, &frame, memory, wire + 12 * f, y + 12 * f);
	}
	struct v92_observation model;
	const struct v92_observation_training training = {
		.input = x, .linear = u, .sample = y, .levels = wire,
		.count = ARRAY_LENGTH(x), .taps = taps, .host_gain = .75, .law = law,
	};
	CHECK(v92_observation_fit(&model, &training), "fit failed");
	struct v92_observation frozen = model;
	CHECK(v92_pcm_frame_tx_reset_b1u(&tx) == V92_PCM_FRAME_OK, "reset failed");
	memset(memory, 0, sizeof(memory));
	for (unsigned f = 0; f < V92_B1U_FRAMES + 100 + V92_PCM_RX_DELAY_FRAMES; f++) {
		struct v92_pcm_frame frame;
		struct v92_pcm_frame_decision decision;
		struct v92_pcm_frame_likelihood likelihood;
		int16_t levels[12], samples[12];
		bool received[12]; memset(received, true, sizeof(received));
		source_bits(bits, mapping.data_bits_per_frame, f);
		CHECK(v92_pcm_frame_encode(&tx, bits, mapping.data_bits_per_frame, &frame) ==
			V92_PCM_FRAME_OK, "payload encode failed");
		channel_samples(law, taps, &frame, memory, levels, samples);
		if (f == 60 && (!observed_rejection(&rx, &model, samples, levels) ||
			!observed_merged_payload(&rx, &model, samples, levels))) return false;
		struct v92_pcm_frame_channel channel = { .model = &model, .levels = levels, .received = received };
		if (f == 50) { channel.levels = NULL; channel.received = NULL; }
		/* Histories are not sorted by likelihood. Rotate their storage at
		 * every frame boundary so a visit-order optimization cannot
		 * depend on the first parent's identity to retain the correct word.
		 * This also covers known B1u, the erased symbol and delayed payload. */
		if (reorder && rx.path_count > 1u) {
			struct v92_pcm_frame_rx_path first = rx.paths[0];

			memmove(rx.paths, rx.paths + 1, (rx.path_count - 1u) * sizeof(first));
			rx.paths[rx.path_count - 1u] = first;
		}
		struct v92_pcm_frame_rx erased_rx = rx;
		if (f == 72) { received[5] = false; levels[5] = 1; samples[5] = INT16_MAX; }
		memset(decoded, 0xa5, sizeof(decoded));
		CHECK(v92_pcm_frame_decode_observed(&rx, samples, f < V92_B1U_FRAMES, decoded,
			mapping.data_bits_per_frame, &decision, &channel, &likelihood) == V92_PCM_FRAME_OK,
			"observed law %u frame %u failed", law, f);
		if (f == 72) {
			struct v92_pcm_frame_decision other;
			struct v92_pcm_frame_likelihood other_likelihood;
			uint8_t other_bits[V92_PCM_MAPPING_MAX_BITS];
			levels[5] = 3; samples[5] = INT16_MIN;
			CHECK(v92_pcm_frame_decode_observed(&erased_rx, samples, false, other_bits,
				mapping.data_bits_per_frame, &other, &channel, &other_likelihood) == V92_PCM_FRAME_OK &&
				!memcmp(&erased_rx, &rx, sizeof(rx)) &&
				!memcmp(other_bits, decoded, mapping.data_bits_per_frame) &&
				!memcmp(&likelihood, &other_likelihood, sizeof(likelihood)),
				"erased symbol influenced likelihood or state");
			/* Erased samples leave likelihood unchanged, but remain in
			 * the raw reconstruction residual. */
			uint64_t error = 0, other_error = 0;

			for (unsigned i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i) {
				int64_t delta = (int64_t)(i == 5 ? INT16_MAX : samples[i]) -
					decision.frame.u[i];
				int64_t other_delta = (int64_t)samples[i] - other.frame.u[i];

				error += (uint64_t)(delta * delta);
				other_error += (uint64_t)(other_delta * other_delta);
			}
			CHECK(decision.squared_error == error && other.squared_error == other_error,
				"erased symbol was omitted from the raw residual");
		}
		CHECK(decision.bits_ready == (f >= V92_PCM_RX_DELAY_FRAMES), "traceback delay changed");
		if (decision.bits_ready) {
			source_bits(bits, mapping.data_bits_per_frame, f - V92_PCM_RX_DELAY_FRAMES);
			CHECK(!decision.ambiguous && !memcmp(bits, decoded, mapping.data_bits_per_frame),
				"law %u frame %u changed payload", law, f);
		}
		for (size_t i = decision.bits_ready ? mapping.data_bits_per_frame : 0; i < sizeof(decoded); i++)
			CHECK(decoded[i] == 0xa5, "wrote outside delivered bits");
		for (unsigned i = 0; i < 12; i++)
			CHECK(isfinite(likelihood.predicted_samples[i]), "non-finite prediction");
	}
	CHECK(!memcmp(&model, &frozen, sizeof(model)), "payload changed frozen model");
	return true;
}

static bool test_constellation_cells(enum g711_law law, double gain)
{
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx tx;
	struct v92_pcm_frame_rx rx;
	uint64_t residual = 0;

	CHECK(mapping_init(&mapping, law, 4u, true) &&
	      v92_pcm_frame_tx_init(&tx, &mapping) == V92_PCM_FRAME_OK &&
	      v92_pcm_frame_rx_init_constellation(&rx, &mapping) == V92_PCM_FRAME_OK,
	      "cell fixture initialization failed");
	for (unsigned f = 0; f < V92_B1U_FRAMES + 128u; ++f) {
		struct v92_pcm_frame encoded;
		struct v92_pcm_frame_decision decision;
		struct v92_pcm_frame_likelihood likelihood;
		struct v92_pcm_frame_cell cells[V92_PCM_MAPPING_FRAME_SYMBOLS];
		struct v92_pcm_frame_channel channel = { .cells = cells };
		uint8_t bits[V92_PCM_MAPPING_MAX_BITS], decoded[V92_PCM_MAPPING_MAX_BITS];
		int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS];
		double expected_cost = 0;
		uint64_t expected_error = 0;

		source_bits(bits, mapping.data_bits_per_frame, f);
		CHECK(v92_pcm_frame_encode(&tx, bits, mapping.data_bits_per_frame, &encoded) ==
		      V92_PCM_FRAME_OK, "cell fixture encoding failed");
		for (unsigned i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i) {
			struct g711_interval cell;
			/* The codec precedes gain and an independently known host
			 * correction. Never re-encode this conditioned observation. */
			int16_t wire = quantize(law, (int16_t)lround(encoded.u[i] / gain));
			double offset = i % 2u ? 13.0 : -7.0;
			CHECK(g711_quantization_interval(law, wire, &cell), "wire cell missing");
			samples[i] = (int16_t)lround(wire * gain + offset);
			cells[i] = (struct v92_pcm_frame_cell){
				.lower = cell.lower * gain + offset,
				.upper = cell.upper * gain + offset,
			};
			int64_t error = samples[i] - encoded.u[i];
			double distance = fmax(cells[i].lower - encoded.u[i],
				fmax(encoded.u[i] - cells[i].upper, 0.0));
			uint64_t weight = rx.post_b1_frames == V92_PCM_RX_DELAY_FRAMES ?
				UINT64_C(65536) : (uint64_t)(65536.0 /
				(1.0 + rx.noise.relative_variance * samples[i] * samples[i]));

			expected_error += (uint64_t)(error * error);
			expected_cost += (uint64_t)(distance * distance * (weight ? weight : 1u));
		}
		if (f == 24u) {
			struct v92_pcm_frame_rx saved = rx;
			struct v92_pcm_frame_decision untouched;
			struct v92_pcm_frame_likelihood unchanged;
			int16_t incompatible_levels[V92_PCM_MAPPING_FRAME_SYMBOLS] = { 0 };
			memset(&decision, 0xa5, sizeof(decision)); untouched = decision;
			memset(&likelihood, 0xa5, sizeof(likelihood)); unchanged = likelihood;
			memset(decoded, 0xa5, sizeof(decoded));
			double upper = cells[11].upper;
			cells[11].upper = NAN;
			CHECK(v92_pcm_frame_decode_observed(&rx, samples, true, decoded,
			      mapping.data_bits_per_frame, &decision, &channel, &likelihood) ==
			      V92_PCM_FRAME_INVALID_PARAMETERS && !memcmp(&rx, &saved, sizeof(rx)) &&
			      !memcmp(&decision, &untouched, sizeof(decision)) &&
			      !memcmp(&likelihood, &unchanged, sizeof(likelihood)) && decoded[0] == 0xa5,
			      "invalid final cell was not transactional");
			cells[11].upper = cells[11].lower - 1.0;
			CHECK(v92_pcm_frame_decode_observed(&rx, samples, true, decoded,
			      mapping.data_bits_per_frame, &decision, &channel, &likelihood) ==
			      V92_PCM_FRAME_INVALID_PARAMETERS && !memcmp(&rx, &saved, sizeof(rx)),
			      "reversed cell was accepted");
			cells[11].upper = upper;
			channel.levels = incompatible_levels;
			CHECK(v92_pcm_frame_decode_observed(&rx, samples, true, decoded,
			      mapping.data_bits_per_frame, &decision, &channel, &likelihood) ==
			      V92_PCM_FRAME_INVALID_PARAMETERS && !memcmp(&rx, &saved, sizeof(rx)),
			      "cells accepted a conflicting observation source");
			channel.levels = NULL;
			channel.cells = (const struct v92_pcm_frame_cell *)&rx;
			CHECK(v92_pcm_frame_decode_observed(&rx, samples, true, decoded,
			      mapping.data_bits_per_frame, &decision, &channel, &likelihood) ==
			      V92_PCM_FRAME_INVALID_ARGUMENT && !memcmp(&rx, &saved, sizeof(rx)),
			      "aliased cells changed receiver");
			channel.cells = cells;
		}
		CHECK(v92_pcm_frame_decode_observed(&rx, samples, f < V92_B1U_FRAMES, decoded,
		      mapping.data_bits_per_frame, &decision, &channel, &likelihood) == V92_PCM_FRAME_OK &&
		      v92_pcm_frame_rx_is_valid(&rx), "cell decode failed: law %u frame %u", law, f);
		CHECK(!memcmp(decision.frame.u, encoded.u, sizeof(encoded.u)) &&
		      decision.squared_error == expected_error && likelihood.cost == expected_cost,
		      "cell ranking changed points or hid physical residual: law %u frame %u", law, f);
		for (unsigned i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i)
			CHECK(likelihood.predicted_samples[i] == encoded.u[i], "cell prediction followed sample");
		residual += decision.squared_error;
		if (decision.bits_ready) {
			source_bits(bits, mapping.data_bits_per_frame, f - V92_PCM_RX_DELAY_FRAMES);
			CHECK(!decision.ambiguous && !memcmp(bits, decoded, mapping.data_bits_per_frame),
			      "cell decoder corrupted payload");
		}
	}
	CHECK(residual > 0, "cell fixture hid companding noise");
	return true;
}

static bool test_inactive_filter_history(void)
{
	static const int16_t coefficients[] = { INT16_MAX, INT16_MIN, 8193, 0 };

	for (unsigned law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
	for (unsigned filtered = 0u; filtered < 2u; ++filtered)
	for (unsigned coefficient = 0u; coefficient < ARRAY_LENGTH(coefficients); ++coefficient)
	for (unsigned training = 0u; training < 2u; ++training) {
		struct v92_pcm_mapping mapping;
		struct v92_pcm_frame_tx transmitter;
		struct v92_pcm_frame_rx baseline, duplicate;
		struct v92_pcm_frame_decision decisions[2];
		uint8_t input[V92_PCM_MAPPING_MAX_BITS];
		uint8_t bits[2][V92_PCM_MAPPING_MAX_BITS] = { { 0 } };
		int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS];

		CHECK(mapping_init(&mapping, (enum g711_law)law, 13u, filtered != 0u),
		      "inactive-history mapping failed");
		struct v92_cpd_parameters cpd = mapping.cpd;

		cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD].coefficient[0] = coefficients[coefficient];
		CHECK(v92_pcm_mapping_init(&mapping, (enum g711_law)law, &cpd) == V92_PCM_MAPPING_OK &&
		      v92_pcm_frame_tx_init(&transmitter, &mapping) == V92_PCM_FRAME_OK &&
		      v92_pcm_frame_rx_init(&baseline, &mapping, UPSTREAM_LEVEL) ==
			      V92_PCM_FRAME_OK, "inactive-history fixture failed");
		memset(input, 1, mapping.data_bits_per_frame);
		CHECK(waveform(&transmitter, input, samples, UPSTREAM_LEVEL),
		      "inactive-history waveform failed");
		baseline.path_count = 2u;
		baseline.paths[1] = baseline.paths[0];
		duplicate = baseline;
		duplicate.paths[1].filter.history[V92_CPD_MAX_TOTAL_COEFFICIENTS - 1u] = 1;
		/* A declared tap with a zero coefficient is also irrelevant, even
		 * when its history slot is active and the current coefficient scales,
		 * reverses or suppresses the transmitted constellation. */
		if (!filtered)
			for (unsigned tap = 0; tap < V92_CPD_DIRECT_FILTER_TAPS - 1u; ++tap)
				duplicate.paths[1].filter.history[tap] = (int32_t)tap + 123;
		CHECK(v92_pcm_frame_rx_is_valid(&duplicate) &&
		      v92_pcm_frame_decode(&baseline, samples, training != 0u, bits[0],
			mapping.data_bits_per_frame, &decisions[0]) == V92_PCM_FRAME_OK &&
		      v92_pcm_frame_decode(&duplicate, samples, training != 0u, bits[1],
			mapping.data_bits_per_frame, &decisions[1]) == V92_PCM_FRAME_OK,
		      "inactive-history decode failed");
		CHECK(duplicate.path_count == baseline.path_count,
		      "unused history split paths with filter %u: %u versus %u",
		      filtered, duplicate.path_count, baseline.path_count);
		CHECK(!memcmp(bits[0], bits[1], sizeof(bits[0])) &&
		      decisions[0].squared_error == decisions[1].squared_error &&
		      decisions[0].uncertain == decisions[1].uncertain &&
		      decisions[0].ambiguous == decisions[1].ambiguous &&
		      decisions[0].bits_ready == decisions[1].bits_ready &&
		      decisions[0].rm == decisions[1].rm &&
		      !memcmp(decisions[0].predicted_samples, decisions[1].predicted_samples,
			 sizeof(decisions[0].predicted_samples)),
		      "inactive history changed scoring or delivery");
	}
	return true;
}

static bool test_spare_word_beam(unsigned damaged_frame, unsigned differential)
{
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx tx;
	struct v92_pcm_frame_rx rx;

	CHECK(v92_cpd_direct_init(&cpd, G711_LAW_PCMU, 10u, false) == V92_CPD_OK,
	      "spare-word CPd initialization failed");
	/* 28^9 * 14^3 exceeds the 54-bit payload space. Nearby spare words
	 * must not consume every entry in the bounded symbol-path search. */
	for (unsigned i = 0u; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i)
		cpd.modulus[i] = i % 4u == 3u ? 14u : 28u;
	memset(cpd.constellation, 0, sizeof(cpd.constellation));
	cpd.constellation[0].count = 14u;
	for (unsigned i = 0u; i < 14u; ++i)
		cpd.constellation[0].positive_linear[i] = (int16_t)((2u * i + 1u) * 32u);
	CHECK(v92_pcm_mapping_init(&mapping, G711_LAW_PCMU, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_pcm_frame_tx_init(&tx, &mapping) == V92_PCM_FRAME_OK &&
	      v92_pcm_frame_rx_init_constellation(&rx, &mapping) == V92_PCM_FRAME_OK,
	      "spare-word stream initialization failed");
	for (unsigned f = 0u; f < damaged_frame + 26u; ++f) {
		struct v92_pcm_frame frame;
		struct v92_pcm_frame_decision decision;
		uint8_t bits[V92_PCM_MAPPING_MAX_BITS], decoded[V92_PCM_MAPPING_MAX_BITS];
		int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS];
		unsigned prior = tx.state.differential;

		source_bits(bits, mapping.data_bits_per_frame, f);
		CHECK(v92_pcm_frame_encode(&tx, bits, mapping.data_bits_per_frame, &frame) ==
		      V92_PCM_FRAME_OK, "spare-word waveform failed");
		memcpy(samples, frame.u, sizeof(samples));
		if (f == damaged_frame) {
			CHECK(prior == differential, "fixture missed differential state %u", differential);
			/* Move only the final sample toward an unused modulus word,
			 * retaining the fourth symbol's required parity. */
			int target = prior ? 0 : 13;
			int eta = frame.signed_index[11] + 2 * (target - frame.digit[11]);

			while (eta < -14) eta += 28;
			while (eta >= 14) eta -= 28;
			samples[11] = eta < 0 ? -cpd.constellation[0].positive_linear[-eta - 1] :
				cpd.constellation[0].positive_linear[eta];
		}
		CHECK(v92_pcm_frame_decode(&rx, samples, f < V92_B1U_FRAMES, decoded,
			mapping.data_bits_per_frame, &decision) == V92_PCM_FRAME_OK,
		      "spare words exhausted the beam at frame %u", f);
		/* The damaged frame may contain errors. Require independent known
		 * payload after traceback and descrambler recovery have elapsed. */
		if (f >= damaged_frame + 16u) {
			source_bits(bits, mapping.data_bits_per_frame, f - V92_PCM_RX_DELAY_FRAMES);
			CHECK(decision.bits_ready && !decision.ambiguous &&
			      !memcmp(bits, decoded, mapping.data_bits_per_frame),
			      "spare-word recovery corrupted frame %u", f);
		}
	}
	return true;
}

int main(void)
{
	if (!test_spare_word_beam(64u, 1u) || !test_spare_word_beam(69u, 0u))
		return 1;
	if (!test_input_confidence(false) || !test_input_confidence(true) ||
	    !test_inactive_filter_history()) return 1;
	if (!test_trellis_polarity()) return 1;
	if (!test_polarity_acquisition_boundary()) return 1;
	static const int16_t levels[] = { 4096, 4104, 4224, 4351 };

	if (!test_precodec_noise(false) || !test_precodec_noise(true))
		return 1;

	for (unsigned int law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		if (!test_constellation_cells((enum g711_law)law, 0.8) ||
		    !test_constellation_cells((enum g711_law)law, 1.25)) return 1;
		static const uint8_t rates[] = { 1u, 13u, 19u };
		static const int16_t direct_44k_levels[] = { 2016, 4096, 7164, 8192 };
		for (enum v92_cpd_trellis trellis = V92_CPD_TRELLIS_16_STATE;
		     trellis <= V92_CPD_TRELLIS_64_STATE; ++trellis) {
			if (!test_peer_point_selection((enum g711_law)law, trellis)) return 1;
			for (uint8_t rate = 1u; rate <= V92_CPD_MAX_DATA_RATE_NUMBER; ++rate)
				for (unsigned balanced = 0u; balanced < 2u; ++balanced)
					for (int polarity = -1; polarity <= 1; polarity += 2)
						if (!test_constellation_polarity((enum g711_law)law, rate,
							trellis, balanced, polarity)) return 1;
			for (uint8_t rate = 1u; rate <= V92_CPD_MAX_DATA_RATE_NUMBER; ++rate)
				if (!test_rm_receive((enum g711_law)law, rate, trellis, 0u)) return 1;
			for (unsigned scenario = 1u; scenario <= 2u; ++scenario)
				if (!test_rm_receive((enum g711_law)law, 1u, trellis, scenario)) return 1;
		}

		for (unsigned taps = 3; taps <= 4; taps++)
			if (!test_observed_channel((enum g711_law)law, taps, false) ||
			    !test_observed_channel((enum g711_law)law, taps, true)) return 1;

		for (size_t level = 0u; level < ARRAY_LENGTH(direct_44k_levels); ++level)
			if (!test_direct_44k_level((enum g711_law)law, direct_44k_levels[level]))
				return 1;

		if (!test_compensated_channel((enum g711_law)law) ||
		    !test_b1u_validation((enum g711_law)law))
			return 1;
		for (size_t rate = 0u; rate < sizeof(rates) / sizeof(rates[0]); ++rate) {
			for (enum v92_cpd_trellis trellis = V92_CPD_TRELLIS_16_STATE;
			     trellis <= V92_CPD_TRELLIS_64_STATE; ++trellis) {
				if (!test_constellation_coordinates((enum g711_law)law, rates[rate], trellis, false) ||
				    (rates[rate] == 1u &&
				     !test_constellation_coordinates((enum g711_law)law, 1u, trellis, true)))
					return 1;
			}
		}
		for (size_t level = 0u; level < sizeof(levels) / sizeof(levels[0]); ++level) {
			if (!test_uncertain_level((enum g711_law)law, 13u, levels[level]) ||
			    !test_uncertain_level((enum g711_law)law, 19u, levels[level]))
				return 1;
		}
		for (uint8_t rate = 1u; rate <= V92_CPD_MAX_DATA_RATE_NUMBER; ++rate) {
			if (!run_stream((enum g711_law)law, rate, false, false) ||
			    !test_control_epoch((enum g711_law)law, rate, false))
				return 1;
		}
		if (!run_stream((enum g711_law)law, 13u, true, false) ||
		    !test_control_epoch((enum g711_law)law, 13u, true))
			return 1;
	}
	return run_stream(G711_LAW_PCMA, 16u, false, true) &&
		test_noise_training(false) && test_noise_training(true) &&
		test_rejection_and_training_evidence() ? 0 : 1;
}
