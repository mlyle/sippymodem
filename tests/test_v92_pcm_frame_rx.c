// SPDX-License-Identifier: BSD-3-Clause

#include <v92_pcm_frame.h>
#include <v92_cpd_direct.h>

#include "test.h"

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
	int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS])
{
	struct v92_pcm_frame frame;

	if (v92_pcm_frame_encode(transmitter, bits,
			transmitter->mapping.data_bits_per_frame, &frame) !=
	    V92_PCM_FRAME_OK)
		return false;
	for (size_t symbol = 0u; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		int16_t sample;

		if (v92_pcm_frame_scale(&transmitter->mapping, UPSTREAM_LEVEL,
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
		CHECK(waveform(&transmitter, bits, samples) &&
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
		CHECK(waveform(&reencoded, decoded, replay) &&
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
	return true;
}

int main(void)
{
	for (unsigned int law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		for (uint8_t rate = 1u; rate <= V92_CPD_MAX_DATA_RATE_NUMBER; ++rate) {
			if (!run_stream((enum g711_law)law, rate, false))
				return 1;
		}
		if (!run_stream((enum g711_law)law, 13u, true))
			return 1;
	}
	return test_rejection_and_training_evidence() ? 0 : 1;
}
