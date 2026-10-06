// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_SIGN_STATE_FIXTURE_H
#define PCM_SIGN_STATE_FIXTURE_H

#include <pcm_binary_sign.h>
#include <pcm_phase3_j_receiver.h>
#include <string.h>

/* Compare owned fields, without struct padding or unused union storage. */
static inline bool pcm_scramblers_equal(
	const struct v34_scrambler *left, const struct v34_scrambler *right)
{
	return left->history == right->history &&
	       left->polynomial == right->polynomial;
}

static inline bool pcm_binary_sign_states_equal(
	const struct pcm_binary_sign *left, const struct pcm_binary_sign *right)
{
	return pcm_scramblers_equal(&left->scrambler, &right->scrambler) &&
	       left->previous_sign == right->previous_sign;
}

static inline bool v90_jd_parameters_equal(
	const struct v90_jd_parameters *left,
	const struct v90_jd_parameters *right)
{
	return left->data_rate_capability_mask ==
		       right->data_rate_capability_mask &&
	       left->training_constellation == right->training_constellation &&
	       left->rate_renegotiation_constellation ==
		       right->rate_renegotiation_constellation &&
	       left->maximum_spectral_shaping_lookahead ==
		       right->maximum_spectral_shaping_lookahead;
}

static inline bool v92_jd_parameters_equal(
	const struct v92_jd *left, const struct v92_jd *right)
{
	return left->data_rate_capability_mask ==
		       right->data_rate_capability_mask &&
	       left->maximum_spectral_shaping_lookahead ==
		       right->maximum_spectral_shaping_lookahead;
}

static inline bool v92_jp_parameters_equal(
	const struct v92_jp *left, const struct v92_jp *right)
{
	return left->su_phase_extension_q16 == right->su_phase_extension_q16 &&
	       left->training_constellation == right->training_constellation &&
	       left->rate_renegotiation_constellation ==
		       right->rate_renegotiation_constellation;
}

/* Use byte comparisons for fully initialized sentinels with invalid tags. */
static inline bool pcm_j_receivers_equal(
	const struct pcm_phase3_j_receiver *left,
	const struct pcm_phase3_j_receiver *right)
{
	if (!pcm_scramblers_equal(&left->descrambler, &right->descrambler) ||
	    left->protocol != right->protocol || left->law != right->law ||
	    left->stage != right->stage ||
	    left->trn1d_symbols != right->trn1d_symbols ||
	    memcmp(left->frame, right->frame, sizeof(left->frame)) != 0 ||
	    left->training_ucode != right->training_ucode ||
	    left->last_sign != right->last_sign ||
	    left->frame_bits != right->frame_bits ||
	    left->prime_zero_bits != right->prime_zero_bits ||
	    left->prime_candidate != right->prime_candidate)
		return false;
	if (left->protocol == PCM_PROTOCOL_V90)
		return v90_jd_parameters_equal(
			&left->parameters.v90, &right->parameters.v90);
	if (left->protocol == PCM_PROTOCOL_V92)
		return v92_jd_parameters_equal(
			       &left->parameters.v92.jd, &right->parameters.v92.jd) &&
		       v92_jp_parameters_equal(
			       &left->parameters.v92.jp, &right->parameters.v92.jp);
	return false;
}

#endif
