// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_baudot.h"

#include <string.h>

enum {
	BAUDOT_MARK_HZ = 1400,
	BAUDOT_SPACE_HZ = 1800,
	BAUDOT_AMPLITUDE = 8192,
	BAUDOT_DETECTOR_WINDOW = 64,
	BAUDOT_PREAMBLE_SAMPLES = 1200,
	BAUDOT_MARK_HOLD_SAMPLES = 2400,
	BAUDOT_SHIFT_REFRESH_CHARACTERS = 72,
	BAUDOT_CODE_FIGURES = 0x1b,
	BAUDOT_CODE_LETTERS = 0x1f,
};

struct baudot_character {
	uint8_t letters;
	uint8_t figures;
};

/* ANSI/TIA-825 US TTY assignment. Shift codes occupy entries 27 and 31. */
static const struct baudot_character baudot_alphabet[32] = {
	{ '\b', '\b' }, { 'E', '3' },  { '\n', '\n' }, { 'A', '-' },
	{ ' ', ' ' },   { 'S', '-' },  { 'I', '8' },    { 'U', '7' },
	{ '\r', '\r' }, { 'D', '$' },  { 'R', '4' },    { 'J', '\'' },
	{ 'N', ',' },   { 'F', '!' },  { 'C', ':' },    { 'K', '(' },
	{ 'T', '5' },   { 'Z', '"' },  { 'L', ')' },    { 'W', '2' },
	{ 'H', '=' },   { 'Y', '6' },  { 'P', '0' },    { 'Q', '1' },
	{ 'O', '9' },   { 'B', '?' },  { 'G', '+' },    { 0, 0 },
	{ 'M', '.' },   { 'X', '/' },  { 'V', ';' },    { 0, 0 },
};

struct baudot_code_plan {
	uint8_t codes[3];
	size_t count;
	enum sippymodem_baudot_shift resulting_shift;
	uint8_t resulting_characters_since_shift;
};

static const struct sippymodem_fsk_channel baudot_45_channel = {
	.sample_rate = SIPPYMODEM_BAUDOT_SAMPLE_RATE,
	.symbol_rate_numerator = 500,
	.symbol_rate_denominator = 11,
	.mark_frequency_hz = BAUDOT_MARK_HZ,
	.space_frequency_hz = BAUDOT_SPACE_HZ,
	.detector_window = BAUDOT_DETECTOR_WINDOW,
	.metric_smoothing_shift = 1,
};

static const struct sippymodem_fsk_channel baudot_50_channel = {
	.sample_rate = SIPPYMODEM_BAUDOT_SAMPLE_RATE,
	.symbol_rate_numerator = 50,
	.symbol_rate_denominator = 1,
	.mark_frequency_hz = BAUDOT_MARK_HZ,
	.space_frequency_hz = BAUDOT_SPACE_HZ,
	.detector_window = BAUDOT_DETECTOR_WINDOW,
	.metric_smoothing_shift = 1,
};

static bool rate_is_valid(enum sippymodem_baudot_rate rate)
{
	return rate == SIPPYMODEM_BAUDOT_RATE_45 ||
	       rate == SIPPYMODEM_BAUDOT_RATE_50;
}

static bool transmitter_is_valid(
	const struct sippymodem_baudot_transmitter *transmitter)
{
	uint16_t expected_bit_samples;

	if (transmitter == NULL || !rate_is_valid(transmitter->rate) ||
	    transmitter->phase < SIPPYMODEM_BAUDOT_TX_IDLE ||
	    transmitter->phase > SIPPYMODEM_BAUDOT_TX_MARK_HOLD ||
	    transmitter->shift < SIPPYMODEM_BAUDOT_LETTERS ||
	    transmitter->shift > SIPPYMODEM_BAUDOT_FIGURES ||
	    transmitter->code_head >= SIPPYMODEM_BAUDOT_TX_CODE_CAPACITY ||
	    transmitter->code_count > SIPPYMODEM_BAUDOT_TX_CODE_CAPACITY ||
	    transmitter->current_bit > 1 ||
	    transmitter->characters_since_shift >
		    BAUDOT_SHIFT_REFRESH_CHARACTERS)
		return false;
	expected_bit_samples =
		transmitter->rate == SIPPYMODEM_BAUDOT_RATE_50 ? 160u : 176u;
	return transmitter->bit_samples == expected_bit_samples &&
	       transmitter->stop_samples ==
		       expected_bit_samples + expected_bit_samples / 2u;
}

enum sippymodem_baudot_result sippymodem_baudot_transmitter_init(
	struct sippymodem_baudot_transmitter *transmitter,
	enum sippymodem_baudot_rate rate)
{
	const struct sippymodem_fsk_channel *channel;
	struct sippymodem_fsk_tone_generator tones;
	uint16_t bit_samples;

