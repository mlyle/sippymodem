// SPDX-License-Identifier: BSD-3-Clause

#include "baudot.h"

#include <string.h>

enum {
	BAUDOT_MARK_HZ = 1400,
	BAUDOT_SPACE_HZ = 1800,
	BAUDOT_AMPLITUDE = 8192,
	BAUDOT_DETECTOR_WINDOW = 64,
	BAUDOT_PREAMBLE_SAMPLES = 1200,
	BAUDOT_MARK_HOLD_SAMPLES = 2400,
	BAUDOT_SHIFT_REFRESH_CHARACTERS = 72,
	BAUDOT_RX_MIN_DOMINANCE_Q8 = 176,
	/* Detector line power is the sum over its 64-sample window. */
	BAUDOT_RX_MIN_LINE_POWER = BAUDOT_DETECTOR_WINDOW * 256,
	BAUDOT_RX_STABLE_SAMPLES = 3,
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

static const struct fsk_channel baudot_45_channel = {
	.sample_rate = SIPPYMODEM_BAUDOT_SAMPLE_RATE,
	.symbol_rate_numerator = 500,
	.symbol_rate_denominator = 11,
	.mark_frequency_hz = BAUDOT_MARK_HZ,
	.space_frequency_hz = BAUDOT_SPACE_HZ,
	.detector_window = BAUDOT_DETECTOR_WINDOW,
	.metric_smoothing_shift = 1,
};

static const struct fsk_channel baudot_50_channel = {
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

static const struct fsk_channel *channel_for_rate(
	enum sippymodem_baudot_rate rate)
{
	return rate == SIPPYMODEM_BAUDOT_RATE_50 ?
		       &baudot_50_channel : &baudot_45_channel;
}

static uint16_t samples_per_bit(enum sippymodem_baudot_rate rate)
{
	return rate == SIPPYMODEM_BAUDOT_RATE_50 ? 160u : 176u;
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
	expected_bit_samples = samples_per_bit(transmitter->rate);
	return transmitter->bit_samples == expected_bit_samples &&
	       transmitter->stop_samples ==
		       expected_bit_samples + expected_bit_samples / 2u;
}

enum sippymodem_baudot_result sippymodem_baudot_transmitter_init(
	struct sippymodem_baudot_transmitter *transmitter,
	enum sippymodem_baudot_rate rate)
{
	const struct fsk_channel *channel;
	struct fsk_tone_generator tones;
	uint16_t bit_samples;

	if (transmitter == NULL || !rate_is_valid(rate))
		return SIPPYMODEM_BAUDOT_INVALID_ARGUMENT;
	channel = channel_for_rate(rate);
	if (fsk_tone_generator_init(
		    &tones, channel, BAUDOT_AMPLITUDE) != FSK_OK)
		return SIPPYMODEM_BAUDOT_INVALID_ARGUMENT;
	bit_samples = samples_per_bit(rate);
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
		if (fsk_tone_generator_sample(
			    &transmitter->tones, transmitter->current_bit,
			    &output[index]) != FSK_OK)
			return SIPPYMODEM_BAUDOT_INVALID_ARGUMENT;
		transmitter->generated_tone_samples++;
		transmitter->samples_remaining--;
		if (transmitter->samples_remaining == 0)
			advance_transmitter(transmitter);
	}
	return SIPPYMODEM_BAUDOT_OK;
}

static bool receiver_is_valid(
	const struct sippymodem_baudot_receiver *receiver)
{
	const struct fsk_channel *expected;

	if (receiver == NULL || !rate_is_valid(receiver->rate))
		return false;
	expected = channel_for_rate(receiver->rate);
	return receiver->shift >= SIPPYMODEM_BAUDOT_LETTERS &&
	       receiver->shift <= SIPPYMODEM_BAUDOT_FIGURES &&
	       receiver->bit_samples == samples_per_bit(receiver->rate) &&
	       receiver->decision_countdown <=
		       receiver->bit_samples + receiver->bit_samples / 2u &&
	       receiver->character_head <
		       SIPPYMODEM_BAUDOT_RX_CHARACTER_CAPACITY &&
	       receiver->character_count <=
		       SIPPYMODEM_BAUDOT_RX_CHARACTER_CAPACITY &&
	       receiver->detector_bit <= 1 && receiver->stable_bit <= 1 &&
	       receiver->data_index <= 5 && receiver->code < 32 &&
	       receiver->detector.channel.sample_rate == expected->sample_rate &&
	       receiver->detector.channel.symbol_rate_numerator ==
		       expected->symbol_rate_numerator &&
	       receiver->detector.channel.symbol_rate_denominator ==
		       expected->symbol_rate_denominator &&
	       receiver->detector.channel.mark_frequency_hz ==
		       expected->mark_frequency_hz &&
	       receiver->detector.channel.space_frequency_hz ==
		       expected->space_frequency_hz &&
	       receiver->detector.channel.detector_window ==
		       expected->detector_window &&
	       receiver->detector.channel.metric_smoothing_shift ==
		       expected->metric_smoothing_shift;
}

static void reset_receive_signal(
	struct sippymodem_baudot_receiver *receiver)
{
	fsk_receiver_reset(&receiver->detector);
	receiver->quiet_samples = 0;
	receiver->stable_samples = 0;
	receiver->decision_countdown = 0;
	receiver->detector_bit = 1;
	receiver->stable_bit = 1;
	receiver->data_index = 0;
	receiver->code = 0;
	receiver->carrier = false;
	receiver->framing = false;
}

enum sippymodem_baudot_result sippymodem_baudot_receiver_init(
	struct sippymodem_baudot_receiver *receiver,
	enum sippymodem_baudot_rate rate)
{
	struct fsk_receiver detector;

