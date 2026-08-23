// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_CHANNEL_CONDITIONER_H
#define V34_CHANNEL_CONDITIONER_H

#include "v34_framing.h"
#include "v34_symbol_power.h"

#include <stdbool.h>

enum v34_channel_conditioner_result {
	V34_CHANNEL_CONDITIONER_OK,
	V34_CHANNEL_CONDITIONER_INVALID_ARGUMENT,
	V34_CHANNEL_CONDITIONER_NUMERIC_FAILURE,
};

/* Immutable receive scaling derived from the peer's selected data mapping.
 * Keeping an owned copy makes each modem instance independent and permits
 * concurrent point conditioning without synchronization. */
struct v34_channel_conditioner {
	struct v34_power_normalizer normalizer;
};

enum v34_channel_conditioner_result
v34_channel_conditioner_init(
	struct v34_channel_conditioner *conditioner,
	const struct v34_power_normalizer *normalizer);

bool v34_channel_conditioner_is_valid(
	const struct v34_channel_conditioner *conditioner);

/* Restore one already equalized, unit-average-power data symbol to precoder
 * grid units. Receive noise may place a point outside the transmitter's hard
 * peak bound, so every finite point remains a valid estimate. Input and output
 * may alias; the output is unchanged on failure. */
enum v34_channel_conditioner_result
v34_channel_condition_point(
	const struct v34_channel_conditioner *conditioner,
	const struct qam_point *normalized_point,
	struct qam_point *grid_estimate);

/* Condition one complete 4-D interval atomically in transmission order. */
enum v34_channel_conditioner_result
v34_channel_condition_interval(
	const struct v34_channel_conditioner *conditioner,
	const struct qam_point
		normalized[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct qam_point
		grid_estimates[V34_2D_SYMBOLS_PER_4D_INTERVAL]);

#endif /* V34_CHANNEL_CONDITIONER_H */
