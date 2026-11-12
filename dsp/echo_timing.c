// SPDX-License-Identifier: BSD-3-Clause

#include "echo_timing.h"

#include "integer_math.h"
#include "wide_uint.h"

#include <limits.h>
#include <string.h>

/* Predictions are differences of unclipped int16 samples. Even two
 * UINT16_MAX-sample windows keep cross below 2^48, energy below 2^49,
 * and twice the cancellation improvement below 2^52. Only confidence
 * products need 128 bits; evidence differences fit signed 64 bits. */
_Static_assert(ECHO_TIMING_WINDOW <= UINT16_MAX,
	       "echo timing evidence must fit its observation counter");

static bool prediction_is_qualified(
	int64_t cross, uint64_t energy, uint64_t input,
	unsigned samples, unsigned strength)
{
	struct wide_uint explained, required;

	if (cross <= 0 || energy == 0 || (uint64_t)cross * 2u < energy)
		return false;
	wide_multiply_u32(wide_product_u64(cross, cross), samples, &explained);
	wide_multiply_u32(wide_product_u64(input, energy), strength, &required);
	return wide_compare(explained, required) >= 0;
}

static bool prediction_is_harmful(
	int64_t cross, uint64_t energy, uint64_t input,
	unsigned samples, unsigned strength)
{
	/* Subtraction changes energy by prediction_energy - 2 * cross.
	 * Require that increase to exceed the uncertainty of the cross term;
	 * its factor of two contributes four to the variance bound. */
	int64_t excess = (int64_t)energy - 2 * cross;
	struct wide_uint increase, required;

	if (energy == 0 || excess <= 0)
		return false;
	wide_multiply_u32(wide_product_u64((uint64_t)excess, (uint64_t)excess),
		samples, &increase);
	wide_multiply_u32(wide_product_u64(input, energy), strength, &required);
	required = wide_shift_left(required, 2);
	return wide_compare(increase, required) >= 0;
}

static bool timing_is_qualified(
	const struct echo_timing_evidence *e,
	unsigned samples, unsigned strength)
{
	/* Require useful cancellation as well as correlation. Compare against
	 * the current delay on the same input, including the peer's signal.
	 * Strength is sample_count * r-squared. All monitors use 9 in
	 * each window and 36 together; short windows require material returns
	 * too. Agreement and a half-prediction-energy improvement are
	 * required so unrelated input cannot win just by choosing another bin. */
	int64_t improvement = 2 * (e->cross - e->current_cross) -
		((int64_t)e->energy - (int64_t)e->current_energy);

	return prediction_is_qualified(e->cross, e->energy, e->input,
				       samples, strength) &&
	       improvement > 0 && (uint64_t)improvement * 2 >= e->energy;
}

