// SPDX-License-Identifier: BSD-3-Clause

#include "v92_su.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

enum {
	/* sqrt(3/2), rounded to Q15. */
	V92_SU_LEVEL_SCALE_Q15 = 40132,
	V92_SU_LEVEL_ROUND_Q15 = 16384,
};

static bool stage_is_valid(enum v92_su_stage stage)
{
	return stage >= V92_SU_JD_RESPONSE && stage <= V92_SU_DONE;
}

static bool post_jp_phase_is_valid(int32_t phase_q16)
{
	return phase_q16 >= -FRACTIONAL_DELAY_HALF_SAMPLE_Q16 &&
	       phase_q16 < FRACTIONAL_DELAY_HALF_SAMPLE_Q16;
}

bool v92_su_is_valid(const struct v92_su *su)
{
	if (su == NULL || !stage_is_valid(su->stage) || su->level <= 0 ||
	    su->pattern_phase >= V92_SU_PATTERN_SYMBOLS)
		return false;
	switch (su->stage) {
	case V92_SU_JD_RESPONSE:
		return su->stage_symbols < V92_SU_JD_RESPONSE_SYMBOLS &&
		       su->phase_q16 == 0 && su->post_jp_phase_q16 == 0;
	case V92_SU_JD_BAR_RESPONSE:
		return su->stage_symbols < V92_SU_JD_BAR_INPUT_SYMBOLS &&
		       su->phase_q16 == 0 && su->post_jp_phase_q16 == 0;
	case V92_SU_JP_WAIT:
		return su->stage_symbols != UINT64_MAX &&
		       su->phase_q16 ==
			       -FRACTIONAL_DELAY_HALF_SAMPLE_Q16 &&
		       su->post_jp_phase_q16 == 0;
	case V92_SU_JP_BAR_RESPONSE:
		return su->stage_symbols < V92_SU_JP_BAR_INPUT_SYMBOLS &&
		       su->phase_q16 ==
			       -FRACTIONAL_DELAY_HALF_SAMPLE_Q16 &&
		       post_jp_phase_is_valid(su->post_jp_phase_q16);
	case V92_SU_TRN1U_READY:
		return su->stage_symbols == 0 &&
		       post_jp_phase_is_valid(su->post_jp_phase_q16) &&
		       su->phase_q16 == su->post_jp_phase_q16;
	case V92_SU_DONE:
		return su->stage_symbols == 0 &&
		       post_jp_phase_is_valid(su->post_jp_phase_q16) &&
		       (su->phase_q16 == 0 ||
			su->phase_q16 == su->post_jp_phase_q16);
	}
	return false;
}

bool v92_su_is_complete(const struct v92_su *su)
{
	return v92_su_is_valid(su) && su->stage == V92_SU_DONE;
}

static int16_t su_level(int16_t lu)
{
	int32_t scaled = ((int32_t)lu * V92_SU_LEVEL_SCALE_Q15 +
		V92_SU_LEVEL_ROUND_Q15) >> 15;

	return scaled > INT16_MAX ? INT16_MAX : (int16_t)scaled;
}

enum v92_su_result v92_su_init(struct v92_su *su, int16_t lu)
{
	struct v92_su initialized;

	if (su == NULL || lu <= 0)
		return V92_SU_INVALID_ARGUMENT;
	initialized = (struct v92_su){
		.stage = V92_SU_JD_RESPONSE,
		.level = su_level(lu),
	};
	if (!v92_su_is_valid(&initialized))
		return V92_SU_INVALID_ARGUMENT;
	*su = initialized;
	return V92_SU_OK;
}

static bool stage_is_inverted(enum v92_su_stage stage)
{
	return stage == V92_SU_JD_BAR_RESPONSE ||
	       stage == V92_SU_JP_BAR_RESPONSE;
}

static int16_t raw_symbol(struct v92_su *su)
{
	static const int8_t signs[V92_SU_PATTERN_SYMBOLS] = {
		1, 0, 1, -1, 0, -1,
	};
	int sign = signs[su->pattern_phase];

	if (stage_is_inverted(su->stage))
		sign = -sign;
	su->pattern_phase++;
	if (su->pattern_phase == V92_SU_PATTERN_SYMBOLS)
		su->pattern_phase = 0;
	return (int16_t)(sign * su->level);
}

static enum v92_su_result advance(struct v92_su *su)
{
	su->stage_symbols++;
	switch (su->stage) {
	case V92_SU_JD_RESPONSE:
		if (su->stage_symbols == V92_SU_JD_RESPONSE_SYMBOLS) {
			su->stage = V92_SU_JD_BAR_RESPONSE;
			su->stage_symbols = 0;
		}
		return V92_SU_OK;
	case V92_SU_JD_BAR_RESPONSE:
		if (su->stage_symbols == V92_SU_JD_BAR_INPUT_SYMBOLS) {
			su->stage = V92_SU_JP_WAIT;
			su->stage_symbols = 0;
			su->phase_q16 =
				-FRACTIONAL_DELAY_HALF_SAMPLE_Q16;
			return V92_SU_REACHED_JP_WAIT;
		}
		return V92_SU_OK;
	case V92_SU_JP_WAIT:
		return V92_SU_OK;
	case V92_SU_JP_BAR_RESPONSE:
		if (su->stage_symbols == V92_SU_JP_BAR_INPUT_SYMBOLS) {
			su->stage = V92_SU_TRN1U_READY;
			su->stage_symbols = 0;
			su->phase_q16 = su->post_jp_phase_q16;
			return V92_SU_REACHED_TRN1U_READY;
		}
		return V92_SU_OK;
	case V92_SU_TRN1U_READY:
		break;
	case V92_SU_DONE:
		break;
	}
	return V92_SU_INVALID_STATE;
}

