// SPDX-License-Identifier: BSD-3-Clause

#include "v92_control_target.h"
#include "memory_region.h"

#include <math.h>
#include <stddef.h>

bool v92_control_target(
	enum g711_law law, const struct v92_phase4_demodulator *demodulator,
	int16_t nominal, double observed, int16_t *target)
{
	struct g711_interval interval;
	unsigned int bits, magnitudes, magnitude = 0u, matches = 0u;
	int16_t nominal_target, best;
	double best_error;
	int lower, upper;
	int sign = nominal < 0 ? -1 : 1;

	if (demodulator == NULL || target == NULL || !isfinite(observed) ||
	    demodulator->lu <= 0 ||
	    !g711_quantization_interval(law, demodulator->lu, &interval))
		return false;
	bits = v92_phase4_modulator_bits_per_symbol(demodulator->constellation);
	magnitudes = bits == 0u ? 0u : 1u << (bits - 1u);
	for (unsigned int index = 0u; index < magnitudes; ++index) {
		int16_t level;

		if (v92_phase4_modulator_level(demodulator->constellation,
			    demodulator->lu, (uint8_t)index, nominal < 0,
			    &level) != V92_PHASE4_MODULATOR_OK)
			continue;
		if (level == nominal) {
			magnitude = index;
			++matches;
		}
	}
	if (matches == 0u)
		return false;
	nominal_target = best = g711_quantize(law, nominal);
	if (matches > 1u) {
		*target = best;
		return true;
	}
	best_error = fabs(observed - best);
	lower = interval.lower > 0 ? interval.lower : 1;
	upper = interval.upper;
	/* The signed magnitude is monotone in LU, including G.711 rounding.
	 * Search its crossing of observed rather than scan up to 2048 LU values
	 * per symbol. Invalid low/high endpoints round to zero or clip the outer
	 * constellation; the validated nominal LU lies between them. */
	while (lower <= upper) {
		int lu = lower + (upper - lower) / 2;
		int16_t level;
		double error;

		if (v92_phase4_modulator_level(demodulator->constellation,
			    (int16_t)lu, (uint8_t)magnitude, nominal < 0,
			    &level) != V92_PHASE4_MODULATOR_OK) {
			if (lu < demodulator->lu)
				lower = lu + 1;
			else
				upper = lu - 1;
			continue;
		}
		level = g711_quantize(law, level);
		error = fabs(observed - level);
		/* Prefer the nominal target on ties, then the lower magnitude. */
		if (error < best_error ||
		    (error == best_error && best != nominal_target &&
		     sign * level < sign * best)) {
			best = level;
			best_error = error;
		}
		if (sign * level <= sign * observed)
			lower = lu + 1;
		else
			upper = lu - 1;
	}
	*target = best;
	return true;
}

bool v92_control_adapt(struct qam_equalizer *equalizer,
	enum g711_law law, const struct v92_phase4_demodulator *demodulator,
	int16_t nominal, double adaptation, double *output_bias, bool learn_bias)
{
	int16_t gradient;
	double bias, observed, step = adaptation;
	struct qam_point target;

	if (!qam_equalizer_is_valid(equalizer) || demodulator == NULL ||
	    output_bias == NULL ||
	    !qam_adaptation_is_valid(adaptation) ||
	    memory_regions_overlap(equalizer, sizeof(*equalizer),
		    demodulator, sizeof(*demodulator)) ||
	    memory_regions_overlap(equalizer, sizeof(*equalizer),
		    output_bias, sizeof(*output_bias)) ||
	    memory_regions_overlap(demodulator, sizeof(*demodulator),
		    output_bias, sizeof(*output_bias)) || !isfinite(*output_bias))
		return false;
	bias = *output_bias;
	observed = equalizer->last_output.in_phase + bias;
	if (!v92_control_target(law, demodulator, nominal, observed, &gradient))
		return false;
	if (learn_bias) {
		double power = 1e-12;
		double basis = (double)demodulator->lu * demodulator->lu;

		for (unsigned i = 0; i < equalizer->tap_count; ++i)
			power += qam_point_power(&equalizer->history[i]);
		/* Scale the constant coordinate by LU so its learning time is
		 * comparable to a signal tap, rather than one PCM unit. */
		bias += adaptation * basis / (power + basis) * (gradient - observed);
		step *= power / (power + basis);
		if (!isfinite(bias))
			return false;
	}
	target = (struct qam_point){ .in_phase = gradient - *output_bias };
	if (qam_equalizer_adapt(equalizer, &target, step) != QAM_EQUALIZER_OK)
		return false;
	*output_bias = bias;
	return true;
}
