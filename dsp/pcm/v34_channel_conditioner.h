// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_CHANNEL_CONDITIONER_H
#define SIPPYMODEM_V34_CHANNEL_CONDITIONER_H

#include "v34_framing.h"
#include "v34_symbol_power.h"

#include <stdbool.h>

enum sippymodem_v34_channel_conditioner_result {
	SIPPYMODEM_V34_CHANNEL_CONDITIONER_OK,
	SIPPYMODEM_V34_CHANNEL_CONDITIONER_INVALID_ARGUMENT,
	SIPPYMODEM_V34_CHANNEL_CONDITIONER_NUMERIC_FAILURE,
};

/* Immutable receive scaling derived from the peer's selected data mapping.
 * Keeping an owned copy makes each modem instance independent and permits
 * concurrent point conditioning without synchronization. */
struct sippymodem_v34_channel_conditioner {
	struct sippymodem_v34_power_normalizer normalizer;
};

enum sippymodem_v34_channel_conditioner_result
sippymodem_v34_channel_conditioner_init(
	struct sippymodem_v34_channel_conditioner *conditioner,
	const struct sippymodem_v34_power_normalizer *normalizer);

bool sippymodem_v34_channel_conditioner_is_valid(
	const struct sippymodem_v34_channel_conditioner *conditioner);

/* Restore one already equalized, unit-average-power data symbol to precoder
 * grid units. Receive noise may place a point outside the transmitter's hard
 * peak bound, so every finite point remains a valid estimate. Input and output
 * may alias; the output is unchanged on failure. */
enum sippymodem_v34_channel_conditioner_result
sippymodem_v34_channel_condition_point(
	const struct sippymodem_v34_channel_conditioner *conditioner,
	const struct sippymodem_qam_point *normalized_point,
	struct sippymodem_qam_point *grid_estimate);

/* Condition one complete 4-D interval atomically in transmission order. */
enum sippymodem_v34_channel_conditioner_result
sippymodem_v34_channel_condition_interval(
	const struct sippymodem_v34_channel_conditioner *conditioner,
	const struct sippymodem_qam_point
		normalized[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct sippymodem_qam_point
		grid_estimates[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL]);

const char *sippymodem_v34_channel_conditioner_result_name(
	enum sippymodem_v34_channel_conditioner_result result);

#endif /* SIPPYMODEM_V34_CHANNEL_CONDITIONER_H */
