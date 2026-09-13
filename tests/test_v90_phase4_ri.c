// SPDX-License-Identifier: BSD-3-Clause

#include <v90_info1.h>
#include <v90_phase4_ri.h>
#include <v90_phase4_ri_search.h>

#include "test.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint8_t expected_octet(
	enum g711_law law, uint8_t ucode, bool positive)
{
	uint8_t octet = law == G711_LAW_PCMA ?
			(uint8_t)(ucode ^ UINT8_C(0xd5)) :
			(uint8_t)~ucode;

	return positive ? octet : (uint8_t)(octet ^ UINT8_C(0x80));
}

static size_t expected_ri_symbols(size_t request_after)
{
	if (request_after <= V90_PHASE4_RI_MIN_SYMBOLS)
		return V90_PHASE4_RI_MIN_SYMBOLS;
	return ((request_after + V90_PHASE4_RI_PERIOD - 1u) /
		V90_PHASE4_RI_PERIOD) * V90_PHASE4_RI_PERIOD;
}

static bool check_exchange(
	enum g711_law law, uint8_t ucode, size_t request_after,
	bool invert_receive)
{
	struct v90_phase4_ri_tx tx;
	struct v90_phase4_ri_rx rx;
	size_t ri_symbols = expected_ri_symbols(request_after);
	size_t emitted = 0;
	unsigned int transitions = 0;

	CHECK(v90_phase4_ri_tx_init(&tx, law, ucode) ==
		      V90_PHASE4_RI_OK &&
	      v90_phase4_ri_rx_init(&rx, law, ucode) ==
		      V90_PHASE4_RI_OK,
	      "Ri endpoints did not initialize for law %u code %u",
	      (unsigned int)law, ucode);
	while (!v90_phase4_ri_tx_is_complete(&tx) && emitted < 1000u) {
		enum v90_phase4_ri_result tx_result;
		enum v90_phase4_ri_result rx_result;
		bool bar = emitted >= ri_symbols;
		size_t phase = bar ? emitted - ri_symbols : emitted;
		bool positive = bar ? phase % V90_PHASE4_RI_PERIOD >= 3u :
				      phase % V90_PHASE4_RI_PERIOD < 3u;
		uint8_t octet = 0xa5;

		if (emitted == request_after)
			CHECK(v90_phase4_ri_tx_request_finish(&tx) ==
				      V90_PHASE4_RI_OK,
			      "Ri finish request failed at %zu", emitted);
		tx_result = v90_phase4_ri_tx_next(&tx, &octet);
		CHECK(octet == expected_octet(law, ucode, positive),
		      "law %u Ri symbol %zu has the wrong network character",
		      (unsigned int)law, emitted);
		if (invert_receive)
			octet = expected_octet(law, ucode, !positive);
		rx_result = v90_phase4_ri_rx_receive(&rx, octet);
		if (emitted == ri_symbols) {
			CHECK(rx_result == V90_PHASE4_RI_TRANSITION_DETECTED,
			      "Ri-bar transition returned %s at %zu",
			      v90_phase4_ri_result_name(rx_result), emitted);
			transitions++;
		} else if (emitted + 1u ==
			   ri_symbols + V90_PHASE4_RI_BAR_SYMBOLS) {
			CHECK(tx_result == V90_PHASE4_RI_OK &&
			      rx_result == V90_PHASE4_RI_DONE,
			      "Ri endpoints did not complete at %zu", emitted);
		} else {
			CHECK(tx_result == V90_PHASE4_RI_OK &&
			      rx_result == V90_PHASE4_RI_OK,
			      "Ri exchange returned %s/%s at %zu",
			      v90_phase4_ri_result_name(tx_result),
			      v90_phase4_ri_result_name(rx_result), emitted);
		}
		emitted++;
	}
	CHECK(emitted == ri_symbols + V90_PHASE4_RI_BAR_SYMBOLS &&
	      transitions == 1 && v90_phase4_ri_tx_is_complete(&tx) &&
	      v90_phase4_ri_rx_is_complete(&rx) &&
	      tx.ri_symbols == V90_PHASE4_RI_MIN_SYMBOLS &&
	      rx.ri_symbols == V90_PHASE4_RI_MIN_SYMBOLS &&
	      rx.inverted == invert_receive,
	      "law %u Ri exchange ended at the wrong boundary",
	      (unsigned int)law);
	{
		struct v90_phase4_ri_tx saved_tx = tx;
		struct v90_phase4_ri_rx saved_rx = rx;
		uint8_t octet = 0x5a;

		CHECK(v90_phase4_ri_tx_next(&tx, &octet) ==
			      V90_PHASE4_RI_DONE &&
		      v90_phase4_ri_tx_request_finish(&tx) ==
			      V90_PHASE4_RI_DONE &&
		      v90_phase4_ri_rx_receive(&rx, 0xa5) ==
			      V90_PHASE4_RI_DONE &&
		      octet == 0x5a &&
		      memcmp(&tx, &saved_tx, sizeof(tx)) == 0 &&
		      memcmp(&rx, &saved_rx, sizeof(rx)) == 0,
		      "completed Ri endpoints consumed another character");
	}
	return true;
}

