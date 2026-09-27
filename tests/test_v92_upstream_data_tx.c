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
					      v92_pcm_frame_scale(mapping.cpd.four_times_gain_q0_16, TEST_LEVEL,
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
		      v92_pcm_frame_scale(mapping.cpd.four_times_gain_q0_16, TEST_LEVEL, frame.v[index], &expected) ==
			      V92_PCM_FRAME_OK && output == expected,
		      "idle did not preserve the coding epoch");
	}
	return true;
}

static bool test_rm_sequence(enum g711_law law, uint8_t rate, bool filtered, unsigned offset)
{
	struct v92_pcm_mapping mapping;
	struct v92_upstream_data_tx transmit;
	struct source_state input = { 0 };
	int16_t sample;

	CHECK(mapping_init(&mapping, law, rate, filtered) &&
		v92_upstream_data_tx_init(&transmit, &mapping, TEST_LEVEL) == V92_UPSTREAM_DATA_TX_OK,
		"RM fixture initialization failed");
	for (unsigned i = 0; i < V92_B1U_SYMBOLS + 36u + offset; ++i)
		CHECK(v92_upstream_data_tx_next(&transmit, source, &input, &sample) ==
			V92_UPSTREAM_DATA_TX_OK, "RM fixture did not reach data");
	struct v92_upstream_data_tx before = transmit;
	struct v92_pcm_frame_tx frames = transmit.encoder;
	unsigned source_calls = input.calls;

	if (before.queue_count != 0u) {
		struct v92_upstream_data_tx invalid = before;

		invalid.queue[V92_PCM_MAPPING_FRAME_SYMBOLS - before.queue_count - 1u] = 1;
		CHECK(!v92_upstream_data_tx_is_valid(&invalid),
			"consumed queue slot accepted");
	}

	input.invalid = 1u;
	CHECK(v92_upstream_data_tx_start_rm(&transmit) == V92_UPSTREAM_DATA_TX_OK &&
		transmit.rm_symbols_remaining == 408u + before.queue_count &&
		!v92_upstream_data_tx_is_online(&transmit), "RM request did not queue");
	struct v92_upstream_data_tx pending = transmit;
	CHECK(v92_upstream_data_tx_start_rm(&transmit) == V92_UPSTREAM_DATA_TX_INVALID_STATE &&
		v92_upstream_data_tx_reset_control(&transmit) == V92_UPSTREAM_DATA_TX_INVALID_STATE &&
		memcmp(&transmit, &pending, sizeof(transmit)) == 0,
		"pending RM request was restarted/reset");
	for (unsigned i = 0; i < before.queue_count; ++i)
		CHECK(v92_upstream_data_tx_next(&transmit, source, &input, &sample) ==
			V92_UPSTREAM_DATA_TX_OK &&
			sample == before.queue[V92_PCM_MAPPING_FRAME_SYMBOLS - before.queue_count + i] &&
			memcmp(&transmit.encoder, &before.encoder, sizeof(before.encoder)) == 0,
			"RM damaged the pending data-frame tail");
	for (unsigned f = 0; f < 34u; ++f) {
		struct v92_pcm_frame frame;

		CHECK(v92_pcm_frame_encode_rm(&frames.mapping, &frames.state,
			f >= 32u, &frame) == V92_PCM_FRAME_OK,
			"RM frame did not encode");
		for (unsigned i = 0; i < 12u; ++i) {
			bool complete = f == 33u && i == 11u;
			int16_t expected;

			if (complete)
				CHECK(v92_pcm_frame_tx_reset_control(&frames) == V92_PCM_FRAME_OK,
					"RM control reset failed");
			CHECK(v92_pcm_frame_scale(mapping.cpd.four_times_gain_q0_16, TEST_LEVEL, frame.v[i], &expected) ==
				V92_PCM_FRAME_OK &&
				v92_upstream_data_tx_next(&transmit, source, &input, &sample) ==
				(complete ? V92_UPSTREAM_DATA_TX_RM_COMPLETE : V92_UPSTREAM_DATA_TX_OK) &&
				sample == expected && input.calls == source_calls &&
				transmit.rm_symbols_remaining == 407u - (f * 12u + i) &&
				v92_upstream_data_tx_is_valid(&transmit) &&
				v92_upstream_data_tx_is_online(&transmit) == complete &&
				memcmp(&transmit.encoder, &frames, sizeof(frames)) == 0,
				"RM discontinuity: law %u rate %u filtered %u offset %u frame %u symbol %u",
				law, rate, filtered, offset, f, i);
		}
	}
	/* The next source is control in the previous data mapping, with no B1u.
	 * Backpressure here must leave the completed request boundary intact. */
	pending = transmit;
	sample = 12345;
	CHECK(v92_upstream_data_tx_next(&transmit, source, &input, &sample) ==
		V92_UPSTREAM_DATA_TX_SOURCE_FAILURE && sample == 12345 &&
		memcmp(&transmit, &pending, sizeof(transmit)) == 0,
		"rejected post-RM control source consumed the boundary");
	input = (struct source_state){ 0 };
	uint8_t bits[V92_PCM_MAPPING_MAX_BITS];
	struct v92_pcm_frame frame;
	pattern(bits, mapping.data_bits_per_frame, 0u);
	CHECK(v92_pcm_frame_encode(&frames, bits, mapping.data_bits_per_frame, &frame) ==
		V92_PCM_FRAME_OK, "post-RM control frame failed");
	for (unsigned i = 0; i < 12u; ++i) {
		int16_t expected;

		CHECK(v92_pcm_frame_scale(mapping.cpd.four_times_gain_q0_16, TEST_LEVEL, frame.v[i], &expected) == V92_PCM_FRAME_OK &&
			v92_upstream_data_tx_next(&transmit, source, &input, &sample) ==
			V92_UPSTREAM_DATA_TX_OK && sample == expected && input.calls == 1u,
			"post-RM control reset the channel or inserted B1u");
	}
	return true;
}