	if (transmitter == NULL || !rate_is_valid(rate))
		return SIPPYMODEM_BAUDOT_INVALID_ARGUMENT;
	channel = rate == SIPPYMODEM_BAUDOT_RATE_50 ?
			  &baudot_50_channel : &baudot_45_channel;
	if (sippymodem_fsk_tone_generator_init(
		    &tones, channel, BAUDOT_AMPLITUDE) != SIPPYMODEM_FSK_OK)
		return SIPPYMODEM_BAUDOT_INVALID_ARGUMENT;
	bit_samples = rate == SIPPYMODEM_BAUDOT_RATE_50 ? 160u : 176u;
	memset(transmitter, 0, sizeof(*transmitter));
	transmitter->tones = tones;
	transmitter->rate = rate;
	transmitter->shift = SIPPYMODEM_BAUDOT_LETTERS;
	transmitter->phase = SIPPYMODEM_BAUDOT_TX_IDLE;
	transmitter->bit_samples = bit_samples;
	transmitter->stop_samples =
		(uint16_t)(bit_samples + bit_samples / 2u);
	transmitter->current_bit = 1;
	return SIPPYMODEM_BAUDOT_OK;
}

static void find_character(
	uint8_t character, uint8_t *code,
	enum sippymodem_baudot_shift *shift)
{
	uint8_t upper = character;

	if (upper >= 'a' && upper <= 'z')
		upper = (uint8_t)(upper - ('a' - 'A'));
	for (uint8_t value = 0; value < 32; ++value) {
		if (baudot_alphabet[value].letters == upper) {
			*code = value;
			*shift = SIPPYMODEM_BAUDOT_LETTERS;
			return;
		}
	}
	for (uint8_t value = 0; value < 32; ++value) {
		if (baudot_alphabet[value].figures == upper) {
			*code = value;
			*shift = SIPPYMODEM_BAUDOT_FIGURES;
			return;
		}
	}
	/* Preserve unsupported terminal input visibly instead of dropping it. */
	*code = 25;
	*shift = SIPPYMODEM_BAUDOT_FIGURES;
}

static bool character_is_column_neutral(uint8_t code)
{
	return code < 32 && baudot_alphabet[code].letters != 0 &&
	       baudot_alphabet[code].letters == baudot_alphabet[code].figures;
}

static struct baudot_code_plan plan_character(
	const struct sippymodem_baudot_transmitter *transmitter,
	uint8_t code,
	enum sippymodem_baudot_shift requested_shift,
	bool fresh_burst)
{
	struct baudot_code_plan plan = {
		.resulting_shift = transmitter->shift,
		.resulting_characters_since_shift =
			transmitter->characters_since_shift,
	};

	if (fresh_burst) {
		plan.codes[plan.count++] = BAUDOT_CODE_LETTERS;
		plan.resulting_shift = SIPPYMODEM_BAUDOT_LETTERS;
		plan.resulting_characters_since_shift = 0;
	}
	/* A space repairs a peer that was left in the figures column. */
	if (code == 4)
		requested_shift = SIPPYMODEM_BAUDOT_LETTERS;
	if ((code == 4 || !character_is_column_neutral(code)) &&
	    requested_shift != plan.resulting_shift) {
		plan.codes[plan.count++] =
			requested_shift == SIPPYMODEM_BAUDOT_FIGURES ?
				BAUDOT_CODE_FIGURES : BAUDOT_CODE_LETTERS;
		plan.resulting_shift = requested_shift;
		plan.resulting_characters_since_shift = 0;
	} else if (plan.resulting_characters_since_shift >=
		   BAUDOT_SHIFT_REFRESH_CHARACTERS) {
		plan.codes[plan.count++] =
			plan.resulting_shift == SIPPYMODEM_BAUDOT_FIGURES ?
				BAUDOT_CODE_FIGURES : BAUDOT_CODE_LETTERS;
		plan.resulting_characters_since_shift = 0;
	}
	plan.codes[plan.count++] = code;
	plan.resulting_characters_since_shift++;
	return plan;
}

static size_t ring_index(uint16_t head, size_t offset)
{
	return ((size_t)head + offset) %
	       SIPPYMODEM_BAUDOT_TX_CODE_CAPACITY;
}

static void push_code(
	struct sippymodem_baudot_transmitter *transmitter, uint8_t code)
{
	transmitter->codes[ring_index(
		transmitter->code_head, transmitter->code_count)] =
		code & 0x1fu;
	transmitter->code_count++;
}

static bool pop_code(
	struct sippymodem_baudot_transmitter *transmitter, uint8_t *code)
{
	if (transmitter->code_count == 0)
		return false;
	*code = transmitter->codes[transmitter->code_head];
	transmitter->code_head = (uint16_t)ring_index(
		transmitter->code_head, 1);
	transmitter->code_count--;
	return true;
}

