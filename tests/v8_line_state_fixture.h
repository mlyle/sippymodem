// SPDX-License-Identifier: BSD-3-Clause

#ifndef V8_LINE_STATE_FIXTURE_H
#define V8_LINE_STATE_FIXTURE_H

#include <v8_line_conditioner.h>
#include <string.h>

static inline bool v8_line_blocks_equal(
	const struct v8_line_conditioned_block *left,
	const struct v8_line_conditioned_block *right)
{
	return memcmp(left->samples, right->samples, sizeof(left->samples)) == 0 &&
	       left->gain_reset == right->gain_reset;
}

static inline bool v8_line_states_equal(
	const struct v8_line_conditioner *left,
	const struct v8_line_conditioner *right)
{
	return memcmp(left->entrance_history, right->entrance_history,
		      sizeof(left->entrance_history)) == 0 &&
	       memcmp(left->input_queue, right->input_queue,
		      sizeof(left->input_queue)) == 0 &&
	       memcmp(left->power_history, right->power_history,
		      sizeof(left->power_history)) == 0 &&
	       left->filtered_energy == right->filtered_energy &&
	       left->gain == right->gain && left->gain_delta == right->gain_delta &&
	       left->post_filter_word == right->post_filter_word &&
	       left->power_index == right->power_index &&
	       left->input_count == right->input_count &&
	       left->overflow_count == right->overflow_count &&
	       left->role == right->role && left->gain_mode == right->gain_mode;
}

#endif