static bool test_rm_matrix(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (uint8_t rate = 1u; rate <= V92_CPD_MAX_DATA_RATE_NUMBER; ++rate)
			for (unsigned filtered = 0u; filtered < 2u; ++filtered)
				for (unsigned offset = 0u; offset < 12u; ++offset)
					CHECK(test_rm_sequence(law, rate, filtered != 0u, offset),
						"RM sequence matrix failed");
	return true;
}

static bool test_finish_sequence(enum g711_law law, uint8_t rate, bool filtered)
{
	struct v92_pcm_mapping mapping;
	struct v92_upstream_data_tx tx;
	struct source_state input = { 0 };
	int16_t sample;
	CHECK(mapping_init(&mapping, law, rate, filtered), "finish mapping failed");
	/* The OLD CPd can retain initial training's extension request. */
	mapping.cpd.extend_e2u = filtered;
	CHECK(v92_upstream_data_tx_init(&tx, &mapping, TEST_LEVEL) == V92_UPSTREAM_DATA_TX_OK,
	      "finish initialization failed");
	for (unsigned i = 0; i < V92_B1U_SYMBOLS + 48; ++i) {
		CHECK(v92_upstream_data_tx_next(&tx, source, &input, &sample) == V92_UPSTREAM_DATA_TX_OK,
		      "finish warmup failed");
		if (tx.queue_count) {
			struct v92_upstream_data_tx saved = tx;
			CHECK(v92_upstream_data_tx_start_e2u(&tx) == V92_UPSTREAM_DATA_TX_INVALID_STATE &&
			      memcmp(&tx, &saved, sizeof(tx)) == 0, "partial-frame finish accepted");
		}
	}
	struct v92_pcm_frame_tx frames = tx.encoder;
	CHECK(v92_upstream_data_tx_start_e2u(&tx) == V92_UPSTREAM_DATA_TX_OK,
	      "finish did not start");
	struct v92_upstream_data_tx pending = tx;
	CHECK(v92_upstream_data_tx_start_e2u(&tx) == V92_UPSTREAM_DATA_TX_INVALID_STATE &&
	      v92_upstream_data_tx_start_rm(&tx) == V92_UPSTREAM_DATA_TX_INVALID_STATE &&
	      v92_upstream_data_tx_reset_control(&tx) == V92_UPSTREAM_DATA_TX_INVALID_STATE &&
	      memcmp(&tx, &pending, sizeof(tx)) == 0, "pending finish was restarted/reset");
	input.invalid = 1;
	for (unsigned f = 0; f <= V92_B1U_FRAMES; ++f) {
		struct v92_pcm_frame frame;
		uint8_t bits[V92_PCM_MAPPING_MAX_BITS];
		memset(bits, f != 0, mapping.data_bits_per_frame);
		CHECK(v92_pcm_frame_encode(&frames, bits, mapping.data_bits_per_frame, &frame) ==
		      V92_PCM_FRAME_OK, "finish frame failed");
		if (f == 1) {
			struct v92_pcm_frame_tx reset;
			struct v92_pcm_frame wrong;
			CHECK(v92_pcm_frame_tx_init(&reset, &mapping) == V92_PCM_FRAME_OK &&
			      v92_pcm_frame_encode(&reset, bits, mapping.data_bits_per_frame, &wrong) == V92_PCM_FRAME_OK &&
			      memcmp(frame.v, wrong.v, sizeof(frame.v)) != 0,
			      "FB1u fixture cannot detect a premature coding reset");
		}
		for (unsigned i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i) {
			bool complete = f == V92_B1U_FRAMES && i == V92_PCM_MAPPING_FRAME_SYMBOLS - 1;
			int16_t expected;
			CHECK(!v92_upstream_data_tx_is_online(&tx) &&
			      v92_pcm_frame_scale(mapping.cpd.four_times_gain_q0_16, TEST_LEVEL, frame.v[i], &expected) == V92_PCM_FRAME_OK &&
			      v92_upstream_data_tx_next(&tx, source, &input, &sample) ==
			      (complete ? V92_UPSTREAM_DATA_TX_FB1U_COMPLETE : V92_UPSTREAM_DATA_TX_OK) &&
			      sample == expected && input.calls == 4 &&
			      v92_upstream_data_tx_is_valid(&tx) &&
			      v92_upstream_data_tx_is_online(&tx) == complete &&
			      memcmp(&tx.encoder, &frames, sizeof(frames)) == 0,
			      "E2u/FB1u discontinuity: law %u rate %u filtered %u frame %u sample %u",
			      law, rate, filtered, f, i);
		}
	}
	return true;
}

