// SPDX-License-Identifier: BSD-3-Clause

#ifndef V44_TX_MONITOR_H
#define V44_TX_MONITOR_H

#include "v44_encoder.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V44_TX_MONITOR_DEFAULT_WINDOW_OCTETS = 256,
	V44_TX_MONITOR_DEFAULT_LEAVE_EXCESS_BITS = 32,
	V44_TX_MONITOR_DEFAULT_ENTER_SAVINGS_BITS = 64,
};

struct v44_tx_monitor_config {
	uint32_t window_octets;
	uint32_t leave_compressed_excess_bits;
	uint32_t enter_compressed_savings_bits;
};

enum v44_tx_monitor_decision {
	V44_TX_MONITOR_STAY = 0,
	V44_TX_MONITOR_ENTER_COMPRESSED,
	V44_TX_MONITOR_ENTER_TRANSPARENT,
};

struct v44_tx_monitor_sample {
	enum v44_encoder_mode mode;
	uint64_t represented_octets;
	uint64_t compressed_bits;
	enum v44_tx_monitor_decision decision;
};

enum v44_tx_monitor_result {
	V44_TX_MONITOR_OK = 0,
	V44_TX_MONITOR_DISABLED,
	V44_TX_MONITOR_INVALID_ARGUMENT,
	V44_TX_MONITOR_INVALID_STATE,
};

struct v44_tx_monitor;

struct v44_tx_monitor_config
v44_tx_monitor_default_config(void);

/*
 * The sample window must be at least the negotiated maximum string length.
 * A new monitor starts in compressed mode, matching a new stream encoder.
 */
struct v44_tx_monitor *v44_tx_monitor_create(
	const struct v44_parameters *parameters,
	const struct v44_tx_monitor_config *config);
void v44_tx_monitor_destroy(
	struct v44_tx_monitor *monitor);

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
enum v44_tx_monitor_result v44_tx_monitor_observe(
	struct v44_tx_monitor *monitor,
	uint8_t octet,
	enum v44_tx_monitor_decision *decision);

/*
 * Mirror a real C-FLUSH after all preceding octets have been observed. The
 * flush can finish a sample, so it returns and latches a decision exactly as
 * observe does. Valid calls initialize decision.
 */
enum v44_tx_monitor_result v44_tx_monitor_flush(
	struct v44_tx_monitor *monitor,
	enum v44_tx_monitor_decision *decision);

/*
 * Confirm a completed real mode transition. A pending decision requires its
 * matching target. With no pending decision, this also follows an explicit
 * caller-requested transition. Pre-transition costs are excluded from the
 * next sample, and an ECM confirmation resets the shadow dictionary.
 */
enum v44_tx_monitor_result
v44_tx_monitor_confirm_mode(
	struct v44_tx_monitor *monitor,
	enum v44_encoder_mode mode);

/* Leave sample unchanged when no completed sample is available. */
bool v44_tx_monitor_get_last_sample(
	const struct v44_tx_monitor *monitor,
	struct v44_tx_monitor_sample *sample);

#endif