size_t sippymodem_baudot_queue_text(
	struct sippymodem_baudot_transmitter *transmitter,
	const uint8_t *text,
	size_t count)
{
	size_t consumed = 0;

	if (!transmitter_is_valid(transmitter) ||
	    (text == NULL && count != 0))
		return 0;
	while (consumed < count) {
		struct baudot_code_plan plan;
		enum sippymodem_baudot_shift shift;
		uint8_t code;
		bool fresh_burst =
			transmitter->phase == SIPPYMODEM_BAUDOT_TX_IDLE &&
			transmitter->code_count == 0;

		find_character(text[consumed], &code, &shift);
		plan = plan_character(
			transmitter, code, shift, fresh_burst);
		if (plan.count >
		    (size_t)SIPPYMODEM_BAUDOT_TX_CODE_CAPACITY -
			    transmitter->code_count)
			break;
		for (size_t index = 0; index < plan.count; ++index)
			push_code(transmitter, plan.codes[index]);
		transmitter->shift = plan.resulting_shift;
		transmitter->characters_since_shift =
			plan.resulting_characters_since_shift;
		consumed++;
	}
	return consumed;
}

size_t sippymodem_baudot_pending_codes(
	const struct sippymodem_baudot_transmitter *transmitter)
{
	return transmitter_is_valid(transmitter) ? transmitter->code_count : 0;
}

bool sippymodem_baudot_transmitting(
	const struct sippymodem_baudot_transmitter *transmitter)
{
	return transmitter_is_valid(transmitter) &&
	       (transmitter->phase != SIPPYMODEM_BAUDOT_TX_IDLE ||
		transmitter->code_count != 0);
}

static void set_bit(
	struct sippymodem_baudot_transmitter *transmitter, uint8_t bit)
{
	transmitter->current_bit = bit & 1u;
}

static bool begin_code(
	struct sippymodem_baudot_transmitter *transmitter)
{
	if (!pop_code(transmitter, &transmitter->current_code))
		return false;
	transmitter->phase = SIPPYMODEM_BAUDOT_TX_CODE;
	transmitter->current_element = 0;
	transmitter->samples_remaining = transmitter->bit_samples;
	set_bit(transmitter, 0);
	return true;
}

static void advance_transmitter(
	struct sippymodem_baudot_transmitter *transmitter)
{
	switch (transmitter->phase) {
	case SIPPYMODEM_BAUDOT_TX_IDLE:
		if (transmitter->code_count != 0) {
			transmitter->phase = SIPPYMODEM_BAUDOT_TX_PREAMBLE;
			transmitter->samples_remaining =
				BAUDOT_PREAMBLE_SAMPLES;
			set_bit(transmitter, 1);
		}
		break;
	case SIPPYMODEM_BAUDOT_TX_PREAMBLE:
		(void)begin_code(transmitter);
		break;
	case SIPPYMODEM_BAUDOT_TX_CODE:
		transmitter->current_element++;
		if (transmitter->current_element <= 5) {
			transmitter->samples_remaining = transmitter->bit_samples;
			set_bit(transmitter,
				(uint8_t)(transmitter->current_code >>
					  (transmitter->current_element - 1u)));
		} else if (transmitter->current_element == 6) {
			transmitter->samples_remaining = transmitter->stop_samples;
			set_bit(transmitter, 1);
		} else {
			transmitter->transmitted_codes++;
			if (!begin_code(transmitter)) {
				transmitter->phase =
					SIPPYMODEM_BAUDOT_TX_MARK_HOLD;
				transmitter->samples_remaining =
					BAUDOT_MARK_HOLD_SAMPLES;
				set_bit(transmitter, 1);
			}
		}
		break;
	case SIPPYMODEM_BAUDOT_TX_MARK_HOLD:
		if (!begin_code(transmitter)) {
			transmitter->phase = SIPPYMODEM_BAUDOT_TX_IDLE;
			transmitter->samples_remaining = 0;
		}
		break;
	}
}

enum sippymodem_baudot_result sippymodem_baudot_generate(
	struct sippymodem_baudot_transmitter *transmitter,
	int16_t *output,
	size_t count)
{
	if (!transmitter_is_valid(transmitter) ||
	    (output == NULL && count != 0))
		return SIPPYMODEM_BAUDOT_INVALID_ARGUMENT;
	for (size_t index = 0; index < count; ++index) {
		if (transmitter->phase == SIPPYMODEM_BAUDOT_TX_MARK_HOLD &&
		    transmitter->code_count != 0)
			(void)begin_code(transmitter);
		if (transmitter->samples_remaining == 0)
			advance_transmitter(transmitter);
		if (transmitter->phase == SIPPYMODEM_BAUDOT_TX_IDLE) {
			output[index] = 0;
			continue;
		}
		if (sippymodem_fsk_tone_generator_sample(
			    &transmitter->tones, transmitter->current_bit,
			    &output[index]) != SIPPYMODEM_FSK_OK)
			return SIPPYMODEM_BAUDOT_INVALID_ARGUMENT;
		transmitter->generated_tone_samples++;
		transmitter->samples_remaining--;
		if (transmitter->samples_remaining == 0)
			advance_transmitter(transmitter);
	}
	return SIPPYMODEM_BAUDOT_OK;
}

const char *sippymodem_baudot_result_name(
	enum sippymodem_baudot_result result)
{
	switch (result) {
	case SIPPYMODEM_BAUDOT_OK:
		return "ok";
	case SIPPYMODEM_BAUDOT_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown Baudot result";
}