static bool test_finish_matrix(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (uint8_t rate = 1; rate <= V92_CPD_MAX_DATA_RATE_NUMBER; ++rate)
			for (unsigned filtered = 0; filtered < 2; ++filtered)
				CHECK(test_finish_sequence(law, rate, filtered != 0), "finish matrix failed");
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
	      v92_upstream_data_tx_start_rm(NULL) == V92_UPSTREAM_DATA_TX_INVALID_ARGUMENT &&
	      v92_upstream_data_tx_start_rm(&transmit) == V92_UPSTREAM_DATA_TX_INVALID_STATE &&
	      v92_upstream_data_tx_start_e2u(NULL) == V92_UPSTREAM_DATA_TX_INVALID_ARGUMENT &&
	      v92_upstream_data_tx_start_e2u(&transmit) == V92_UPSTREAM_DATA_TX_INVALID_STATE &&
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
	      v92_upstream_data_tx_next_owned(NULL, NULL, NULL, &output) ==
		      V92_UPSTREAM_DATA_TX_INVALID_ARGUMENT &&
	      v92_upstream_data_tx_next_owned(&transmit, NULL, NULL, NULL) ==
		      V92_UPSTREAM_DATA_TX_INVALID_ARGUMENT &&
	      v92_upstream_data_tx_next_owned(&transmit, NULL, NULL, &transmit.queue[0]) ==
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
	transmit.rm_symbols_remaining = 408u;
	CHECK(!v92_upstream_data_tx_is_valid(&transmit), "RM during B1u accepted");
	transmit.b1u_symbols_remaining = 0u;
	CHECK(v92_upstream_data_tx_is_valid(&transmit), "aligned RM state rejected");
	transmit.rm_symbols_remaining = 409u;
	CHECK(!v92_upstream_data_tx_is_valid(&transmit), "RM queue/count mismatch accepted");
	transmit.rm_symbols_remaining = 420u;
	CHECK(!v92_upstream_data_tx_is_valid(&transmit), "overlong RM sequence accepted");
	transmit = saved;
	transmit.finish_symbols_remaining = 588;
	CHECK(!v92_upstream_data_tx_is_valid(&transmit), "finish during B1u accepted");
	transmit.b1u_symbols_remaining = 0;
	CHECK(v92_upstream_data_tx_is_valid(&transmit), "aligned finish rejected");
	transmit.rm_symbols_remaining = 408;
	CHECK(!v92_upstream_data_tx_is_valid(&transmit), "overlapping finish/RM accepted");
	transmit.rm_symbols_remaining = 0;
	transmit.finish_symbols_remaining = 587;
	CHECK(!v92_upstream_data_tx_is_valid(&transmit), "finish queue mismatch accepted");
	transmit.finish_symbols_remaining = 600;
	CHECK(!v92_upstream_data_tx_is_valid(&transmit), "overlong finish accepted");
	transmit = saved;
	transmit.queue[11] = 1;
	CHECK(!v92_upstream_data_tx_is_valid(&transmit), "unowned queue sample accepted");
	return true;
}

