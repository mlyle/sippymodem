// SPDX-License-Identifier: BSD-3-Clause

#include "v92_ja_sync.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool bytes_are_zero(const uint8_t *bytes, size_t count)
{
	for (size_t index = 0; index < count; ++index) {
		if (bytes[index] != 0u)
			return false;
	}
	return true;
}

static bool coding_is_gpa(const struct pcm_binary_sign *coding)
{
	return pcm_binary_sign_is_valid(coding) &&
	       coding->scrambler.polynomial == V34_POLYNOMIAL_GPA;
}

static bool coding_matches(
	const struct pcm_binary_sign *first,
	const struct pcm_binary_sign *second)
{
	return first->scrambler.history == second->scrambler.history &&
	       first->scrambler.polynomial == second->scrambler.polynomial &&
	       first->previous_sign == second->previous_sign;
}

static bool marker_is_valid(const struct v92_ja_sync *sync)
{
	if (sync->marker_count > V92_JA_MARKER_BITS ||
	    sync->marker_index >= V92_JA_MARKER_BITS)
		return false;
	for (size_t index = 0; index < V92_JA_MARKER_BITS; ++index) {
		if (sync->marker_signs[index] > 1u)
			return false;
		if (sync->marker_count < V92_JA_MARKER_BITS &&
		    index >= sync->marker_count && sync->marker_signs[index] != 0u)
			return false;
	}
	return sync->marker_count == V92_JA_MARKER_BITS ||
	       sync->marker_index == 0u;
}

bool v92_ja_sync_is_valid(const struct v92_ja_sync *sync)
{
	if (sync == NULL || !coding_is_gpa(&sync->training) ||
	    !coding_is_gpa(&sync->window_start) || !marker_is_valid(sync))
		return false;
	switch (sync->stage) {
	case V92_JA_SYNC_TRAINING:
		return sync->marker_count == 0u && sync->marker_index == 0u &&
		       bytes_are_zero(
			       sync->marker_signs, sizeof(sync->marker_signs));
	case V92_JA_SYNC_SEARCHING:
		return sync->trained_symbols >=
		       V92_JA_SYNC_MIN_TRAINING_SYMBOLS;
	case V92_JA_SYNC_COMPLETE:
		return sync->trained_symbols >=
			       V92_JA_SYNC_MIN_TRAINING_SYMBOLS &&
		       sync->marker_count == 0u && sync->marker_index == 0u &&
		       bytes_are_zero(
			       sync->marker_signs, sizeof(sync->marker_signs)) &&
		       coding_matches(&sync->training, &sync->window_start);
	}
	return false;
}

enum v92_ja_sync_result v92_ja_sync_init(struct v92_ja_sync *sync)
{
	struct v92_ja_sync initialized = { 0 };

	if (sync == NULL ||
	    pcm_binary_sign_init(
		    &initialized.training, V34_POLYNOMIAL_GPA) !=
		    PCM_BINARY_SIGN_OK ||
	    pcm_binary_sign_init(
		    &initialized.window_start, V34_POLYNOMIAL_GPA) !=
		    PCM_BINARY_SIGN_OK ||
	    !v92_ja_sync_is_valid(&initialized))
		return V92_JA_SYNC_INVALID_ARGUMENT;
	*sync = initialized;
	return V92_JA_SYNC_OK;
}

bool v92_ja_sync_is_complete(const struct v92_ja_sync *sync)
{
	return v92_ja_sync_is_valid(sync) &&
	       sync->stage == V92_JA_SYNC_COMPLETE;
}

enum v92_ja_sync_result v92_ja_sync_train(
	struct v92_ja_sync *sync, uint8_t sign)
{
	struct v92_ja_sync next;

	if (sync == NULL || sign > 1u)
		return V92_JA_SYNC_INVALID_ARGUMENT;
	if (!v92_ja_sync_is_valid(sync) ||
	    sync->stage != V92_JA_SYNC_TRAINING ||
	    sync->trained_symbols == UINT64_MAX)
		return V92_JA_SYNC_INVALID_STATE;
	next = *sync;
	if (pcm_binary_sign_track_scrambler_output(
		    &next.training, sign) != PCM_BINARY_SIGN_OK)
		return V92_JA_SYNC_INVALID_STATE;
	next.trained_symbols++;
	if (!v92_ja_sync_is_valid(&next))
		return V92_JA_SYNC_INVALID_STATE;
	*sync = next;
	return V92_JA_SYNC_OK;
}

enum v92_ja_sync_result v92_ja_sync_begin(struct v92_ja_sync *sync)
{
	struct v92_ja_sync next;

