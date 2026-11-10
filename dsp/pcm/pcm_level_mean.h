// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_LEVEL_MEAN_H
#define PCM_LEVEL_MEAN_H

/* One six-symbol PCM frame of matched-level averaging. Owners supply finite
 * values and retain their own initialization, polarity and amplitude bounds. */
static inline double pcm_level_mean(double previous, double level)
{
	return previous + (level - previous) / 6.0;
}

#endif
