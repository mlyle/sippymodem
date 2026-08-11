// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V44_TX_MONITOR_H
#define SIPPYMODEM_V44_TX_MONITOR_H

#include "v44_encoder.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V44_TX_MONITOR_DEFAULT_WINDOW_OCTETS = 256,
	SIPPYMODEM_V44_TX_MONITOR_DEFAULT_LEAVE_EXCESS_BITS = 32,
	SIPPYMODEM_V44_TX_MONITOR_DEFAULT_ENTER_SAVINGS_BITS = 64,
};

struct sippymodem_v44_tx_monitor_config {
	uint32_t window_octets;
	uint32_t leave_compressed_excess_bits;
	uint32_t enter_compressed_savings_bits;
};

enum sippymodem_v44_tx_monitor_decision {
	SIPPYMODEM_V44_TX_MONITOR_STAY = 0,
	SIPPYMODEM_V44_TX_MONITOR_ENTER_COMPRESSED,
	SIPPYMODEM_V44_TX_MONITOR_ENTER_TRANSPARENT,
};

struct sippymodem_v44_tx_monitor_sample {
	enum sippymodem_v44_encoder_mode mode;
	uint64_t represented_octets;
	uint64_t compressed_bits;
	enum sippymodem_v44_tx_monitor_decision decision;
};

enum sippymodem_v44_tx_monitor_result {
	SIPPYMODEM_V44_TX_MONITOR_OK = 0,
	SIPPYMODEM_V44_TX_MONITOR_DISABLED,
	SIPPYMODEM_V44_TX_MONITOR_INVALID_ARGUMENT,
	SIPPYMODEM_V44_TX_MONITOR_INVALID_STATE,
};

struct sippymodem_v44_tx_monitor;

struct sippymodem_v44_tx_monitor_config
sippymodem_v44_tx_monitor_default_config(void);

/*
 * The sample window must be at least the negotiated maximum string length.
 * A new monitor starts in compressed mode, matching a new stream encoder.
 */
struct sippymodem_v44_tx_monitor *sippymodem_v44_tx_monitor_create(
	const struct sippymodem_v44_parameters *parameters,
	const struct sippymodem_v44_tx_monitor_config *config);
void sippymodem_v44_tx_monitor_destroy(
	struct sippymodem_v44_tx_monitor *monitor);

/*
 * Observe one octet after the real encoder accepts it. The monitor owns and
 * drains a two-octet compressed shadow; it never retains caller data. A mode
 * decision is latched, and further observations are rejected until the real
 * transition completes and confirm_mode is called.
 *
 * Valid calls initialize decision. INVALID_ARGUMENT leaves it unchanged.
 * DISABLED means adaptation stopped permanently; the real data path may
 * continue without the monitor.
 */
enum sippymodem_v44_tx_monitor_result sippymodem_v44_tx_monitor_observe(
	struct sippymodem_v44_tx_monitor *monitor,
	uint8_t octet,
	enum sippymodem_v44_tx_monitor_decision *decision);

/*
 * Mirror a real C-FLUSH after all preceding octets have been observed. The
 * flush can finish a sample, so it returns and latches a decision exactly as
 * observe does. Valid calls initialize decision.
 */
enum sippymodem_v44_tx_monitor_result sippymodem_v44_tx_monitor_flush(
	struct sippymodem_v44_tx_monitor *monitor,
	enum sippymodem_v44_tx_monitor_decision *decision);

/*
 * Confirm a completed real mode transition. A pending decision requires its
 * matching target. With no pending decision, this also follows an explicit
 * caller-requested transition. Pre-transition costs are excluded from the
 * next sample, and an ECM confirmation resets the shadow dictionary.
 */
enum sippymodem_v44_tx_monitor_result
sippymodem_v44_tx_monitor_confirm_mode(
	struct sippymodem_v44_tx_monitor *monitor,
	enum sippymodem_v44_encoder_mode mode);

/* Leave sample unchanged when no completed sample is available. */
bool sippymodem_v44_tx_monitor_get_last_sample(
	const struct sippymodem_v44_tx_monitor *monitor,
	struct sippymodem_v44_tx_monitor_sample *sample);

const char *sippymodem_v44_tx_monitor_result_name(
	enum sippymodem_v44_tx_monitor_result result);

#endif