	if (sync == NULL)
		return V92_JA_SYNC_INVALID_ARGUMENT;
	if (!v92_ja_sync_is_valid(sync) ||
	    sync->stage != V92_JA_SYNC_TRAINING ||
	    sync->trained_symbols < V92_JA_SYNC_MIN_TRAINING_SYMBOLS)
		return V92_JA_SYNC_INVALID_STATE;
	next = *sync;
	next.window_start = next.training;
	next.stage = V92_JA_SYNC_SEARCHING;
	if (!v92_ja_sync_is_valid(&next))
		return V92_JA_SYNC_INVALID_STATE;
	*sync = next;
	return V92_JA_SYNC_OK;
}

static bool push_sign(struct v92_ja_sync *sync, uint8_t sign)
{
	size_t write_index;

	if (sync->marker_count == V92_JA_MARKER_BITS) {
		if (pcm_binary_sign_track_scrambler_output(
			    &sync->window_start,
			    sync->marker_signs[sync->marker_index]) !=
		    PCM_BINARY_SIGN_OK)
			return false;
		write_index = sync->marker_index;
		sync->marker_index = (uint8_t)(
			(sync->marker_index + 1u) % V92_JA_MARKER_BITS);
	} else {
		write_index = sync->marker_count;
		sync->marker_count++;
	}
	sync->marker_signs[write_index] = sign;
	return pcm_binary_sign_track_scrambler_output(
		       &sync->training, sign) == PCM_BINARY_SIGN_OK;
}

static bool marker_matches(
	const struct v92_ja_sync *sync,
	struct pcm_binary_sign *after)
{
	struct pcm_binary_sign candidate = sync->window_start;
	unsigned int preamble_errors = 0u;

	if (sync->marker_count != V92_JA_MARKER_BITS || after == NULL)
		return false;
	for (size_t index = 0; index < V92_JA_MARKER_BITS; ++index) {
		uint8_t bit = index + 1u < V92_JA_MARKER_BITS;
		uint8_t predicted;
		uint8_t observed = sync->marker_signs[
			(sync->marker_index + index) % V92_JA_MARKER_BITS];

		if (pcm_binary_sign_encode(
			    &candidate, bit, &predicted) != PCM_BINARY_SIGN_OK)
			return false;
		if (observed == predicted)
			continue;
		if (index >= V92_PCM_PHASE3_JA_PREAMBLE_BITS ||
		    ++preamble_errors > V92_JA_SYNC_MAX_PREAMBLE_ERRORS)
			return false;
	}
	*after = candidate;
	return true;
}

enum v92_ja_sync_result v92_ja_sync_receive(
	struct v92_ja_sync *sync, uint8_t sign)
{
	struct v92_ja_sync next;
	struct pcm_binary_sign after;

	if (sync == NULL || sign > 1u)
		return V92_JA_SYNC_INVALID_ARGUMENT;
	if (!v92_ja_sync_is_valid(sync) ||
	    sync->stage == V92_JA_SYNC_TRAINING)
		return V92_JA_SYNC_INVALID_STATE;
	if (sync->stage == V92_JA_SYNC_COMPLETE)
		return V92_JA_SYNC_ALREADY_COMPLETE;
	next = *sync;
	if (!push_sign(&next, sign))
		return V92_JA_SYNC_INVALID_STATE;
	if (marker_matches(&next, &after)) {
		next.training = after;
		next.window_start = after;
		next.marker_count = 0u;
		next.marker_index = 0u;
		memset(next.marker_signs, 0, sizeof(next.marker_signs));
		next.stage = V92_JA_SYNC_COMPLETE;
	}
	if (!v92_ja_sync_is_valid(&next))
		return V92_JA_SYNC_INVALID_STATE;
	*sync = next;
	return next.stage == V92_JA_SYNC_COMPLETE ?
		       V92_JA_SYNC_SYNCHRONIZED : V92_JA_SYNC_OK;
}

bool v92_ja_sync_handoff(
	const struct v92_ja_sync *sync,
	struct pcm_binary_sign *coding)
{
	if (!v92_ja_sync_is_complete(sync) || coding == NULL)
		return false;
	*coding = sync->training;
	return true;
}

const char *v92_ja_sync_result_name(enum v92_ja_sync_result result)
{
	switch (result) {
	case V92_JA_SYNC_OK:
		return "ok";
	case V92_JA_SYNC_SYNCHRONIZED:
		return "synchronized";
	case V92_JA_SYNC_ALREADY_COMPLETE:
		return "already complete";
	case V92_JA_SYNC_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_JA_SYNC_INVALID_STATE:
		return "invalid state";
	}
	return "unknown";
}
