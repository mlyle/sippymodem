// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V44_DUPLEX_H
#define SIPPYMODEM_V44_DUPLEX_H

#include "sippymodem_v44_stream.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum sippymodem_v44_duplex_result {
	SIPPYMODEM_V44_DUPLEX_OK = 0,
	SIPPYMODEM_V44_DUPLEX_NEED_INPUT,
	SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT,
	SIPPYMODEM_V44_DUPLEX_FLUSHED,
	SIPPYMODEM_V44_DUPLEX_ENTER_PARAMETER,
	SIPPYMODEM_V44_DUPLEX_INVALID_ARGUMENT,
	SIPPYMODEM_V44_DUPLEX_INVALID_STATE,
	SIPPYMODEM_V44_DUPLEX_PROCEDURAL_ERROR,
};

/*
 * Valid data-path calls always initialize output_length, including when they
 * report a state or wire error. INVALID_ARGUMENT leaves caller spans intact.
 * A procedural error permanently fails only the affected direction.
 */

struct sippymodem_v44_duplex;

struct sippymodem_v44_duplex_config {
	bool transmit_enabled;
	struct sippymodem_v44_parameters transmit_parameters;
	bool receive_enabled;
	struct sippymodem_v44_parameters receive_parameters;
};

/*
 * Parameters use the local endpoint's transmit/receive perspective. Enabled
 * directions own independent codec state and derive bounded child capacities
 * from those parameters. One transmit call and one receive call may run
 * concurrently; calls within a direction and destruction must be serialized.
 * Disabled directions are literal pass-through paths with no child codec.
 */
struct sippymodem_v44_duplex *sippymodem_v44_duplex_create(
	const struct sippymodem_v44_duplex_config *config);
void sippymodem_v44_duplex_destroy(
	struct sippymodem_v44_duplex *duplex);

/*
 * Consume uncompressed bytes and drain compressed bytes directly into the
 * caller's output buffer. The object owns no queue beyond the encoder's
 * two-octet field queue. NEED_INPUT means all supplied input was accepted.
 * NEED_OUTPUT may follow input consumption; retry with the updated pointer.
 * A zero-capacity output may be NULL. Input and output must not overlap.
 */
enum sippymodem_v44_duplex_result sippymodem_v44_duplex_transmit(
	struct sippymodem_v44_duplex *duplex,
	const uint8_t **input,
	size_t *input_length,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length);

/*
 * Flush bytes accepted before the first call through one explicit C-FLUSH,
 * draining on NEED_OUTPUT. While this is incomplete, transmit returns
 * INVALID_STATE. A disabled transmit direction returns FLUSHED without wire
 * data.
 */
enum sippymodem_v44_duplex_result sippymodem_v44_duplex_flush_transmit(
	struct sippymodem_v44_duplex *duplex,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length);

/*
 * Consume compressed bytes and drain decoded bytes directly to output.
 * NEED_OUTPUT may follow input consumption; retry with the updated pointer.
 * FLUSHED and ENTER_PARAMETER preserve any following input for the caller.
 * A zero-capacity output may be NULL. Input and output must not overlap.
 */
enum sippymodem_v44_duplex_result sippymodem_v44_duplex_receive(
	struct sippymodem_v44_duplex *duplex,
	const uint8_t **input,
	size_t *input_length,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length);

enum sippymodem_v44_duplex_result
sippymodem_v44_duplex_resume_receive_transparent(
	struct sippymodem_v44_duplex *duplex);
/* Finish returns NEED_OUTPUT until receive drains all decoded bytes. */
enum sippymodem_v44_duplex_result sippymodem_v44_duplex_finish_receive(
	struct sippymodem_v44_duplex *duplex);

const char *sippymodem_v44_duplex_result_name(
	enum sippymodem_v44_duplex_result result);

#endif
