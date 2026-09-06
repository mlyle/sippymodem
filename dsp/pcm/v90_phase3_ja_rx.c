// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase3_ja_rx.h"

#include "memory_region.h"
#include "v34_constellation.h"
#include "v34_training_sequence.h"

static bool search_is_initial(const struct v90_ja_search *search)
{
	return search->descrambler.history == 0 &&
	       search->descrambler.polynomial == V34_POLYNOMIAL_GPA &&
	       search->bit_index == 0 && search->warmup_bits == 0 &&
	       search->consecutive_ones == 0 &&
	       !search->has_previous_rotation && !search->collecting &&
	       !search->complete;
}

bool v90_phase3_ja_rx_is_valid(const struct v90_phase3_ja_rx *rx)
{
	if (rx == NULL || !v34_precoder_trainer_is_valid(&rx->precoder) ||
	    !v90_ja_search_is_valid(&rx->search))
		return false;
	if (rx->precoder.observed_symbols <= V34_TRAINING_SEQUENCE_MIN_SYMBOLS)
		return search_is_initial(&rx->search);
	return rx->search.has_previous_rotation;
}

bool v90_phase3_ja_rx_is_complete(const struct v90_phase3_ja_rx *rx)
{
	return v90_phase3_ja_rx_is_valid(rx) && rx->search.complete;
}

enum v90_phase3_ja_rx_result v90_phase3_ja_rx_init(
	struct v90_phase3_ja_rx *rx)
{
	struct v90_phase3_ja_rx initialized = { 0 };

	if (rx == NULL)
		return V90_PHASE3_JA_RX_INVALID_ARGUMENT;
	if (v34_precoder_trainer_init(&initialized.precoder) !=
		    V34_PRECODER_TRAINER_OK ||
	    v90_ja_search_init(&initialized.search) != V90_JA_SEARCH_OK)
		return V90_PHASE3_JA_RX_NUMERIC_FAILURE;
	*rx = initialized;
	return V90_PHASE3_JA_RX_OK;
}

enum v90_phase3_ja_rx_result v90_phase3_ja_rx_observe(
	struct v90_phase3_ja_rx *rx, const struct qam_point *decision)
{
	struct v90_phase3_ja_rx next;
	struct qam_point sliced;
	uint8_t quarter_point;
	uint8_t rotation;
	enum v90_ja_search_result result = V90_JA_SEARCH_OK;

	if (!v90_phase3_ja_rx_is_valid(rx) || !point_is_finite(decision) ||
	    memory_regions_overlap(rx, sizeof(*rx), decision, sizeof(*decision)))
		return V90_PHASE3_JA_RX_INVALID_ARGUMENT;
	if (rx->search.complete)
		return V90_PHASE3_JA_RX_COMPLETE;
	if (rx->precoder.observed_symbols == UINT32_MAX)
		return V90_PHASE3_JA_RX_OUT_OF_RANGE;
	next = *rx;
	if (v34_training_constellation_slice(
		    V34_CONSTELLATION_4_POINT, decision, &quarter_point,
		    &rotation) != V34_CONSTELLATION_OK ||
	    v34_training_constellation_point(
		    V34_CONSTELLATION_4_POINT, quarter_point, rotation,
		    &sliced) != V34_CONSTELLATION_OK ||
	    v34_precoder_trainer_observe(
		    &next.precoder, decision, &sliced) !=
		    V34_PRECODER_TRAINER_OK)
		return V90_PHASE3_JA_RX_NUMERIC_FAILURE;
	if (next.precoder.observed_symbols > V34_TRAINING_SEQUENCE_MIN_SYMBOLS)
		result = v90_ja_search_receive(&next.search, rotation);
	if ((result != V90_JA_SEARCH_OK && result != V90_JA_SEARCH_DETECTED &&
	     result != V90_JA_SEARCH_REJECTED) ||
	    !v90_phase3_ja_rx_is_valid(&next))
		return V90_PHASE3_JA_RX_NUMERIC_FAILURE;
	*rx = next;
	if (result == V90_JA_SEARCH_DETECTED)
		return V90_PHASE3_JA_RX_DETECTED;
	return result == V90_JA_SEARCH_REJECTED ? V90_PHASE3_JA_RX_REJECTED :
		V90_PHASE3_JA_RX_OK;
}

enum v90_phase3_ja_rx_result v90_phase3_ja_rx_handoff(
	const struct v90_phase3_ja_rx *rx,
	struct v90_phase3_ja_handoff *handoff)
{
	struct v90_phase3_ja_handoff generated;

	if (!v90_phase3_ja_rx_is_valid(rx) || handoff == NULL ||
	    memory_regions_overlap(rx, sizeof(*rx), handoff, sizeof(*handoff)))
		return V90_PHASE3_JA_RX_INVALID_ARGUMENT;
	if (!rx->search.complete)
		return V90_PHASE3_JA_RX_INVALID_STATE;
	if (v34_precoder_trainer_finish(&rx->precoder, &generated.precoder) !=
		    V34_PRECODER_TRAINER_OK ||
	    !v90_ja_search_parameters(&rx->search, &generated.dil))
		return V90_PHASE3_JA_RX_NUMERIC_FAILURE;
	*handoff = generated;
	return V90_PHASE3_JA_RX_OK;
}
