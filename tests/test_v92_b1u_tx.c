// SPDX-License-Identifier: BSD-3-Clause

#include <v92_b1u_tx.h>
#include <v92_cpd_direct.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

static bool mapping(
	struct v92_pcm_mapping *mapping, enum g711_law law, uint8_t rate)
{
	struct v92_cpd_parameters cpd;

	return v92_cpd_direct_init(&cpd, law, rate, (rate & 1u) != 0u) ==
		       V92_CPD_OK &&
	       v92_pcm_mapping_init(mapping, law, &cpd) == V92_PCM_MAPPING_OK;
}

static bool exercise(enum g711_law law, uint8_t rate)
{
	struct v92_pcm_mapping selected;
	struct v92_pcm_frame_tx oracle;
	struct v92_pcm_frame_tx handoff;
	struct v92_b1u_tx tx;
	uint8_t ones[V92_PCM_MAPPING_MAX_BITS];
	uint16_t emitted = 0u;
	int16_t sample = 0;

	CHECK(mapping(&selected, law, rate) &&
	      v92_pcm_frame_tx_init(&oracle, &selected) == V92_PCM_FRAME_OK &&
	      v92_b1u_tx_init(&tx, &selected, 8192) == V92_B1U_TX_OK,
	      "B1u did not initialize law=%u rate=%u", law, rate);
	memset(ones, 1, selected.data_bits_per_frame);
	for (unsigned int frame_index = 0u;
	     frame_index < V92_B1U_FRAME_COUNT; ++frame_index) {
		struct v92_pcm_frame frame;

		CHECK(v92_pcm_frame_encode(
			      &oracle, ones, selected.data_bits_per_frame,
			      &frame) == V92_PCM_FRAME_OK,
		      "oracle frame stopped law=%u rate=%u frame=%u",
		      law, rate, frame_index);
		for (size_t symbol = 0u;
		     symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
			int16_t expected;

			CHECK(v92_pcm_frame_scale(
				      &selected, 8192, frame.v[symbol],
				      &expected) == V92_PCM_FRAME_OK &&
			      v92_b1u_tx_next(&tx, &sample) == V92_B1U_TX_OK &&
			      sample == expected,
			      "B1u symbol differed law=%u rate=%u symbol=%u",
			      law, rate, emitted);
			emitted++;
		}
	}
	CHECK(emitted == V92_B1U_SYMBOL_COUNT &&
	      v92_b1u_tx_is_complete(&tx) &&
	      !v92_b1u_tx_has_failed(&tx) &&
	      v92_b1u_tx_handoff(&tx, &handoff) == V92_B1U_TX_OK &&
	      memcmp(&handoff, &oracle, sizeof(handoff)) == 0,
	      "B1u handoff differed law=%u rate=%u", law, rate);
	sample = 1234;
	CHECK(v92_b1u_tx_next(&tx, &sample) == V92_B1U_TX_COMPLETE &&
	      sample == 1234,
	      "complete B1u consumed output law=%u rate=%u", law, rate);
	return true;
}

static bool test_rate_matrix(void)
{
	for (unsigned int law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		for (uint8_t rate = 1u;
		     rate <= V92_CPD_MAX_DATA_RATE_NUMBER; ++rate) {
			if (!exercise((enum g711_law)law, rate))
				return false;
		}
	}
	return true;
}

static bool test_contracts(void)
{
	static const char *const names[] = {
		"ok", "complete", "failed", "invalid argument",
		"invalid parameters", "invalid state", "numeric failure",
	};
	struct v92_pcm_mapping selected;
	struct v92_b1u_tx tx;
	struct v92_b1u_tx untouched;
	struct v92_b1u_tx saved;
	struct v92_pcm_frame_tx handoff;
	struct v92_pcm_frame_tx handoff_saved;
	int16_t sample = 4321;

	CHECK(mapping(&selected, G711_LAW_PCMU, 4u),
	      "contract mapping did not initialize");
	memset(&untouched, 0x5a, sizeof(untouched));
	saved = untouched;
	CHECK(v92_b1u_tx_init(NULL, &selected, 8192) ==
		      V92_B1U_TX_INVALID_ARGUMENT &&
	      v92_b1u_tx_init(&untouched, NULL, 8192) ==
		      V92_B1U_TX_INVALID_ARGUMENT &&
	      v92_b1u_tx_init(&untouched, &selected, 0) ==
		      V92_B1U_TX_INVALID_PARAMETERS &&
	      memcmp(&untouched, &saved, sizeof(saved)) == 0 &&
	      v92_b1u_tx_init(&tx, &selected, 8192) == V92_B1U_TX_OK,
	      "B1u initialization was not transactional");
	memset(&handoff, 0x6b, sizeof(handoff));
	handoff_saved = handoff;
	CHECK(v92_b1u_tx_next(NULL, &sample) ==
		      V92_B1U_TX_INVALID_ARGUMENT &&
	      v92_b1u_tx_next(&tx, NULL) == V92_B1U_TX_INVALID_ARGUMENT &&
	      v92_b1u_tx_handoff(&tx, &handoff) ==
		      V92_B1U_TX_INVALID_STATE &&
	      memcmp(&handoff, &handoff_saved, sizeof(handoff)) == 0,
	      "rejected B1u operation changed output");
	saved = tx;
	tx.frame_tx.state.differential = 2u;
	CHECK(v92_b1u_tx_next(&tx, &sample) == V92_B1U_TX_INVALID_STATE &&
	      sample == 4321 && !v92_b1u_tx_is_valid(&tx),
	      "invalid B1u state consumed output");
	tx = saved;
	tx.failed = true;
	CHECK(v92_b1u_tx_has_failed(&tx) &&
	      v92_b1u_tx_next(&tx, &sample) == V92_B1U_TX_FAILED,
	      "failed B1u was not stable");
	for (size_t index = 0u; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      v92_b1u_tx_result_name(
				      (enum v92_b1u_tx_result)index),
			      names[index]) == 0,
		      "B1u result name %zu differed", index);
	CHECK(strcmp(
		      v92_b1u_tx_result_name((enum v92_b1u_tx_result)99),
		      "unknown") == 0 &&
	      !v92_b1u_tx_is_valid(NULL) &&
	      !v92_b1u_tx_is_complete(NULL) &&
	      !v92_b1u_tx_has_failed(NULL),
	      "B1u validation contracts differed");
	return true;
}

int main(void)
{
	return test_rate_matrix() && test_contracts() ? 0 : 1;
}