static bool test_exact_sequences(void)
{
	CHECK(V90_PHASE4_RI_PERIOD == 6 &&
	      V90_PHASE4_RI_MIN_SYMBOLS == 192 &&
	      V90_PHASE4_RI_BAR_SYMBOLS == 24,
	      "Ri standard lengths changed");
	for (enum g711_law law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law) {
		CHECK(check_exchange(
			      law, V90_INFO1A_MIN_TRAINING_UCODE, 0, false) &&
		      check_exchange(
			      law, V90_INFO1A_MAX_TRAINING_UCODE, 197, true),
		      "law %u Ri exchange failed", (unsigned int)law);
	}
	return true;
}

static bool feed_ri(
	struct v90_phase4_ri_rx *rx, enum g711_law law,
	uint8_t ucode, size_t count)
{
	for (size_t index = 0; index < count; ++index) {
		bool positive = index % V90_PHASE4_RI_PERIOD < 3u;

		if (v90_phase4_ri_rx_receive(
			    rx, expected_octet(law, ucode, positive)) !=
		    V90_PHASE4_RI_OK)
			return false;
	}
	return true;
}

static bool test_transition_rejection(void)
{
	struct v90_phase4_ri_rx rx;
	struct v90_phase4_ri_rx saved;
	uint8_t ucode = 91;

	CHECK(v90_phase4_ri_rx_init(&rx, G711_LAW_PCMU, ucode) ==
		      V90_PHASE4_RI_OK &&
	      feed_ri(&rx, G711_LAW_PCMU, ucode, 6),
	      "early-transition fixture did not initialize");
	saved = rx;
	CHECK(v90_phase4_ri_rx_receive(
		      &rx, expected_octet(G711_LAW_PCMU, ucode, false)) ==
		      V90_PHASE4_RI_INVALID_SEQUENCE &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0,
	      "receiver accepted Ri-bar before the 192T minimum");

	CHECK(v90_phase4_ri_rx_init(&rx, G711_LAW_PCMA, ucode) ==
		      V90_PHASE4_RI_OK &&
	      feed_ri(
		      &rx, G711_LAW_PCMA, ucode,
		      V90_PHASE4_RI_MIN_SYMBOLS + 1u),
	      "unaligned-transition fixture did not initialize");
	saved = rx;
	CHECK(v90_phase4_ri_rx_receive(
		      &rx, expected_octet(G711_LAW_PCMA, ucode, false)) ==
		      V90_PHASE4_RI_INVALID_SEQUENCE &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0,
	      "receiver accepted a mid-period Ri-bar transition");
	return true;
}

static bool check_search(
	enum g711_law law, bool inverted, uint8_t start_phase)
{
	struct v90_phase4_ri_search search;
	uint8_t ucode = 91;
	unsigned int acquisitions = 0;
	unsigned int transitions = 0;