	if (receiver == NULL || !rate_is_valid(rate) ||
	    fsk_receiver_init(
		    &detector, channel_for_rate(rate)) != FSK_OK)
		return SIPPYMODEM_BAUDOT_INVALID_ARGUMENT;
	memset(receiver, 0, sizeof(*receiver));
	receiver->detector = detector;
	receiver->rate = rate;
	receiver->shift = SIPPYMODEM_BAUDOT_LETTERS;
	receiver->bit_samples = samples_per_bit(rate);
	reset_receive_signal(receiver);
	return SIPPYMODEM_BAUDOT_OK;
}

void sippymodem_baudot_receiver_reset(
	struct sippymodem_baudot_receiver *receiver)
{
	if (receiver_is_valid(receiver))
		reset_receive_signal(receiver);
}

static void increment_counter(uint64_t *counter)
{
	if (*counter != UINT64_MAX)
		(*counter)++;
}

static size_t received_character_index(uint16_t head, size_t offset)
{
	return ((size_t)head + offset) %
	       SIPPYMODEM_BAUDOT_RX_CHARACTER_CAPACITY;
}

static void queue_received_character(
	struct sippymodem_baudot_receiver *receiver, uint8_t character)
{
	if (receiver->character_count >=
	    SIPPYMODEM_BAUDOT_RX_CHARACTER_CAPACITY) {
		increment_counter(&receiver->receive_overruns);
		return;
	}
	receiver->characters[received_character_index(
		receiver->character_head, receiver->character_count)] = character;
	receiver->character_count++;
	increment_counter(&receiver->received_characters);
}

static void decode_code(
	struct sippymodem_baudot_receiver *receiver, uint8_t code)
{
	uint8_t character;

	code &= 0x1fu;
	if (code == BAUDOT_CODE_LETTERS) {
		receiver->shift = SIPPYMODEM_BAUDOT_LETTERS;
		return;
	}
	if (code == BAUDOT_CODE_FIGURES) {
		receiver->shift = SIPPYMODEM_BAUDOT_FIGURES;
		return;
	}
	character = receiver->shift == SIPPYMODEM_BAUDOT_FIGURES ?
			    baudot_alphabet[code].figures :
			    baudot_alphabet[code].letters;
	if (character != 0)
		queue_received_character(receiver, character);
	if (code == 4)
		receiver->shift = SIPPYMODEM_BAUDOT_LETTERS;
}

static void begin_receive_frame(
	struct sippymodem_baudot_receiver *receiver,
	bool carrier_began_with_space)
{
	uint32_t delay = carrier_began_with_space ?
				 (uint32_t)receiver->bit_samples * 5u / 4u :
				 (uint32_t)receiver->bit_samples * 3u / 2u;

	receiver->framing = true;
	receiver->data_index = 0;
	receiver->code = 0;
	receiver->decision_countdown = (uint16_t)delay;
}

static bool signal_is_present(
	const struct sippymodem_baudot_receiver *receiver,
	bool decision_ready)
{
	struct fsk_measurement measurement =
		fsk_receiver_measure(&receiver->detector);

	return decision_ready && measurement.ready &&
	       measurement.line_power >= BAUDOT_RX_MIN_LINE_POWER &&
	       measurement.dominance_q8 >= BAUDOT_RX_MIN_DOMINANCE_Q8;
}

static void lose_receive_carrier(
	struct sippymodem_baudot_receiver *receiver)
{
	if (receiver->framing)
		increment_counter(&receiver->framing_errors);
	receiver->carrier = false;
	receiver->framing = false;
	receiver->stable_samples = 0;
	receiver->decision_countdown = 0;
	receiver->detector_bit = 1;
	receiver->stable_bit = 1;
}

static void sample_receive_frame(
	struct sippymodem_baudot_receiver *receiver)
{
	if (receiver->data_index < 5) {
		if (receiver->stable_bit != 0)
			receiver->code |=
				(uint8_t)(1u << receiver->data_index);
		receiver->data_index++;
		receiver->decision_countdown =
			(uint16_t)(receiver->bit_samples - 1u);
		return;
	}
	if (receiver->stable_bit != 0)
		decode_code(receiver, receiver->code);
	else
		increment_counter(&receiver->framing_errors);
	receiver->framing = false;
	receiver->data_index = 0;
	receiver->decision_countdown = 0;
	receiver->code = 0;
}

static enum sippymodem_baudot_result receive_sample(
	struct sippymodem_baudot_receiver *receiver, int16_t sample)
{
	uint8_t detected_bit;
	uint8_t previous_stable = receiver->stable_bit;
	bool decision_ready;

