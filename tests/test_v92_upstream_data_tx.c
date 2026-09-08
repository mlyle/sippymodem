// SPDX-License-Identifier: BSD-3-Clause

#include <v92_upstream_data_tx.h>
#include <v92_cpd_direct.h>

#include "test.h"

#include <string.h>

enum { TEST_LEVEL = 4096, DATA_FRAMES = 4 };

struct source_state {
	unsigned int calls;
	unsigned int invalid;
};

static void pattern(uint8_t *bits, size_t count, unsigned int frame)
{
	uint32_t random = UINT32_C(0x62d71a95) ^ frame;

	for (size_t index = 0u; index < count; ++index) {
		random = random * UINT32_C(1664525) + UINT32_C(1013904223);
		bits[index] = (uint8_t)(random >> 31);
	}
}

static bool source(void *context, uint8_t *bits, size_t count)
{
	struct source_state *state = context;

	state->calls++;
	if (state->invalid == 1u)
		return false;
	if (state->invalid == 2u)
		return true;
	pattern(bits, count, state->calls - 1u);
	if (state->invalid == 3u)
		bits[count - 1u] = 2u;
	return true;
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

static bool test_stream(bool filtered)
{
	/* Independent first payload frame after 48 B1u frames at DRN1/LU4096.
	 * Pins the coding epoch as well as the sample-to-media conversion. */
	static const uint8_t first_data[2][V92_PCM_MAPPING_FRAME_SYMBOLS] = {
		{ 0x57, 0xb5, 0x2d, 0x1e, 0xb5, 0xd7,
		  0xbf, 0xbf, 0xb5, 0x35, 0xbf, 0x9c },
		{ 0x71, 0x9c, 0x07, 0x35, 0x9c, 0xf1,
		  0xeb, 0xeb, 0x9c, 0x1c, 0xeb, 0xb6 },
	};

	for (unsigned int law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		for (uint8_t rate = 1u; rate <= V92_CPD_MAX_DATA_RATE_NUMBER; ++rate) {
			struct v92_pcm_mapping mapping;
			struct v92_upstream_data_tx transmit;
			struct v92_pcm_frame_tx frames;
			struct source_state input = { 0 };
			uint8_t bits[V92_PCM_MAPPING_MAX_BITS];

			CHECK(mapping_init(&mapping, (enum g711_law)law,
				      rate, filtered) &&
			      v92_upstream_data_tx_init(&transmit, &mapping, TEST_LEVEL) ==
				      V92_UPSTREAM_DATA_TX_OK &&
			      v92_pcm_frame_tx_init(&frames, &mapping) == V92_PCM_FRAME_OK,
			      "rate %u law %u initialization failed", rate, law);
			for (unsigned int frame_index = 0u;
			     frame_index < V92_B1U_FRAMES + DATA_FRAMES; ++frame_index) {
				struct v92_pcm_frame frame;
				bool b1u = frame_index < V92_B1U_FRAMES;

				if (b1u)
					memset(bits, 1, mapping.data_bits_per_frame);
				else
					pattern(bits, mapping.data_bits_per_frame,
						frame_index - V92_B1U_FRAMES);
				CHECK(v92_pcm_frame_encode(&frames, bits,
					      mapping.data_bits_per_frame, &frame) ==
					      V92_PCM_FRAME_OK, "frame encoding failed");
				for (size_t symbol = 0u;
				     symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
					int16_t actual;
					int16_t expected;
					size_t sent = frame_index *
						V92_PCM_MAPPING_FRAME_SYMBOLS + symbol;
					unsigned int calls = b1u ? 0u :
						frame_index - V92_B1U_FRAMES + 1u;

					CHECK(v92_upstream_data_tx_is_online(&transmit) ==
						      (sent >= V92_B1U_SYMBOLS) &&
					      v92_upstream_data_tx_next(&transmit,
						      source, &input, &actual) ==
						      V92_UPSTREAM_DATA_TX_OK &&
					      v92_pcm_frame_scale(&mapping, TEST_LEVEL,
						      frame.v[symbol], &expected) ==
						      V92_PCM_FRAME_OK &&
					      actual == expected &&
					      input.calls == calls &&
					      v92_upstream_data_tx_is_valid(&transmit),
					      "rate %u law %u frame %u symbol %zu discontinuity",
					      rate, law, frame_index, symbol);
					if (!filtered && rate == 1u &&
					    frame_index == V92_B1U_FRAMES) {
						uint8_t octet = law == G711_LAW_PCMU ?
							g711_encode_pcmu(actual) :
							g711_encode_pcma(actual);

						CHECK(octet == first_data[law][symbol],
						      "law %u payload fixture changed at %zu",
						      law, symbol);
					}
				}
			}
			CHECK(input.calls == DATA_FRAMES &&
			      transmit.queue_count == 0u &&
			      v92_upstream_data_tx_is_online(&transmit) &&
			      memcmp(&frames.state, &transmit.encoder.state,
				      sizeof(frames.state)) == 0,
			      "B1u/data coding histories did not continue");
		}
	}
	return true;
}

static bool test_rejected_sources_and_idle(void)
{
	struct v92_pcm_mapping mapping;
	struct v92_upstream_data_tx transmit;
	struct v92_upstream_data_tx saved;
	struct v92_pcm_frame_tx frames;
	struct v92_pcm_frame frame;
	struct source_state input = { .invalid = 1u };
	uint8_t ones[V92_PCM_MAPPING_MAX_BITS];
	int16_t output;

	CHECK(mapping_init(&mapping, G711_LAW_PCMU, 1u, false) &&
	      v92_upstream_data_tx_init(&transmit, &mapping, TEST_LEVEL) ==
		      V92_UPSTREAM_DATA_TX_OK, "initialization failed");
	for (size_t index = 0u; index < V92_B1U_SYMBOLS; ++index)
		CHECK(v92_upstream_data_tx_next(&transmit, source, &input, &output) ==
			      V92_UPSTREAM_DATA_TX_OK && input.calls == 0u,
		      "B1u consumed or rejected a payload source");
	saved = transmit;
	for (unsigned int invalid = 1u; invalid <= 3u; ++invalid) {
		input = (struct source_state){ .invalid = invalid };
		output = 12345;
		CHECK(v92_upstream_data_tx_next(&transmit, source, &input, &output) ==
			      V92_UPSTREAM_DATA_TX_SOURCE_FAILURE &&
		      input.calls == 1u && output == 12345 &&
		      memcmp(&transmit, &saved, sizeof(saved)) == 0,
		      "rejected source %u consumed state or output", invalid);
	}
	/* A retry requests one frame only; queued samples never revisit it. */
	input = (struct source_state){ 0 };
	CHECK(v92_upstream_data_tx_next(&transmit, source, &input, &output) ==
		      V92_UPSTREAM_DATA_TX_OK && input.calls == 1u,
	      "source retry failed");
	input.invalid = 1u;
	for (size_t index = 1u; index < V92_PCM_MAPPING_FRAME_SYMBOLS; ++index)
		CHECK(v92_upstream_data_tx_next(&transmit, source, &input, &output) ==
			      V92_UPSTREAM_DATA_TX_OK && input.calls == 1u,
		      "queued sample revisited the source");
	frames = transmit.encoder;
	memset(ones, 1, mapping.data_bits_per_frame);
	CHECK(v92_pcm_frame_encode(&frames, ones, mapping.data_bits_per_frame, &frame) ==
		      V92_PCM_FRAME_OK, "idle frame failed");
	for (size_t index = 0u; index < V92_PCM_MAPPING_FRAME_SYMBOLS; ++index) {
		int16_t expected;

		CHECK(v92_upstream_data_tx_next(&transmit, NULL, NULL, &output) ==
			      V92_UPSTREAM_DATA_TX_OK &&
		      v92_pcm_frame_scale(&mapping, TEST_LEVEL, frame.v[index], &expected) ==
			      V92_PCM_FRAME_OK && output == expected,
		      "idle did not preserve the coding epoch");
	}
	return true;
}

static bool test_validation(void)
{
	struct v92_pcm_mapping mapping;
	struct v92_upstream_data_tx transmit;
	struct v92_upstream_data_tx saved;
	int16_t output = 12345;

	CHECK(mapping_init(&mapping, G711_LAW_PCMA, 19u, false) &&
	      v92_upstream_data_tx_init(&transmit, &mapping, TEST_LEVEL) ==
		      V92_UPSTREAM_DATA_TX_OK, "initialization failed");
	saved = transmit;
	CHECK(!v92_upstream_data_tx_is_valid(NULL) &&
	      !v92_upstream_data_tx_is_online(NULL) &&
	      v92_upstream_data_tx_reset_control(NULL) == V92_UPSTREAM_DATA_TX_INVALID_ARGUMENT &&
	      v92_upstream_data_tx_reset_control(&transmit) == V92_UPSTREAM_DATA_TX_INVALID_STATE &&
	      v92_upstream_data_tx_init(NULL, &mapping, TEST_LEVEL) ==
		      V92_UPSTREAM_DATA_TX_INVALID_ARGUMENT &&
	      v92_upstream_data_tx_init(&transmit, NULL, TEST_LEVEL) ==
		      V92_UPSTREAM_DATA_TX_INVALID_ARGUMENT &&
	      v92_upstream_data_tx_init(&transmit, &mapping, 0) ==
		      V92_UPSTREAM_DATA_TX_OUT_OF_RANGE &&
	      v92_upstream_data_tx_next(NULL, NULL, NULL, &output) ==
		      V92_UPSTREAM_DATA_TX_INVALID_ARGUMENT &&
	      v92_upstream_data_tx_next(&transmit, NULL, NULL, NULL) ==
		      V92_UPSTREAM_DATA_TX_INVALID_ARGUMENT &&
	      v92_upstream_data_tx_next(&transmit, NULL, NULL, &transmit.queue[0]) ==
		      V92_UPSTREAM_DATA_TX_INVALID_ARGUMENT &&
	      output == 12345 && memcmp(&transmit, &saved, sizeof(saved)) == 0,
	      "invalid arguments changed state or output");
	mapping.data_bits_per_frame = 0u;
	CHECK(v92_upstream_data_tx_init(&transmit, &mapping, TEST_LEVEL) ==
		      V92_UPSTREAM_DATA_TX_INVALID_PARAMETERS &&
	      memcmp(&transmit, &saved, sizeof(saved)) == 0,
	      "invalid mapping changed state");
	transmit.b1u_symbols_remaining--;
	CHECK(!v92_upstream_data_tx_is_valid(&transmit), "invalid B1u queue accepted");
	struct v92_upstream_data_tx invalid = transmit;

	CHECK(v92_upstream_data_tx_reset_control(&transmit) == V92_UPSTREAM_DATA_TX_INVALID_STATE &&
	      memcmp(&transmit, &invalid, sizeof(transmit)) == 0,
	      "control reset accepted invalid state");
	transmit = saved;
	transmit.queue[11] = 1;
	CHECK(!v92_upstream_data_tx_is_valid(&transmit), "unowned queue sample accepted");
	return true;
}

int main(void)
{
	return test_stream(false) && test_stream(true) &&
	       test_rejected_sources_and_idle() && test_validation() ? 0 : 1;
}