static bool test_owned_clock_case(enum g711_law law, uint8_t rate, bool filtered)
{
	struct v92_pcm_mapping mapping;
	struct v92_upstream_data_tx checked;
	struct v92_upstream_data_tx owned;
	struct source_state checked_source = { 0 };
	struct source_state owned_source = { 0 };

	CHECK(mapping_init(&mapping, law, rate, filtered) &&
	      v92_upstream_data_tx_init(&checked, &mapping, TEST_LEVEL) ==
		      V92_UPSTREAM_DATA_TX_OK, "owned clock initialization failed");
	owned = checked;
	for (unsigned phase = 0u; phase < 3u; ++phase) {
		size_t count = phase == 0u ? V92_B1U_SYMBOLS + 48u :
			phase == 1u ? 408u :
			V92_PCM_MAPPING_FRAME_SYMBOLS + V92_B1U_SYMBOLS;

		if (phase == 1u)
			CHECK(v92_upstream_data_tx_start_rm(&checked) ==
				      V92_UPSTREAM_DATA_TX_OK &&
			      v92_upstream_data_tx_start_rm(&owned) ==
				      V92_UPSTREAM_DATA_TX_OK,
			      "owned clock RM setup failed");
		if (phase == 2u)
			CHECK(v92_upstream_data_tx_start_e2u(&checked) ==
				      V92_UPSTREAM_DATA_TX_OK &&
			      v92_upstream_data_tx_start_e2u(&owned) ==
				      V92_UPSTREAM_DATA_TX_OK,
			      "owned clock finish setup failed");
		for (size_t index = 0u; index < count; ++index) {
			int16_t checked_sample = 0;
			int16_t owned_sample = 0;
			enum v92_upstream_data_tx_result checked_result =
				v92_upstream_data_tx_next(
					&checked, source, &checked_source,
					&checked_sample);
			enum v92_upstream_data_tx_result owned_result =
				v92_upstream_data_tx_next_owned(
					&owned, source, &owned_source, &owned_sample);

			CHECK(checked_result == owned_result &&
			      checked_sample == owned_sample &&
			      checked_source.calls == owned_source.calls &&
			      memcmp(&checked, &owned, sizeof(checked)) == 0 &&
			      v92_upstream_data_tx_is_valid(&owned),
			      "owned clock diverged at phase %u sample %zu",
			      phase, index);
		}
	}
	return true;
}

static bool test_owned_clock(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (uint8_t rate = 1u; rate <= V92_CPD_MAX_DATA_RATE_NUMBER; ++rate)
			for (unsigned filtered = 0u; filtered < 2u; ++filtered)
				CHECK(test_owned_clock_case(law, rate, filtered != 0u),
				      "owned clock matrix failed");
	return true;
}

static bool test_owned_frame_rollback(void)
{
	struct v92_pcm_mapping mapping;
	struct v92_upstream_data_tx tx;

	CHECK(mapping_init(&mapping, G711_LAW_PCMU, 19u, true) &&
	      v92_upstream_data_tx_init(&tx, &mapping, TEST_LEVEL) ==
		      V92_UPSTREAM_DATA_TX_OK, "rollback fixture failed");
	tx.b1u_symbols_remaining = 0u;
	/* Even on the block-owner path, a late scaling failure must not commit
	 * already encoded scrambler, differential, trellis or filter histories. */
	for (unsigned fault = 0u; fault < 3u; ++fault) {
		struct v92_upstream_data_tx invalid = tx;

		if (fault == 0u)
			invalid.upstream_level = 0;
		else if (fault == 1u)
			invalid.encoder.mapping.cpd.four_times_gain_q0_16 = 0u;
		else
			invalid.encoder.state.differential = 2u;
		for (unsigned source_fault = 0u; source_fault < 4u; ++source_fault) {
			struct v92_upstream_data_tx saved = invalid;
			struct source_state input = { .invalid = source_fault };
			int16_t sample = 12345;

			CHECK(v92_upstream_data_tx_next_owned(&invalid, source,
				      &input, &sample) == (source_fault != 0u ?
				      V92_UPSTREAM_DATA_TX_SOURCE_FAILURE :
				      V92_UPSTREAM_DATA_TX_NUMERIC_FAILURE) &&
			      input.calls == 1u && sample == 12345 &&
			      memcmp(&invalid, &saved, sizeof(saved)) == 0,
			      "owned frame failure committed state: fault %u source %u",
			      fault, source_fault);
		}
	}
	return true;
}

int main(void)
{
	return test_stream(false) && test_stream(true) &&
	       test_rm_matrix() && test_finish_matrix() &&
	       test_rejected_sources_and_idle() && test_validation() &&
	       test_owned_clock() && test_owned_frame_rollback() ? 0 : 1;
}