	CHECK(v90_phase4_ri_search_init(&search, law, ucode) ==
		      V90_PHASE4_RI_SEARCH_OK,
	      "Ri search did not initialize for law %u",
	      (unsigned int)law);
	for (size_t index = 0; index < 24u; ++index) {
		bool positive = index % 2u != 0u;

		CHECK(v90_phase4_ri_search_receive(
			      &search,
			      expected_octet(law, ucode, positive)) ==
			      V90_PHASE4_RI_SEARCH_OK &&
		      !v90_phase4_ri_search_is_acquired(&search),
		      "non-Ri prefix acquired at %zu", index);
	}
	CHECK(v90_phase4_ri_search_receive(
		      &search, expected_octet(law, 0, true)) ==
		      V90_PHASE4_RI_SEARCH_OK,
	      "wrong-magnitude prefix was not ignored");
	for (size_t index = 0;
	     index < V90_PHASE4_RI_SEARCH_SYMBOLS; ++index) {
		bool positive = (start_phase + index) %
			V90_PHASE4_RI_PERIOD < 3u;
		enum v90_phase4_ri_search_result result;

		if (inverted)
			positive = !positive;
		result = v90_phase4_ri_search_receive(
			&search, expected_octet(law, ucode, positive));
		if (result == V90_PHASE4_RI_SEARCH_ACQUIRED)
			acquisitions++;
		else
			CHECK(result == V90_PHASE4_RI_SEARCH_OK,
			      "Ri search returned %s at %zu",
			      v90_phase4_ri_search_result_name(result), index);
	}
	CHECK(acquisitions == 1u &&
	      search.stage == V90_PHASE4_RI_SEARCH_AMBIGUOUS &&
	      v90_phase4_ri_search_is_acquired(&search),
	      "Ri search did not retain both polarity hypotheses");
	for (uint8_t phase = start_phase;
	     phase != 0u; phase = (uint8_t)((phase + 1u) %
					     V90_PHASE4_RI_PERIOD)) {
		bool positive = phase < 3u;

		if (inverted)
			positive = !positive;
		CHECK(v90_phase4_ri_search_receive(
			      &search,
			      expected_octet(law, ucode, positive)) ==
			      V90_PHASE4_RI_SEARCH_OK,
		      "Ri search did not advance to its frame boundary");
	}
	for (size_t index = 0; index < V90_PHASE4_RI_BAR_SYMBOLS; ++index) {
		bool positive = index % V90_PHASE4_RI_PERIOD >= 3u;
		enum v90_phase4_ri_search_result result;

		if (inverted)
			positive = !positive;
		result = v90_phase4_ri_search_receive(
			&search, expected_octet(law, ucode, positive));
		if (index == 0u) {
			CHECK(result ==
				      V90_PHASE4_RI_SEARCH_TRANSITION_DETECTED,
			      "Ri-bar search transition returned %s",
			      v90_phase4_ri_search_result_name(result));
			transitions++;
		} else if (index + 1u == V90_PHASE4_RI_BAR_SYMBOLS) {
			CHECK(result == V90_PHASE4_RI_SEARCH_DONE,
			      "Ri-bar search completion returned %s",
			      v90_phase4_ri_search_result_name(result));
		} else {
			CHECK(result == V90_PHASE4_RI_SEARCH_OK,
			      "Ri-bar search returned %s at %zu",
			      v90_phase4_ri_search_result_name(result), index);
		}
	}
	CHECK(transitions == 1u &&
	      v90_phase4_ri_search_is_complete(&search) &&
	      search.selected_hypothesis < 2u &&
	      search.hypotheses[search.selected_hypothesis].inverted ==
		      inverted,
	      "Ri search resolved the wrong polarity hypothesis");
	{
		struct v90_phase4_ri_search saved = search;

		CHECK(v90_phase4_ri_search_receive(&search, 0xa5) ==
			      V90_PHASE4_RI_SEARCH_DONE &&
		      memcmp(&search, &saved, sizeof(search)) == 0,
		      "completed Ri search consumed another character");
	}
	return true;
}

