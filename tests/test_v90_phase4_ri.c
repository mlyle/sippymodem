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
		      &search, expected_octet(law, ucode + 1u, true)) ==
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
	{
		struct v90_phase4_ri_search saved = search;

		CHECK(v90_phase4_ri_search_receive(
			      &search,
			      expected_octet(law, ucode + 1u, true)) ==
			      V90_PHASE4_RI_SEARCH_INVALID_SEQUENCE &&
		      memcmp(&search, &saved, sizeof(search)) == 0,
		      "acquired Ri search consumed a malformed character");
	}
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
		      &rx, expected_octet(G711_LAW_PCMA, 90, true)) ==
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