static bool finish_timing_window(struct echo_timing *timing,
	struct echo_timing_model *model, struct echo_timing_window *t,
	unsigned samples)
{
	const unsigned bins = ECHO_TIMING_BINS;
	const unsigned radius = ECHO_TIMING_RADIUS;
	unsigned best = radius;
	for (unsigned candidate = 0; candidate < bins; ++candidate)
		if (2 * t->cross[candidate] - (int64_t)t->energy[candidate] >
		    2 * t->cross[best] - (int64_t)t->energy[best])
			best = candidate;
	struct echo_timing_evidence evidence = {
		.cross = t->cross[best], .current_cross = t->cross[radius],
		.energy = t->energy[best], .current_energy = t->energy[radius],
		.input = t->input,
	};
	struct echo_timing_evidence combined = {
		.cross = evidence.cross + t->first.cross,
		.current_cross = evidence.current_cross + t->first.current_cross,
		.energy = evidence.energy + t->first.energy,
		.current_energy = evidence.current_energy + t->first.current_energy,
		.input = evidence.input + t->first.input,
	};
	bool qualified = timing_is_qualified(&evidence, samples, 9);
	if (t->second) {
		/* Weak or suspended models need two fresh supported windows to
		 * restore subtraction. A strong model enters the same policy only
		 * after both windows and their sum show harmful cancellation. */
		if (model->require_support || model->suspended ||
		    (prediction_is_harmful(t->first.current_cross,
			    t->first.current_energy, t->first.input, samples, 9) &&
		     prediction_is_harmful(evidence.current_cross,
			    evidence.current_energy, evidence.input, samples, 9) &&
		     prediction_is_harmful(combined.current_cross,
			    combined.current_energy, combined.input, 2 * samples, 36)))
			model->suspended =
				!prediction_is_qualified(t->first.current_cross,
					t->first.current_energy, t->first.input, samples, 9) ||
				!prediction_is_qualified(evidence.current_cross,
					evidence.current_energy, evidence.input, samples, 9) ||
				!prediction_is_qualified(combined.current_cross,
					combined.current_energy, combined.input, 2 * samples, 36);
	} else if (model->require_support &&
		   !prediction_is_qualified(evidence.current_cross,
			   evidence.current_energy, evidence.input, samples, 9)) {
		/* The first unsupported window suspends a marginal model; one
		 * supported window cannot restore an already suspended model. */
		model->suspended = true;
	}
	/* Short windows require a material return: 1/32 of received energy
	 * for fast tracking, 1/128 for intermediate tracking. The long monitor
	 * keeps its sensitivity to weaker returns. All still require the same
	 * independent-window agreement and combined confidence above. */
	unsigned material_scale = samples == ECHO_TIMING_FAST_WINDOW ? 32 : 128;
	bool material_return =
		scaled_u64_at_least(evidence.energy, material_scale, evidence.input) &&
		scaled_u64_at_least(t->first.energy, material_scale, t->first.input);
	if ((samples == ECHO_TIMING_WINDOW || material_return) &&
	    t->second && t->first_qualified && qualified && best == t->candidate) {
		/* Monitor the frozen model being subtracted. Its replacement's
		 * acquisition window must keep its independent training state. */
		/* Two disjoint windows must choose the same useful delay, and
		 * the combined normalized correlation must be stronger still. */
		if (timing_is_qualified(&combined, 2 * samples, 36) &&
		    model->retime(model->owner, (int)best - (int)radius)) {
			model->suspended = false;
			echo_timing_reset(timing);
			return true;
		}
	}
	/* Each completed window is disjoint from its predecessor. Retain it
	 * for the next comparison even when it just served as the second one;
	 * fixed pairs otherwise discard agreement across a pair boundary. */
	t->second = true;
	t->first = evidence;
	t->candidate = best;
	t->first_qualified = qualified;
	t->input = 0;
	memset(t->cross, 0, sizeof(t->cross));
	memset(t->energy, 0, sizeof(t->energy));
	return false;
}

static void accumulate_timing_window(struct echo_timing_window *to,
	const struct echo_timing_window *from)
{
	/* Fold completed blocks before clearing them. Each input contributes
	 * once to every scale without another per-sample correlation scan. */
	to->input += from->input;
	for (unsigned i = 0; i < ECHO_TIMING_BINS; i++) {
		to->cross[i] += from->cross[i];
		to->energy[i] += from->energy[i];
	}
}

/* A ring window is at most two contiguous, reverse-ordered spans. */
static void accumulate_prediction_span(
	struct echo_timing_window *window, unsigned first,
	const int32_t *end, unsigned count, int16_t input)
{
	for (unsigned i = 0; i < count; ++i) {
		int32_t predicted = *--end;
		window->cross[first + i] += (int64_t)input * predicted;
		window->energy[first + i] += (int64_t)predicted * predicted;
	}
}

void echo_timing_observe(struct echo_timing *t,
	struct echo_timing_model *model, int16_t received, int16_t cleaned)
{
	const unsigned bins = ECHO_TIMING_BINS;

	if (cleaned == INT16_MIN || cleaned == INT16_MAX ||
	    received == INT16_MIN || received == INT16_MAX) {
		echo_timing_reset(t);
		return;
	}
	if (t->fill == 0)
		memset(t, 0, sizeof(*t));
	unsigned index = t->write;

	t->prediction[index] = (int32_t)received - cleaned;
	int16_t input = t->received[t->received_write];
	t->received[t->received_write] = received;
	if (++t->received_write == ECHO_TIMING_RADIUS)
		t->received_write = 0;
	t->write = (index + 1) % bins;
	if (t->fill < bins && ++t->fill < bins)
		return;

	t->fast.input += (int32_t)input * input;
	unsigned split = index + 1u;
	accumulate_prediction_span(&t->fast, 0, t->prediction + split, split, input);
	accumulate_prediction_span(&t->fast, split, t->prediction + bins, bins - split, input);
	if (++t->count % ECHO_TIMING_FAST_WINDOW != 0)
		return;
	accumulate_timing_window(&t->medium, &t->fast);
	if (finish_timing_window(t, model, &t->fast, ECHO_TIMING_FAST_WINDOW))
		return;
	if (t->count % ECHO_TIMING_MEDIUM_WINDOW != 0)
		return;
	accumulate_timing_window(&t->slow, &t->medium);
	if (finish_timing_window(t, model, &t->medium, ECHO_TIMING_MEDIUM_WINDOW))
		return;
	if (t->count == ECHO_TIMING_WINDOW) {
		t->count = 0;
		finish_timing_window(t, model, &t->slow,
				     ECHO_TIMING_WINDOW);
	}
}