enum v92_su_result v92_su_next(
	struct v92_su *su, struct fractional_delay *delay,
	int16_t *output)
{
	struct fractional_delay filtered;
	struct v92_su next;
	enum fractional_delay_result filter_result;
	enum v92_su_result result;
	int16_t generated;
	int16_t sample;

	if (su == NULL || delay == NULL || output == NULL)
		return V92_SU_INVALID_ARGUMENT;
	if (!v92_su_is_valid(su) || !fractional_delay_is_valid(delay) ||
	    delay->phase_q16 != su->phase_q16)
		return V92_SU_INVALID_STATE;
	if (su->stage == V92_SU_TRN1U_READY || su->stage == V92_SU_DONE)
		return V92_SU_COMPLETE;
	next = *su;
	sample = raw_symbol(&next);
	result = advance(&next);
	if (result == V92_SU_INVALID_STATE || !v92_su_is_valid(&next))
		return V92_SU_INVALID_STATE;
	filtered = *delay;
	filter_result = fractional_delay_process(
		&filtered, sample, &generated);
	if (filter_result == FRACTIONAL_DELAY_OK &&
	    filtered.phase_q16 != next.phase_q16)
		filter_result = fractional_delay_set_phase(
			&filtered, next.phase_q16);
	if (filter_result != FRACTIONAL_DELAY_OK)
		return V92_SU_FILTER_ERROR;
	*su = next;
	*delay = filtered;
	*output = generated;
	return result;
}

enum v92_su_result v92_su_accept_jp(
	struct v92_su *su, const struct v92_jp *jp)
{
	struct v92_su next;

	if (su == NULL || jp == NULL)
		return V92_SU_INVALID_ARGUMENT;
	if (!v92_su_is_valid(su) || su->stage != V92_SU_JP_WAIT)
		return V92_SU_INVALID_STATE;
	if (!v92_jp_is_valid(jp))
		return V92_SU_INVALID_ARGUMENT;
	next = *su;
	next.post_jp_phase_q16 =
		(int32_t)jp->su_phase_extension_q16 -
		FRACTIONAL_DELAY_HALF_SAMPLE_Q16;
	next.stage = V92_SU_JP_BAR_RESPONSE;
	next.stage_symbols = 0;
	if (!v92_su_is_valid(&next))
		return V92_SU_INVALID_STATE;
	*su = next;
	return V92_SU_OK;
}

enum v92_su_result v92_su_finish(
	struct v92_su *su, struct fractional_delay *delay,
	bool restore_codec_grid)
{
	struct fractional_delay next_delay;
	struct v92_su next;
	int32_t phase_q16;

	if (su == NULL || delay == NULL)
		return V92_SU_INVALID_ARGUMENT;
	if (!v92_su_is_valid(su) ||
	    su->stage != V92_SU_TRN1U_READY ||
	    !fractional_delay_is_valid(delay) ||
	    delay->phase_q16 != su->phase_q16)
		return V92_SU_INVALID_STATE;
	phase_q16 = restore_codec_grid ? 0 : su->post_jp_phase_q16;
	next_delay = *delay;
	if (fractional_delay_set_phase(&next_delay, phase_q16) !=
	    FRACTIONAL_DELAY_OK)
		return V92_SU_FILTER_ERROR;
	next = *su;
	next.stage = V92_SU_DONE;
	next.stage_symbols = 0;
	next.phase_q16 = phase_q16;
	if (!v92_su_is_valid(&next))
		return V92_SU_INVALID_STATE;
	*su = next;
	*delay = next_delay;
	return V92_SU_COMPLETE;
}

bool v92_su_get_phase(const struct v92_su *su, int32_t *phase_q16)
{
	if (!v92_su_is_valid(su) || phase_q16 == NULL)
		return false;
	*phase_q16 = su->phase_q16;
	return true;
}

const char *v92_su_stage_name(enum v92_su_stage stage)
{
	switch (stage) {
	case V92_SU_JD_RESPONSE:
		return "Jd response";
	case V92_SU_JD_BAR_RESPONSE:
		return "inverted Jd response";
	case V92_SU_JP_WAIT:
		return "Jp wait";
	case V92_SU_JP_BAR_RESPONSE:
		return "inverted Jp response";
	case V92_SU_TRN1U_READY:
		return "ready for TRN1u";
	case V92_SU_DONE:
		return "complete";
	}
	return "unknown";
}

const char *v92_su_result_name(enum v92_su_result result)
{
	switch (result) {
	case V92_SU_OK:
		return "ok";
	case V92_SU_REACHED_JP_WAIT:
		return "Jp wait started";
	case V92_SU_REACHED_TRN1U_READY:
		return "TRN1u ready";
	case V92_SU_COMPLETE:
		return "complete";
	case V92_SU_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_SU_INVALID_STATE:
		return "invalid state";
	case V92_SU_FILTER_ERROR:
		return "fractional delay failed";
	}
	return "unknown";
}
