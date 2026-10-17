// SPDX-License-Identifier: BSD-3-Clause

#include "echo_timing.h"

#include "wide_uint.h"

#include <limits.h>
#include <string.h>

/* Compare value * scale >= bound without overflowing the energy sum. */
static bool scaled_energy_at_least(uint64_t value, unsigned scale, uint64_t bound)
{
	return value >= bound / scale + (bound % scale != 0);
}

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

/* Sign-extend signed evidence into a two's-complement wide value. The
 * differences below fit in signed 128 bits even at the 64-bit boundaries. */
static struct wide_uint signed_evidence(int64_t value)
{
	return (struct wide_uint){ value < 0 ? UINT64_MAX : 0, (uint64_t)value };
}

static bool evidence_is_positive(struct wide_uint value)
{
	return (value.high >> 63) == 0 && (value.high != 0 || value.low != 0);
}

static bool prediction_is_harmful(
	int64_t cross, uint64_t energy, uint64_t input,
	unsigned samples, unsigned strength)
{
	/* Subtraction changes energy by prediction_energy - 2 * cross.
	 * Require that increase to exceed the uncertainty of the cross term;
	 * its factor of two contributes four to the variance bound. */
	struct wide_uint excess = wide_subtract(wide_from_u64(energy),
		wide_shift_left(signed_evidence(cross), 1));
	struct wide_uint increase, required;

	if (energy == 0 || !evidence_is_positive(excess))
		return false;
	wide_multiply_u32(wide_multiply(excess, excess), samples, &increase);
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
	struct wide_uint improvement = wide_subtract(wide_shift_left(
		wide_subtract(signed_evidence(e->cross), signed_evidence(e->current_cross)), 1),
		wide_subtract(wide_from_u64(e->energy), wide_from_u64(e->current_energy)));

	return prediction_is_qualified(e->cross, e->energy, e->input,
				       samples, strength) &&
	       evidence_is_positive(improvement) &&
	       wide_compare(wide_shift_left(improvement, 1), wide_from_u64(e->energy)) >= 0;
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
	bool qualified = timing_is_qualified(&evidence, samples, 9);
	if (model->require_support &&
	    !prediction_is_qualified(evidence.current_cross,
		evidence.current_energy, evidence.input,
		samples, 9))
		model->suspended = true;
	if (t->second &&
	    prediction_is_harmful(t->first.current_cross,
		    t->first.current_energy, t->first.input, samples, 9) &&
	    prediction_is_harmful(evidence.current_cross,
		    evidence.current_energy, evidence.input, samples, 9) &&
	    prediction_is_harmful(
		    evidence.current_cross + t->first.current_cross,
		    evidence.current_energy + t->first.current_energy,
		    evidence.input + t->first.input, 2 * samples, 36))
		model->suspended = true;
	if ((model->require_support || model->suspended) && t->second) {
		/* A marginal or harmful prediction needs fresh support in data.
		 * Reuse the timing monitor's independent confidence bounds at the
		 * current delay. One unsupported window suspends subtraction;
		 * two fresh windows are required to restore it. Keep testing the
		 * frozen prediction while bypassed. */
		model->suspended =
			!prediction_is_qualified(t->first.current_cross,
				t->first.current_energy, t->first.input,
				samples, 9) ||
			!prediction_is_qualified(evidence.current_cross,
				evidence.current_energy, evidence.input,
				samples, 9) ||
			!prediction_is_qualified(
				evidence.current_cross + t->first.current_cross,
				evidence.current_energy + t->first.current_energy,
				evidence.input + t->first.input,
				2 * samples, 36);
	}
	/* Short windows require a material return: 1/32 of received energy
	 * for fast tracking, 1/128 for intermediate tracking. The long monitor
	 * keeps its sensitivity to weaker returns. All still require the same
	 * independent-window agreement and combined confidence above. */
	unsigned material_scale = samples == ECHO_TIMING_FAST_WINDOW ? 32 : 128;
	bool material_return =
		scaled_energy_at_least(evidence.energy, material_scale, evidence.input) &&
		scaled_energy_at_least(t->first.energy, material_scale, t->first.input);
	if ((samples == ECHO_TIMING_WINDOW || material_return) &&
	    t->second && t->first_qualified && qualified && best == t->candidate) {
		struct echo_timing_evidence combined = {
			.cross = evidence.cross + t->first.cross,
			.current_cross = evidence.current_cross + t->first.current_cross,
			.energy = evidence.energy + t->first.energy,
			.current_energy = evidence.current_energy + t->first.current_energy,
			.input = evidence.input + t->first.input,
		};
		/* Monitor the frozen model being subtracted. Its replacement's
		 * acquisition window must keep its independent training state. */
		/* Two disjoint windows must choose the same useful delay, and
		 * the combined normalized correlation must be stronger still. */
		if (timing_is_qualified(&combined, 2 * samples, 36) &&
		    model->retime(model->owner, (int)best - (int)radius)) {
			model->suspended = false;
			memset(timing, 0, sizeof(*timing));
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
	t->count = 0;
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
	to->count += from->count;
	for (unsigned i = 0; i < ECHO_TIMING_BINS; i++) {
		to->cross[i] += from->cross[i];
		to->energy[i] += from->energy[i];
	}
}

void echo_timing_observe(struct echo_timing *t,
	struct echo_timing_model *model, int16_t received, int16_t cleaned)
{
	const unsigned bins = ECHO_TIMING_BINS;
	const unsigned radius = ECHO_TIMING_RADIUS;
	unsigned index = t->write;

	if (cleaned == INT16_MIN || cleaned == INT16_MAX ||
	    received == INT16_MIN || received == INT16_MAX) {
		memset(t, 0, sizeof(*t));
		return;
	}
	t->prediction[index] = (int32_t)received - cleaned;
	t->received[index] = received;
	t->write = (index + 1) % bins;
	if (t->fill < bins && ++t->fill < bins)
		return;
	int16_t input = t->received[(index + bins - radius) % bins];

	t->fast.input += (int32_t)input * input;
	for (unsigned candidate = 0; candidate < bins; ++candidate) {
		int32_t predicted = t->prediction[index];
		int64_t cross = (int64_t)input * predicted;
		uint64_t energy = (int64_t)predicted * predicted;

		t->fast.cross[candidate] += cross;
		t->fast.energy[candidate] += energy;
		index = index == 0 ? bins - 1 : index - 1;
	}
	if (++t->fast.count != ECHO_TIMING_FAST_WINDOW)
		return;
	accumulate_timing_window(&t->medium, &t->fast);
	if (finish_timing_window(t, model, &t->fast, ECHO_TIMING_FAST_WINDOW))
		return;
	if (t->medium.count != ECHO_TIMING_MEDIUM_WINDOW)
		return;
	accumulate_timing_window(&t->slow, &t->medium);
	if (finish_timing_window(t, model, &t->medium, ECHO_TIMING_MEDIUM_WINDOW))
		return;
	if (t->slow.count == ECHO_TIMING_WINDOW)
		finish_timing_window(t, model, &t->slow,
				     ECHO_TIMING_WINDOW);
}
