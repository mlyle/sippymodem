// SPDX-License-Identifier: BSD-3-Clause
#include "v92_erasure.h"

#include "memory_region.h"
#include "sample_math.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <string.h>

enum window_result {
	WINDOW_OK,
	WINDOW_STALE,
	WINDOW_INVALID,
	WINDOW_FALLBACK,
};

_Static_assert(V92_ERASURE_HISTORY > V92_PCM_RX_DELAY_FRAMES + 2, "live trust span");

static bool inverse_is_valid(const struct v92_erasure_inverse *inverse)
{
	if (!inverse->ready || (inverse->polarity != 1 && inverse->polarity != -1) ||
	    !inverse->source.active || !v92_residual_is_valid(&inverse->source))
		return false;
	for (unsigned i = 0; i < V92_RESIDUAL_LAGS; ++i)
		if (!isfinite(inverse->lower[i]) || !isfinite(inverse->upper[i]) ||
		    inverse->lower[i] > inverse->source.history[i] ||
		    inverse->source.history[i] > inverse->upper[i])
			return false;
	return true;
}

static bool inverse_begin(struct v92_erasure_inverse *inverse,
	const struct v92_residual *model, int polarity)
{
	if (!inverse || !model || (polarity != 1 && polarity != -1) ||
	    memory_regions_overlap(inverse, sizeof(*inverse), model, sizeof(*model)) ||
	    !v92_residual_is_valid(model))
		return false;
	struct v92_erasure_inverse next = {
		.source = *model, .polarity = polarity, .ready = true, .rejoined = true,
	};
	for (unsigned i = 0; i < V92_RESIDUAL_LAGS; ++i)
		next.lower[i] = next.upper[i] = model->history[i];
	*inverse = next;
	return true;
}

static bool inverse_step(struct v92_erasure_inverse *inverse,
	int32_t raw, bool received, struct v92_erasure_interval *output)
{
	if (!inverse || !output || !inverse_is_valid(inverse) ||
	    memory_regions_overlap(inverse, sizeof(*inverse), output, sizeof(*output)) ||
	    (received && (raw < INT16_MIN || raw > INT16_MAX)))
		return false;
	struct v92_residual lower = inverse->source, upper = inverse->source;
	/* Fixed finite coefficients make each native multiply/subtract monotone
	 * in every fixed rounding mode. Evaluate both endpoints with the same
	 * native recurrence. These bound program outputs, rather than a separate
	 * real-arithmetic filter. Fast-math and excess precision are unsupported. */
	for (unsigned i = 0; i < inverse->source.count; ++i) {
		unsigned at = (unsigned)ring_index_at_age(inverse->source.cursor,
			V92_RESIDUAL_LAGS, inverse->source.delay[i]);
		bool positive = inverse->source.coefficient[i] >= 0.;
		lower.history[at] = positive ? inverse->upper[at] : inverse->lower[at];
		upper.history[at] = positive ? inverse->lower[at] : inverse->upper[at];
	}
	double lo = v92_residual_output(&lower, received ? raw : (double)INT16_MIN);
	double hi = v92_residual_output(&upper, received ? raw : (double)INT16_MAX);
	double point = v92_residual_output(&inverse->source, received ? raw : 0.);
	if (!isfinite(lo) || !isfinite(hi) || !isfinite(point) || lo > point || point > hi)
		return false;
	struct v92_erasure_interval result = {
		.lower = lo, .upper = hi, .point = point, .received = received,
	};
	double physical_lo = inverse->polarity > 0 ? lo : -hi;
	double physical_hi = inverse->polarity > 0 ? hi : -lo;
	if (received && physical_lo >= INT16_MIN && physical_hi <= INT16_MAX) {
		long a = lrint(physical_lo), b = lrint(physical_hi);
		result.unique = a == b;
		if (result.unique)
			result.rounded = (int16_t)a;
	}
	unsigned cursor = inverse->source.cursor;
	inverse->lower[cursor] = lo;
	inverse->upper[cursor] = hi;
	v92_residual_retain(&inverse->source, point);
	/* Numeric equality alone cannot certify the sign of a rounded zero.
	 * Keep any zero-containing history under bounded ownership. */
	bool exact = true;
	for (unsigned i = 0; i < V92_RESIDUAL_LAGS; ++i)
		exact &= inverse->lower[i] != 0. &&
			!memcmp(inverse->lower + i, inverse->upper + i, sizeof(double)) &&
			!memcmp(inverse->lower + i, inverse->source.history + i, sizeof(double));
	inverse->rejoined = result.rejoined =
		received && exact && v92_residual_is_valid(&inverse->source);
	if (received) {
		if (inverse->received != UINT64_MAX)
			++inverse->received;
	} else if (inverse->missing != UINT64_MAX) {
		++inverse->missing;
	}
	*output = result;
	return true;
}

static bool window_begin(struct v92_erasure_window *window, uint64_t call, uint64_t epoch)
{
	if (!window || !call || !epoch)
		return false;
	*window = (struct v92_erasure_window){ .call = call, .epoch = epoch, .initialized = true };
	return true;
}