static bool check_search_reacquisition(enum g711_law law)
{
	struct v90_phase4_ri_search search;
	uint8_t ucode = 91;
	unsigned int acquisitions = 0;

	CHECK(v90_phase4_ri_search_init(&search, law, ucode) ==
		      V90_PHASE4_RI_SEARCH_OK,
	      "Ri reacquisition did not initialize for law %u",
	      (unsigned int)law);
	for (size_t index = 0;
	     index < V90_PHASE4_RI_SEARCH_SYMBOLS; ++index) {
		bool positive = index % V90_PHASE4_RI_PERIOD < 3u;
		enum v90_phase4_ri_search_result result =
			v90_phase4_ri_search_receive(
				&search, expected_octet(law, ucode, positive));

		if (result == V90_PHASE4_RI_SEARCH_ACQUIRED)
			acquisitions++;
		else
			CHECK(result == V90_PHASE4_RI_SEARCH_OK,
			      "initial Ri search returned %s at %zu",
			      v90_phase4_ri_search_result_name(result), index);
	}
	CHECK(acquisitions == 1u &&
	      v90_phase4_ri_search_is_acquired(&search) &&
	      v90_phase4_ri_search_receive(
		      &search, expected_octet(law, 0, true)) ==
		      V90_PHASE4_RI_SEARCH_OK &&
	      !v90_phase4_ri_search_is_acquired(&search) &&
	      search.history_count == 0u,
	      "malformed acquired Ri did not restart the sliding hunt");
	for (size_t index = 0;
	     index < V90_PHASE4_RI_SEARCH_SYMBOLS; ++index) {
		bool positive = index % V90_PHASE4_RI_PERIOD < 3u;
		enum v90_phase4_ri_search_result result =
			v90_phase4_ri_search_receive(
				&search, expected_octet(law, ucode, positive));

		if (result == V90_PHASE4_RI_SEARCH_ACQUIRED)
			acquisitions++;
		else
			CHECK(result == V90_PHASE4_RI_SEARCH_OK,
			      "repeated Ri search returned %s at %zu",
			      v90_phase4_ri_search_result_name(result), index);
	}
	CHECK(acquisitions == 2u &&
	      v90_phase4_ri_search_is_acquired(&search),
	      "Ri search did not reacquire after one malformed character");
	for (size_t index = 0; index < V90_PHASE4_RI_BAR_SYMBOLS; ++index) {
		bool positive = index % V90_PHASE4_RI_PERIOD >= 3u;
		enum v90_phase4_ri_search_result result =
			v90_phase4_ri_search_receive(
				&search, expected_octet(law, ucode, positive));

		if (index == 0u)
			CHECK(result ==
				      V90_PHASE4_RI_SEARCH_TRANSITION_DETECTED,
			      "reacquired Ri-bar transition returned %s",
			      v90_phase4_ri_search_result_name(result));
		else if (index + 1u == V90_PHASE4_RI_BAR_SYMBOLS)
			CHECK(result == V90_PHASE4_RI_SEARCH_DONE,
			      "reacquired Ri-bar completion returned %s",
			      v90_phase4_ri_search_result_name(result));
		else
			CHECK(result == V90_PHASE4_RI_SEARCH_OK,
			      "reacquired Ri-bar returned %s at %zu",
			      v90_phase4_ri_search_result_name(result), index);
	}
	CHECK(v90_phase4_ri_search_is_complete(&search),
	      "reacquired Ri search did not complete");
	return true;
}

static bool test_unframed_search(void)
{
	for (enum g711_law law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law) {
		for (uint8_t phase = 0; phase < V90_PHASE4_RI_PERIOD;
		     ++phase)
			CHECK(check_search(law, false, phase) &&
			      check_search(law, true, phase),
			      "law %u phase %u Ri search failed",
			      (unsigned int)law, phase);
		CHECK(check_search_reacquisition(law),
		      "law %u Ri reacquisition failed",
		      (unsigned int)law);
	}
	return true;
}

static uint8_t sampled_octet(enum g711_law law, int32_t sample)
{
	if (sample > INT16_MAX)
		sample = INT16_MAX;
	if (sample < INT16_MIN)
		sample = INT16_MIN;
	return law == G711_LAW_PCMU ? g711_encode_pcmu((int16_t)sample) :
		g711_encode_pcma((int16_t)sample);
}

