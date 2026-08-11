// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V44_TRANSPARENT_DECODER_H
#define SIPPYMODEM_V44_TRANSPARENT_DECODER_H

#include <stddef.h>
#include <stdint.h>

enum sippymodem_v44_transparent_result {
	SIPPYMODEM_V44_TRANSPARENT_OK = 0,
	SIPPYMODEM_V44_TRANSPARENT_NEED_INPUT,
	SIPPYMODEM_V44_TRANSPARENT_NEED_OUTPUT,
	SIPPYMODEM_V44_TRANSPARENT_ENTER_COMPRESSED,
	SIPPYMODEM_V44_TRANSPARENT_ENTER_PARAMETER,
	SIPPYMODEM_V44_TRANSPARENT_INVALID_ARGUMENT,
	SIPPYMODEM_V44_TRANSPARENT_PROCEDURAL_ERROR,
};

struct sippymodem_v44_transparent_decoder;

/*
 * A new decoder represents the C-INIT state: compressed mode with ESCAPE equal
 * to zero. Enter transparent mode explicitly after decoding ETM.
 */
struct sippymodem_v44_transparent_decoder *
sippymodem_v44_transparent_decoder_create(void);
void sippymodem_v44_transparent_decoder_destroy(
	struct sippymodem_v44_transparent_decoder *decoder);

/*
 * Process at most one transparent character or command sequence. NEED_INPUT
 * may consume and retain an ESCAPE prefix. NEED_OUTPUT leaves the pending data
 * or command octet unconsumed so it can be retried with output space. On a
 * successful character, output_length is one; commands produce no output.
 *
 * Mode-transition results are latched without consuming following input. On
 * ENTER_COMPRESSED, the caller must reinitialize its compressed dictionary
 * before passing the preserved input to it. ENTER_PARAMETER similarly hands
 * the preserved input to a parameter-mode parser. The mode owner must deliver
 * any compressed output queued before ETM ahead of transparent output.
 */
enum sippymodem_v44_transparent_result
sippymodem_v44_transparent_decoder_process(
	struct sippymodem_v44_transparent_decoder *decoder,
	const uint8_t **input,
	size_t *input_length,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length);

/*
 * Enter transparent mode after ETM or a parameter-mode End. The evolved
 * ESCAPE value is preserved. Create a fresh decoder when a full C-INIT
 * reinitialization must restore ESCAPE to zero.
 */
enum sippymodem_v44_transparent_result
sippymodem_v44_transparent_decoder_enter(
	struct sippymodem_v44_transparent_decoder *decoder);

/*
 * Local robustness policy fails closed on an undefined command or a dangling
 * ESCAPE at the end of a complete stream.
 */
enum sippymodem_v44_transparent_result
sippymodem_v44_transparent_decoder_finish(
	struct sippymodem_v44_transparent_decoder *decoder);

const char *sippymodem_v44_transparent_result_name(
	enum sippymodem_v44_transparent_result result);

#endif