	if (fsk_receiver_detect(
		    &receiver->detector, sample, &detected_bit,
		    &decision_ready) != FSK_OK)
		return SIPPYMODEM_BAUDOT_INVALID_ARGUMENT;
	if (!signal_is_present(receiver, decision_ready)) {
		if (receiver->quiet_samples != UINT16_MAX)
			receiver->quiet_samples++;
		if (receiver->quiet_samples >= receiver->bit_samples / 2u)
			lose_receive_carrier(receiver);
		return SIPPYMODEM_BAUDOT_OK;
	}

	receiver->quiet_samples = 0;
	detected_bit &= 1u;
	if (!receiver->carrier) {
		receiver->carrier = true;
		receiver->detector_bit = detected_bit;
		receiver->stable_bit = detected_bit;
		receiver->stable_samples = 1;
		if (detected_bit == 0)
			begin_receive_frame(receiver, true);
	} else {
		if (detected_bit == receiver->detector_bit) {
			if (receiver->stable_samples != UINT16_MAX)
				receiver->stable_samples++;
		} else {
			receiver->detector_bit = detected_bit;
			receiver->stable_samples = 1;
		}
		if (receiver->stable_samples >= BAUDOT_RX_STABLE_SAMPLES)
			receiver->stable_bit = receiver->detector_bit;
		if (!receiver->framing && previous_stable != 0 &&
		    receiver->stable_bit == 0)
			begin_receive_frame(receiver, false);
	}

	if (receiver->framing) {
		if (receiver->decision_countdown == 0)
			sample_receive_frame(receiver);
		else
			receiver->decision_countdown--;
	}
	return SIPPYMODEM_BAUDOT_OK;
}

enum sippymodem_baudot_result sippymodem_baudot_receive(
	struct sippymodem_baudot_receiver *receiver,
	const int16_t *input,
	size_t count)
{
	if (!receiver_is_valid(receiver) || (input == NULL && count != 0))
		return SIPPYMODEM_BAUDOT_INVALID_ARGUMENT;
	for (size_t index = 0; index < count; ++index)
		if (receive_sample(receiver, input[index]) !=
		    SIPPYMODEM_BAUDOT_OK)
			return SIPPYMODEM_BAUDOT_INVALID_ARGUMENT;
	return SIPPYMODEM_BAUDOT_OK;
}

size_t sippymodem_baudot_drain_text(
	struct sippymodem_baudot_receiver *receiver,
	uint8_t *text,
	size_t capacity)
{
	size_t count = 0;

	if (!receiver_is_valid(receiver) ||
	    (text == NULL && capacity != 0))
		return 0;
	while (count < capacity && receiver->character_count != 0) {
		text[count++] = receiver->characters[receiver->character_head];
		receiver->character_head = (uint16_t)received_character_index(
			receiver->character_head, 1);
		receiver->character_count--;
	}
	return count;
}

size_t sippymodem_baudot_pending_text(
	const struct sippymodem_baudot_receiver *receiver)
{
	return receiver_is_valid(receiver) ? receiver->character_count : 0;
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