static bool check_sampled_ri(
	enum g711_law law, uint8_t ucode, unsigned int channel,
	unsigned int start, bool inverted)
{
	static const int32_t taps[][3] = {
		{100, 0, 0}, {85, 0, 0}, {110, 0, 0}, {90, 8, 2},
	};
	struct v90_phase4_ri_tx tx;
	struct v90_phase4_ri_rx aligned;
	struct v90_phase4_ri_search search;
	int32_t previous = 0, second = 0;
	unsigned int acquisitions = 0, transitions = 0, completions = 0;

	CHECK(v90_phase4_ri_tx_init(&tx, law, ucode) == V90_PHASE4_RI_OK &&
	      v90_phase4_ri_tx_request_finish(&tx) == V90_PHASE4_RI_OK &&
	      v90_phase4_ri_rx_init(&aligned, law, ucode) == V90_PHASE4_RI_OK &&
	      v90_phase4_ri_search_init(&search, law, ucode) ==
		      V90_PHASE4_RI_SEARCH_OK,
	      "sampled Ri fixture failed to initialize");
	for (unsigned int index = 0; index < V90_PHASE4_RI_MIN_SYMBOLS +
	     V90_PHASE4_RI_BAR_SYMBOLS; ++index) {
		uint8_t octet;
		int32_t sample;
		enum v90_phase4_ri_search_result result;
		enum v90_phase4_ri_result expected =
			index == V90_PHASE4_RI_MIN_SYMBOLS ?
			V90_PHASE4_RI_TRANSITION_DETECTED :
			index + 1u == V90_PHASE4_RI_MIN_SYMBOLS +
			V90_PHASE4_RI_BAR_SYMBOLS ? V90_PHASE4_RI_DONE :
			V90_PHASE4_RI_OK;

		CHECK(v90_phase4_ri_tx_next(&tx, &octet) == V90_PHASE4_RI_OK,
		      "sampled Ri sender failed at %u", index);
		sample = law == G711_LAW_PCMU ? g711_decode_pcmu(octet) :
			g711_decode_pcma(octet);
		if (inverted)
			sample = -sample;
		octet = sampled_octet(law, (taps[channel][0] * sample +
			taps[channel][1] * previous + taps[channel][2] * second) /
			100);
		second = previous;
		previous = sample;
		CHECK(v90_phase4_ri_rx_receive(&aligned, octet) == expected,
		      "sampled aligned Ri failed at %u", index);
		if (index < start)
			continue;
		result = v90_phase4_ri_search_receive(&search, octet);
		if (result == V90_PHASE4_RI_SEARCH_ACQUIRED) {
			acquisitions++;
			CHECK(index + 1u == start + V90_PHASE4_RI_SEARCH_SYMBOLS,
			      "sampled Ri acquired at wrong boundary %u", index);
		} else if (result == V90_PHASE4_RI_SEARCH_TRANSITION_DETECTED) {
			transitions++;
			CHECK(index == V90_PHASE4_RI_MIN_SYMBOLS,
			      "sampled Ri reversed at wrong boundary %u", index);
		} else if (result == V90_PHASE4_RI_SEARCH_DONE) {
			completions++;
			CHECK(index + 1u == V90_PHASE4_RI_MIN_SYMBOLS +
			      V90_PHASE4_RI_BAR_SYMBOLS,
			      "sampled Ri handed off at wrong boundary %u", index);
		} else
			CHECK(result == V90_PHASE4_RI_SEARCH_OK,
			      "sampled Ri search failed at %u", index);
	}
	CHECK(acquisitions == 1u && transitions == 1u && completions == 1u &&
	      v90_phase4_ri_search_is_complete(&search) &&
	      search.hypotheses[search.selected_hypothesis].inverted == inverted,
	      "sampled Ri search lost its sequence or polarity");
	return true;
}

static bool test_sampled_sequences(void)
{
	static const uint8_t codes[] = {67, 79, 91, 111, 127};

	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (size_t code = 0; code < ARRAY_LENGTH(codes); ++code)
			for (unsigned int channel = 0; channel < 4; ++channel)
				for (unsigned int phase = 0; phase < 6; ++phase)
					CHECK(check_sampled_ri(law, codes[code], channel,
						      phase, false) &&
					      check_sampled_ri(law, codes[code], channel,
						      phase, true),
					      "law %u code %u channel %u phase %u failed",
					      law, codes[code], channel, phase);
	return true;
}