static enum window_result window_check(const struct v92_erasure_window *window,
	uint64_t call, uint64_t epoch, uint64_t frame, const bool *output)
{
	if (!window || !output || !window->initialized ||
	    memory_regions_overlap(window, sizeof(*window), output, sizeof(*output)))
		return WINDOW_INVALID;
	if (window->call != call || window->epoch != epoch)
		return WINDOW_STALE;
	if (frame != window->next_frame || frame >= UINT64_MAX / V92_PCM_MAPPING_FRAME_SYMBOLS)
		return WINDOW_INVALID;
	return WINDOW_OK;
}

static void window_mark(struct v92_erasure_window *window, uint64_t frame, bool safe)
{
	struct v92_erasure_slot *slot = &window->slot[frame % V92_ERASURE_HISTORY];
	if (!slot->occupied || slot->frame != frame)
		*slot = (struct v92_erasure_slot){ .frame = frame, .occupied = true, .safe = safe };
	else
		slot->safe &= safe;
}

static enum window_result window_fallback(struct v92_erasure_window *window, bool *output)
{
	window->fallback = true;
	window->pending = false;
	*output = false;
	return WINDOW_FALLBACK;
}

static enum window_result window_prepare(struct v92_erasure_window *window,
	uint64_t call, uint64_t epoch, uint64_t frame, bool usable, bool *reseed)
{
	enum window_result result = window_check(window, call, epoch, frame, reseed);
	if (result != WINDOW_OK)
		return result;
	if (window->fallback)
		return window_fallback(window, reseed);
	if (window->pending)
		return WINDOW_INVALID;
	if (window->active && frame - window->episode_start >= V92_ERASURE_EPISODE_FRAMES)
		return window_fallback(window, reseed);
	struct v92_erasure_window next = *window;
	bool seed = false;
	window_mark(&next, frame, true);
	if (!usable) {
		if (!next.active) {
			if (next.episodes == V92_ERASURE_MAX_EPISODES)
				return window_fallback(window, reseed);
			next.active = true;
			next.episode_start = frame;
			++next.episodes;
		}
		next.dirty = true;
		uint64_t first = frame > V92_PCM_RX_DELAY_FRAMES ? frame - V92_PCM_RX_DELAY_FRAMES : 0;
		for (uint64_t at = first; at <= frame; ++at)
			window_mark(&next, at, false);
	} else if (next.dirty) {
		if (next.reseeds == V92_ERASURE_MAX_RESEEDS)
			return window_fallback(window, reseed);
		seed = true;
		++next.reseeds;
		next.dirty = false;
		window_mark(&next, frame, false);
		window_mark(&next, frame + 1, false);
	}
	next.pending = true;
	*window = next;
	*reseed = seed;
	return WINDOW_OK;
}

static enum window_result window_finish(struct v92_erasure_window *window,
	uint64_t call, uint64_t epoch, uint64_t frame, bool bits_ready,
	bool ambiguous, bool *release)
{
	enum window_result result = window_check(window, call, epoch, frame, release);
	if (result != WINDOW_OK)
		return result;
	if (window->fallback)
		return window_fallback(window, release);
	if (!window->pending || (bits_ready && frame < V92_PCM_RX_DELAY_FRAMES))
		return WINDOW_INVALID;
	bool safe = false;
	if (bits_ready) {
		uint64_t source = frame - V92_PCM_RX_DELAY_FRAMES;
		const struct v92_erasure_slot *slot = &window->slot[source % V92_ERASURE_HISTORY];
		if (!slot->occupied || slot->frame != source)
			return WINDOW_INVALID;
		safe = slot->safe && !ambiguous;
		if (!safe && window->held == V92_ERASURE_MAX_HELD)
			return window_fallback(window, release);
	}
	if (bits_ready && !safe)
		++window->held;
	if (safe)
		window->active = false;
	window->pending = false;
	++window->next_frame;
	*release = safe;
	return WINDOW_OK;
}

static bool erasure_seed(struct v92_pcm_frame_rx *receiver)
{
	if (!v92_pcm_frame_rx_is_valid(receiver) || receiver->kind != V92_PCM_RX_HISTORY ||
	    receiver->observation != V92_PCM_FRAME_CONSTELLATION ||
	    receiver->b1u_frames != V92_B1U_FRAMES ||
	    receiver->post_b1_frames != V92_PCM_RX_DELAY_FRAMES ||
	    receiver->pending_frames != V92_PCM_RX_DELAY_FRAMES)
		return false;
	unsigned states = v34_trellis_state_count(receiver->paths[0].trellis.selection);
	if (!states || 2 * states > V92_PCM_RX_PATHS)
		return false;
	struct v92_pcm_frame_rx_path seed = receiver->paths[0];
	memset(&seed.filter, 0, sizeof(seed.filter));
	seed.metric = 0;
	seed.descrambler.feedback.history = 0;
	seed.ambiguous_frames = (uint16_t)((1u << receiver->pending_frames) - 1u);
	seed.nondata_frames = seed.b1u_code_mismatches = 0;
	seed.rm_frames = seed.rm_prime_frames = 0;
	memset(receiver->paths, 0, sizeof(receiver->paths));
	for (unsigned state = 0; state < states; ++state)
		for (unsigned sign = 0; sign < 2; ++sign) {
			seed.trellis.state = (uint8_t)state;
			seed.differential = (uint8_t)sign;
			receiver->paths[2 * state + sign] = seed;
		}
	receiver->path_count = 2 * states;
	return true;
}

