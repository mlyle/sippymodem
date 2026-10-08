// SPDX-License-Identifier: BSD-3-Clause

#ifndef V44_DUPLEX_H
#define V44_DUPLEX_H

#include "v44_encoder.h"
#include "v44_tx_monitor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum v44_duplex_result {
	V44_DUPLEX_OK = 0,
	V44_DUPLEX_NEED_INPUT,
	V44_DUPLEX_NEED_OUTPUT,
	V44_DUPLEX_FLUSHED,
	V44_DUPLEX_ENTER_PARAMETER,
	V44_DUPLEX_INVALID_ARGUMENT,
	V44_DUPLEX_INVALID_STATE,
	V44_DUPLEX_PROCEDURAL_ERROR,
};

/*
 * Valid data-path calls always initialize output_length, including when they
 * report a state or wire error. INVALID_ARGUMENT leaves caller spans intact.
 * A procedural error permanently fails only the affected direction.
 */

struct v44_duplex;

struct v44_duplex_config {
	bool transmit_enabled;
	struct v44_parameters transmit_parameters;
	bool transmit_adaptation_enabled;
	struct v44_tx_monitor_config transmit_adaptation;
	bool receive_enabled;
	struct v44_parameters receive_parameters;
};

/*
 * Parameters use the local endpoint's transmit/receive perspective. Enabled
 * directions own independent codec state and derive bounded child capacities
 * from those parameters. One transmit call and one receive call may run
 * concurrently; calls within a direction and destruction must be serialized.
 * Disabled directions are literal pass-through paths with no child codec.
 * Transmit adaptation is opt-in, requires transmit to be enabled, and drives
 * mode changes internally without changing the public data-path results.
 */
struct v44_duplex *v44_duplex_create(
	const struct v44_duplex_config *config);
void v44_duplex_destroy(
	struct v44_duplex *duplex);

/*
 * Explicitly change transmit stream mode. Drain output and repeat the same
 * target on NEED_OUTPUT. Until the request returns OK, transmit and flush are
 * rejected, as is a conflicting target. A request for the current mode is an
 * idempotent no-op, not an output barrier. Disabled transmit has no stream
 * mode. A request matching an automatic transition may finish that transition.
 */
enum v44_duplex_result
v44_duplex_request_transmit_mode(
	struct v44_duplex *duplex,
	enum v44_encoder_mode target,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length);

/*
 * Consume uncompressed bytes and drain compressed bytes directly into the
 * caller's output buffer. The object owns no queue beyond the encoder's
 * two-octet field queue. NEED_INPUT means all supplied input was accepted.
 * NEED_OUTPUT may follow input consumption; retry with the updated pointer.
 * An automatic mode transition is drained by the same retry path before any
 * further input is accepted.
 * A zero-capacity output may be NULL. Input and output must not overlap.
 */
enum v44_duplex_result v44_duplex_transmit(
	struct v44_duplex *duplex,
	const uint8_t **input,
	size_t *input_length,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length);

/*
 * Flush bytes accepted before the first call through one explicit C-FLUSH,
 * draining on NEED_OUTPUT. While this is incomplete, transmit returns
 * INVALID_STATE. A disabled transmit direction returns FLUSHED without wire
 * data. A mode decision completed by C-FLUSH is applied by the next transmit
 * call, while this call still reports the FLUSHED stream boundary.
 */
enum v44_duplex_result v44_duplex_flush_transmit(
	struct v44_duplex *duplex,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length);

/*
 * Consume compressed bytes and drain decoded bytes directly to output.
 * NEED_OUTPUT may follow input consumption; retry with the updated pointer.
 * FLUSHED and ENTER_PARAMETER preserve any following input for the caller.
 * A zero-capacity output may be NULL. Input and output must not overlap.
 */
enum v44_duplex_result v44_duplex_receive(
	struct v44_duplex *duplex,
	const uint8_t **input,
	size_t *input_length,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length);

enum v44_duplex_result
v44_duplex_resume_receive_transparent(
	struct v44_duplex *duplex);
/* Finish returns NEED_OUTPUT until receive drains all decoded bytes. */
enum v44_duplex_result v44_duplex_finish_receive(
	struct v44_duplex *duplex);

/* Transmit-direction diagnostics; serialize these with other transmit calls. */
bool v44_duplex_get_transmit_mode(
	const struct v44_duplex *duplex,
	enum v44_encoder_mode *mode);
bool v44_duplex_transmit_adaptation_active(
	const struct v44_duplex *duplex);
bool v44_duplex_get_last_transmit_sample(
	const struct v44_duplex *duplex,
	struct v44_tx_monitor_sample *sample);

#endif