static bool test_level_window(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned int code = V90_INFO1A_MIN_TRAINING_UCODE;
		     code <= V90_INFO1A_MAX_TRAINING_UCODE; ++code) {
			int16_t level;

			CHECK(g711_ucode_sample(law, (uint8_t)code, true, &level),
			      "UINFO level lookup failed");
			int32_t samples[] = {0, INT16_MIN, INT16_MAX,
				level - level / 4 - 1, level - level / 4,
				level + level / 4, level + level / 4 + 1};
			for (size_t i = 0; i < ARRAY_LENGTH(samples); ++i) {
				int32_t value = samples[i];
				int32_t magnitude = value < 0 ? -value : value;
				bool expected = magnitude >= level - level / 4 &&
					magnitude <= level + level / 4;

				if (value > INT16_MAX)
					continue;
				CHECK(v90_phase4_ri_level_matches(law, (uint8_t)code,
						(int16_t)value) == expected,
				      "Ri magnitude window failed at %d", (int)value);
				if (value != INT16_MIN)
					CHECK(v90_phase4_ri_level_matches(law,
						(uint8_t)code, (int16_t)-value) == expected,
					      "Ri magnitude window depends on polarity");
			}
		}
	CHECK(!v90_phase4_ri_level_matches((enum g711_law)99, 91, 8000) &&
	      !v90_phase4_ri_level_matches(G711_LAW_PCMU, 66, 8000) &&
	      !v90_phase4_ri_level_matches(G711_LAW_PCMA, 128, 8000),
	      "Ri magnitude accepted invalid law or UINFO");
	return true;
}

static bool test_search_rejects_other_signals(void)
{
	static const int32_t sinusoid[] = {50, 100, 50, -50, -100, -50};

	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned int kind = 0; kind < 6; ++kind) {
			struct v90_phase4_ri_search search;
			int16_t level;
			uint32_t random = UINT32_C(0x96149725);

			CHECK(g711_ucode_sample(law, 91, true, &level) &&
			      v90_phase4_ri_search_init(&search, law, 91) ==
				      V90_PHASE4_RI_SEARCH_OK,
			      "non-Ri fixture failed to initialize");
			for (unsigned int index = 0; index < 16000; ++index) {
				int32_t sample = level;

				random = random * UINT32_C(1664525) + UINT32_C(1013904223);
				if (kind == 0)
					sample = 0;
				else if (kind == 2)
					sample *= index % 2 ? -1 : 1;
				else if (kind == 3)
					sample = sample * sinusoid[index % 6] / 100;
				else if (kind == 4)
					sample *= random >> 31 ? -1 : 1;
				else if (kind == 5)
					sample = sample / 2 * (index % 6 < 3 ? 1 : -1);
				CHECK(v90_phase4_ri_search_receive(&search,
					      sampled_octet(law, sample)) ==
					      V90_PHASE4_RI_SEARCH_OK &&
				      !v90_phase4_ri_search_is_acquired(&search),
				      "non-Ri kind %u acquired at %u", kind, index);
			}
		}
	return true;
}