void v92_erasure_reset(struct v92_erasure *owner)
{
	if (!owner)
		return;
	struct v92_erasure next = {
		.call = owner->call,
		.epoch = owner->epoch == UINT64_MAX ? UINT64_MAX : owner->epoch + 1,
		.disabled = owner->disabled || owner->epoch == UINT64_MAX,
		.missing = owner->missing, .uncertain = owner->uncertain, .rejoins = owner->rejoins,
	};
	next.trust.episodes = owner->trust.episodes;
	next.trust.reseeds = owner->trust.reseeds;
	next.trust.held = owner->trust.held;
	*owner = next;
}

bool v92_erasure_is_valid(const struct v92_erasure *owner)
{
	if (!owner || owner->trust.episodes > V92_ERASURE_MAX_EPISODES ||
	    owner->trust.reseeds > V92_ERASURE_MAX_RESEEDS ||
	    owner->trust.held > V92_ERASURE_MAX_HELD ||
	    owner->symbols >= V92_PCM_MAPPING_FRAME_SYMBOLS)
		return false;
	if (!owner->initialized)
		return !owner->bounded;
	if (owner->bounded && !inverse_is_valid(&owner->inverse))
		return false;
	return !owner->disabled && owner->call && owner->epoch && owner->trust.initialized &&
		owner->trust.call == owner->call && owner->trust.epoch == owner->epoch &&
		!owner->trust.pending;
}

bool v92_erasure_is_stable(const struct v92_erasure *owner)
{
	if (!owner)
		return false;
	if (!owner->initialized)
		return true;
	if (owner->bounded || owner->trust.active || owner->trust.dirty ||
	    owner->trust.pending || owner->trust.fallback)
		return false;
	for (unsigned i = 0; i < owner->symbols; ++i)
		if (!owner->usable[i])
			return false;
	return true;
}

bool v92_erasure_correct(struct v92_erasure *owner, struct v92_residual *residual,
	int polarity, int32_t raw, bool available, int32_t *correction, bool *usable)
{
	if (!available && !owner->bounded) {
		if (!inverse_begin(&owner->inverse, residual, polarity))
			return false;
		owner->bounded = true;
	}
	*usable = true;
	if (!owner->bounded)
		return v92_residual_sample(residual, raw, polarity, true, correction);
	struct v92_erasure_interval interval;
	if (!inverse_step(&owner->inverse, raw, available, &interval) ||
	    !v92_residual_is_valid(&owner->inverse.source))
		return false;
	double physical = interval.point * polarity;
	if (available && (physical < INT16_MIN || physical > INT16_MAX))
		return false;
	*usable = interval.unique;
	owner->missing += !available;
	owner->epoch_missing += !available;
	owner->uncertain += available && !interval.unique;
	int16_t value = interval.unique ? interval.rounded : sample_round_s16(physical);
	*correction = raw - (int32_t)value * polarity;
	*residual = owner->inverse.source;
	if (interval.rejoined) {
		owner->bounded = false;
		++owner->rejoins;
	}
	return true;
}

bool v92_erasure_frame_begin(struct v92_erasure *owner,
	struct v92_pcm_frame_rx *decoder, bool full, bool *seed)
{
	return window_prepare(&owner->trust, owner->call, owner->epoch,
		owner->trust.next_frame, full, seed) == WINDOW_OK && (!*seed || erasure_seed(decoder));
}

bool v92_erasure_frame_end(struct v92_erasure *owner,
	bool ready, bool ambiguous, bool *release)
{
	if (window_finish(&owner->trust, owner->call, owner->epoch,
		owner->trust.next_frame, ready, ambiguous, release) != WINDOW_OK)
		return false;
	++owner->frames;
	owner->symbols = 0;
	return true;
}

bool v92_erasure_prime(struct v92_erasure *next, const struct v92_erasure *old)
{
#if defined(__FAST_MATH__) || FLT_RADIX != 2 || DBL_MANT_DIG != 53 || FLT_EVAL_METHOD != 0
	return false;
#endif
	if (!window_begin(&next->trust, next->call, next->epoch))
		return false;
	next->trust.episodes = old->trust.episodes;
	next->trust.reseeds = old->trust.reseeds;
	next->trust.held = old->trust.held;
	for (unsigned i = 0; i < V92_PCM_RX_DELAY_FRAMES; ++i) {
		bool seed, release;
		if (window_prepare(&next->trust, next->call, next->epoch, i, true, &seed) != WINDOW_OK ||
		    seed || window_finish(&next->trust, next->call, next->epoch,
			i, false, false, &release) != WINDOW_OK || release)
			return false;
	}
	return true;
}