static bool test_validation(void)
{
	struct v90_phase4_ri_tx tx;
	struct v90_phase4_ri_tx saved_tx;
	struct v90_phase4_ri_rx rx;
	struct v90_phase4_ri_rx saved_rx;
	struct v90_phase4_ri_search search;
	struct v90_phase4_ri_search saved_search;
	uint8_t octet = 0xa5;

	memset(&tx, 0x5a, sizeof(tx));
	memset(&rx, 0xa5, sizeof(rx));
	memset(&search, 0xa5, sizeof(search));
	saved_tx = tx;
	saved_rx = rx;
	saved_search = search;
	CHECK(v90_phase4_ri_tx_init(NULL, G711_LAW_PCMU, 91) ==
		      V90_PHASE4_RI_INVALID_ARGUMENT &&
	      v90_phase4_ri_rx_init(NULL, G711_LAW_PCMU, 91) ==
		      V90_PHASE4_RI_INVALID_ARGUMENT &&
	      v90_phase4_ri_tx_init(
		      &tx, (enum g711_law)99, 91) ==
		      V90_PHASE4_RI_INVALID_ARGUMENT &&
	      v90_phase4_ri_rx_init(
		      &rx, G711_LAW_PCMU,
		      V90_INFO1A_MIN_TRAINING_UCODE - 1u) ==
		      V90_PHASE4_RI_INVALID_ARGUMENT &&
	      v90_phase4_ri_search_init(
		      &search, (enum g711_law)99, 91) ==
		      V90_PHASE4_RI_SEARCH_INVALID_ARGUMENT &&
	      memcmp(&tx, &saved_tx, sizeof(tx)) == 0 &&
	      memcmp(&rx, &saved_rx, sizeof(rx)) == 0 &&
	      memcmp(&search, &saved_search, sizeof(search)) == 0,
	      "invalid Ri initialization changed state");
	CHECK(v90_phase4_ri_tx_next(NULL, &octet) ==
		      V90_PHASE4_RI_INVALID_ARGUMENT &&
	      v90_phase4_ri_tx_next(&tx, &octet) ==
		      V90_PHASE4_RI_INVALID_ARGUMENT &&
	      v90_phase4_ri_tx_request_finish(&tx) ==
		      V90_PHASE4_RI_INVALID_ARGUMENT &&
	      v90_phase4_ri_rx_receive(NULL, 0) ==
		      V90_PHASE4_RI_INVALID_ARGUMENT &&
	      v90_phase4_ri_rx_receive(&rx, 0) ==
		      V90_PHASE4_RI_INVALID_ARGUMENT &&
	      v90_phase4_ri_search_receive(NULL, 0) ==
		      V90_PHASE4_RI_SEARCH_INVALID_ARGUMENT &&
	      v90_phase4_ri_search_receive(&search, 0) ==
		      V90_PHASE4_RI_SEARCH_INVALID_ARGUMENT &&
	      octet == 0xa5 &&
	      memcmp(&tx, &saved_tx, sizeof(tx)) == 0 &&
	      memcmp(&rx, &saved_rx, sizeof(rx)) == 0 &&
	      memcmp(&search, &saved_search, sizeof(search)) == 0 &&
	      !v90_phase4_ri_tx_is_valid(NULL) &&
	      !v90_phase4_ri_rx_is_valid(NULL) &&
	      !v90_phase4_ri_search_is_valid(NULL),
	      "invalid Ri calls were not transactional");
	CHECK(v90_phase4_ri_tx_init(&tx, G711_LAW_PCMU, 91) ==
		      V90_PHASE4_RI_OK &&
	      v90_phase4_ri_tx_request_finish(&tx) ==
		      V90_PHASE4_RI_OK &&
	      v90_phase4_ri_tx_request_finish(&tx) ==
		      V90_PHASE4_RI_OK,
	      "Ri finish request was not idempotent");
	CHECK(v90_phase4_ri_rx_init(&rx, G711_LAW_PCMA, 91) ==
		      V90_PHASE4_RI_OK,
	      "Ri rejection fixture did not initialize");
	saved_rx = rx;
	CHECK(v90_phase4_ri_rx_receive(
		      &rx, expected_octet(G711_LAW_PCMA, 0, true)) ==
		      V90_PHASE4_RI_INVALID_SEQUENCE &&
	      memcmp(&rx, &saved_rx, sizeof(rx)) == 0,
	      "Ri receiver consumed a wrong-magnitude character");
	CHECK(strcmp(v90_phase4_ri_result_name(
		     V90_PHASE4_RI_TRANSITION_DETECTED),
		     "Ri-bar detected") == 0 &&
	      strcmp(v90_phase4_ri_result_name(
		     (enum v90_phase4_ri_result)99),
		     "unknown") == 0 &&
	      strcmp(v90_phase4_ri_search_result_name(
		     V90_PHASE4_RI_SEARCH_ACQUIRED),
		     "Ri acquired") == 0 &&
	      strcmp(v90_phase4_ri_search_result_name(
		     (enum v90_phase4_ri_search_result)99),
		     "unknown") == 0,
	      "Ri result names changed");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "exact PCMU and PCMA sequences", test_exact_sequences },
		{ "transition rejection", test_transition_rejection },
		{ "unframed Ri search", test_unframed_search },
		{ "sampled Ri and exact handoff", test_sampled_sequences },
		{ "UINFO magnitude window", test_level_window },
		{ "reject unrelated signals", test_search_rejects_other_signals },
		{ "validation and transactionality", test_validation },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		printf("[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			printf("[       OK ] %s\n", tests[index].name);
		else {
			printf("[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
