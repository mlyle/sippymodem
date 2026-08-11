// SPDX-License-Identifier: BSD-3-Clause

#include "rate_signal.h"

#include <stddef.h>

enum {
	/* ITU-T V.32 Tables 6 and 7, with B0 stored in bit 15. */
	V32_RATE_FIXED_BITS = 0x0111,
	V32_END_FIXED_BITS = 0xf111,
	/* B4 advertises 2400 bit/s in V.32 and, together with B8, marks a
	 * V.32bis rate word. This implementation does not offer 2400 bit/s. */
	V32_B4_2400_OR_BIS = 0x0800,
	V32_B5_4800 = 0x0400,
	V32_B6_9600 = 0x0200,
	V32_B8_TRELLIS = 0x0080,
	V32_B9_7200 = 0x0040,
	V32_B10_12000 = 0x0020,
	V32_B12_14400 = 0x0008,
	/* V.32 bis reserves B13 and B14 for later definition and requires
	 * receivers to ignore them. */
	V32_IGNORED_EXTENSION_BITS = 0x0006,
	V32_RATE_FIELD_MASK = V32_B4_2400_OR_BIS | V32_B5_4800 |
			       V32_B6_9600 | V32_B9_7200 |
			       V32_B10_12000 | V32_B12_14400,
	V32_KNOWN_PAYLOAD_MASK = V32_RATE_FIELD_MASK | V32_B8_TRELLIS,
	V32_RATE_RECEIVE_MASK = V32_KNOWN_PAYLOAD_MASK |
				V32_IGNORED_EXTENSION_BITS,
	V32_RATE_CAPABILITY_MASK = SIPPYMODEM_V32_RATE_CAPABILITY_4800 |
				   SIPPYMODEM_V32_RATE_CAPABILITY_7200 |
				   SIPPYMODEM_V32_RATE_CAPABILITY_9600 |
				   SIPPYMODEM_V32_RATE_CAPABILITY_12000 |
				   SIPPYMODEM_V32_RATE_CAPABILITY_14400,
	V32_DETECTOR_WINDOW_DIBITS = 2 * SIPPYMODEM_V32_RATE_SIGNAL_DIBITS,
};

static bool signal_type_is_valid(enum sippymodem_v32_rate_signal_type type)
{
	return type == SIPPYMODEM_V32_RATE_SIGNAL ||
	       type == SIPPYMODEM_V32_END_SIGNAL;
}

static bool has_one_rate(uint8_t rates)
{
	return rates != 0 && (rates & (uint8_t)(rates - 1u)) == 0;
}

static uint8_t highest_rate(uint8_t rates)
{
	if ((rates & SIPPYMODEM_V32_RATE_CAPABILITY_14400) != 0)
		return SIPPYMODEM_V32_RATE_CAPABILITY_14400;
	if ((rates & SIPPYMODEM_V32_RATE_CAPABILITY_12000) != 0)
		return SIPPYMODEM_V32_RATE_CAPABILITY_12000;
	if ((rates & SIPPYMODEM_V32_RATE_CAPABILITY_9600) != 0)
		return SIPPYMODEM_V32_RATE_CAPABILITY_9600;
	if ((rates & SIPPYMODEM_V32_RATE_CAPABILITY_7200) != 0)
		return SIPPYMODEM_V32_RATE_CAPABILITY_7200;
	if ((rates & SIPPYMODEM_V32_RATE_CAPABILITY_4800) != 0)
		return SIPPYMODEM_V32_RATE_CAPABILITY_4800;
	return 0;
}

static bool rate_requires_trellis(uint8_t rate)
{
	return rate == SIPPYMODEM_V32_RATE_CAPABILITY_7200 ||
	       rate == SIPPYMODEM_V32_RATE_CAPABILITY_12000 ||
	       rate == SIPPYMODEM_V32_RATE_CAPABILITY_14400;
}

static bool offer_is_valid(
	const struct sippymodem_v32_rate_offer *offer,
	enum sippymodem_v32_rate_signal_type type)
{
	uint8_t highest;

	if (offer == NULL ||
	    (offer->rates & (uint8_t)~V32_RATE_CAPABILITY_MASK) != 0 ||
	    offer->rates == 0)
		return false;
	if (type == SIPPYMODEM_V32_END_SIGNAL && !has_one_rate(offer->rates))
		return false;
	highest = highest_rate(offer->rates);
	if (highest == SIPPYMODEM_V32_RATE_CAPABILITY_9600)
		return true;
	return offer->trellis_at_highest_rate == rate_requires_trellis(highest);
}

static uint16_t encode_rates(uint8_t rates)
{
	uint16_t word = 0;

	if ((rates & (SIPPYMODEM_V32_RATE_CAPABILITY_7200 |
		      SIPPYMODEM_V32_RATE_CAPABILITY_12000 |
		      SIPPYMODEM_V32_RATE_CAPABILITY_14400)) != 0)
		word |= V32_B4_2400_OR_BIS;

	if ((rates & SIPPYMODEM_V32_RATE_CAPABILITY_4800) != 0)
		word |= V32_B5_4800;
	if ((rates & SIPPYMODEM_V32_RATE_CAPABILITY_7200) != 0)
		word |= V32_B9_7200;
	if ((rates & SIPPYMODEM_V32_RATE_CAPABILITY_9600) != 0)
		word |= V32_B6_9600;
	if ((rates & SIPPYMODEM_V32_RATE_CAPABILITY_12000) != 0)
		word |= V32_B10_12000;
	if ((rates & SIPPYMODEM_V32_RATE_CAPABILITY_14400) != 0)
		word |= V32_B12_14400;
	return word;
}

enum sippymodem_v32_rate_signal_result sippymodem_v32_rate_signal_encode(
	enum sippymodem_v32_rate_signal_type type,
	const struct sippymodem_v32_rate_offer *offer, uint16_t *word)
{
	uint16_t encoded;

	if (!signal_type_is_valid(type) || word == NULL)
		return SIPPYMODEM_V32_RATE_SIGNAL_INVALID_ARGUMENT;
	if (!offer_is_valid(offer, type))
		return SIPPYMODEM_V32_RATE_SIGNAL_INVALID_OFFER;
	encoded = type == SIPPYMODEM_V32_RATE_SIGNAL ? V32_RATE_FIXED_BITS :
						      V32_END_FIXED_BITS;
	encoded |= encode_rates(offer->rates);
	if (offer->trellis_at_highest_rate)
		encoded |= V32_B8_TRELLIS;
	*word = encoded;
	return SIPPYMODEM_V32_RATE_SIGNAL_OK;
}

static uint8_t decode_rates(uint16_t word)
{
	uint8_t rates = 0;

	if ((word & V32_B5_4800) != 0)
		rates |= SIPPYMODEM_V32_RATE_CAPABILITY_4800;
	if ((word & V32_B9_7200) != 0)
		rates |= SIPPYMODEM_V32_RATE_CAPABILITY_7200;
	if ((word & V32_B6_9600) != 0)
		rates |= SIPPYMODEM_V32_RATE_CAPABILITY_9600;
	if ((word & V32_B10_12000) != 0)
		rates |= SIPPYMODEM_V32_RATE_CAPABILITY_12000;
	if ((word & V32_B12_14400) != 0)
		rates |= SIPPYMODEM_V32_RATE_CAPABILITY_14400;
	return rates;
}

enum sippymodem_v32_rate_signal_result sippymodem_v32_rate_signal_decode(
	uint16_t word, enum sippymodem_v32_rate_signal_type *type,
	struct sippymodem_v32_rate_offer *offer)
{
	struct sippymodem_v32_rate_offer decoded;
	enum sippymodem_v32_rate_signal_type decoded_type;
	uint16_t rate_fixed = word & (uint16_t)~V32_RATE_RECEIVE_MASK;
	uint16_t end_fixed = word & (uint16_t)~V32_KNOWN_PAYLOAD_MASK;

	if (type == NULL || offer == NULL)
		return SIPPYMODEM_V32_RATE_SIGNAL_INVALID_ARGUMENT;
	if (rate_fixed == V32_RATE_FIXED_BITS) {
		decoded_type = SIPPYMODEM_V32_RATE_SIGNAL;
	} else if (end_fixed == V32_END_FIXED_BITS) {
		decoded_type = SIPPYMODEM_V32_END_SIGNAL;
	} else {
		return SIPPYMODEM_V32_RATE_SIGNAL_INVALID_WORD;
	}
	decoded.rates = decode_rates(word);
	if ((word & V32_B4_2400_OR_BIS) == 0 &&
	    (decoded.rates &
	     (SIPPYMODEM_V32_RATE_CAPABILITY_7200 |
	      SIPPYMODEM_V32_RATE_CAPABILITY_12000 |
	      SIPPYMODEM_V32_RATE_CAPABILITY_14400)) != 0)
		return SIPPYMODEM_V32_RATE_SIGNAL_INVALID_WORD;
	decoded.trellis_at_highest_rate = (word & V32_B8_TRELLIS) != 0;
	if (!offer_is_valid(&decoded, decoded_type))
		return SIPPYMODEM_V32_RATE_SIGNAL_INVALID_WORD;
	*type = decoded_type;
	*offer = decoded;
	return SIPPYMODEM_V32_RATE_SIGNAL_OK;
}

enum sippymodem_v32_rate_signal_result sippymodem_v32_rate_signal_dibit(
	uint16_t word, unsigned int index, uint8_t *dibit)
{
	if (index >= SIPPYMODEM_V32_RATE_SIGNAL_DIBITS || dibit == NULL)
		return SIPPYMODEM_V32_RATE_SIGNAL_INVALID_ARGUMENT;
	*dibit = (uint8_t)((word >> (14u - index * 2u)) & 3u);
	return SIPPYMODEM_V32_RATE_SIGNAL_OK;
}

enum sippymodem_v32_rate_signal_result
sippymodem_v32_rate_signal_detector_init(
	struct sippymodem_v32_rate_signal_detector *detector)
{
	if (detector == NULL)
		return SIPPYMODEM_V32_RATE_SIGNAL_INVALID_ARGUMENT;
	*detector = (struct sippymodem_v32_rate_signal_detector){ 0 };
	return SIPPYMODEM_V32_RATE_SIGNAL_OK;
}

static bool word_is_rate_signal(uint16_t word)
{
	struct sippymodem_v32_rate_offer offer;
	enum sippymodem_v32_rate_signal_type type;

	return sippymodem_v32_rate_signal_decode(word, &type, &offer) ==
		       SIPPYMODEM_V32_RATE_SIGNAL_OK &&
	       type == SIPPYMODEM_V32_RATE_SIGNAL;
}

bool sippymodem_v32_rate_signal_detector_is_valid(
	const struct sippymodem_v32_rate_signal_detector *detector)
{
	if (detector == NULL ||
	    detector->dibits_seen > V32_DETECTOR_WINDOW_DIBITS ||
	    detector->dibits_since_boundary >=
		    SIPPYMODEM_V32_RATE_SIGNAL_DIBITS)
		return false;
	if (!detector->aligned)
		return detector->locked_rate_word == 0 &&
		       detector->dibits_since_boundary == 0;
	return detector->dibits_seen == V32_DETECTOR_WINDOW_DIBITS &&
	       word_is_rate_signal(detector->locked_rate_word);
}

static void lose_alignment(
	struct sippymodem_v32_rate_signal_detector *detector)
{
	detector->locked_rate_word = 0;
	detector->dibits_since_boundary = 0;
	detector->aligned = false;
}

static bool acquire_alignment(
	struct sippymodem_v32_rate_signal_detector *detector,
	struct sippymodem_v32_rate_offer *offer)
{
	uint16_t first = (uint16_t)(detector->window >> 16);
	uint16_t second = (uint16_t)detector->window;
	enum sippymodem_v32_rate_signal_type type;

	if (first != second ||
	    sippymodem_v32_rate_signal_decode(second, &type, offer) !=
		    SIPPYMODEM_V32_RATE_SIGNAL_OK ||
	    type != SIPPYMODEM_V32_RATE_SIGNAL)
		return false;
	detector->locked_rate_word = second;
	detector->dibits_since_boundary = 0;
	detector->aligned = true;
	return true;
}

static enum sippymodem_v32_rate_signal_event observe_aligned_word(
	struct sippymodem_v32_rate_signal_detector *detector,
	struct sippymodem_v32_rate_offer *offer)
{
	uint16_t word = (uint16_t)detector->window;
	enum sippymodem_v32_rate_signal_type type;

	if (sippymodem_v32_rate_signal_decode(word, &type, offer) !=
	    SIPPYMODEM_V32_RATE_SIGNAL_OK) {
		lose_alignment(detector);
		return SIPPYMODEM_V32_RATE_SIGNAL_NO_EVENT;
	}
	if (type == SIPPYMODEM_V32_END_SIGNAL) {
		(void)sippymodem_v32_rate_signal_detector_init(detector);
		return SIPPYMODEM_V32_END_SIGNAL_DETECTED;
	}
	if (word != detector->locked_rate_word) {
		lose_alignment(detector);
		return SIPPYMODEM_V32_RATE_SIGNAL_NO_EVENT;
	}
	return SIPPYMODEM_V32_RATE_SIGNAL_DETECTED;
}

enum sippymodem_v32_rate_signal_result
sippymodem_v32_rate_signal_detector_observe(
	struct sippymodem_v32_rate_signal_detector *detector, uint8_t dibit,
	enum sippymodem_v32_rate_signal_event *event,
	struct sippymodem_v32_rate_offer *offer)
{
	struct sippymodem_v32_rate_signal_detector next;
	struct sippymodem_v32_rate_offer decoded;
	enum sippymodem_v32_rate_signal_event observed =
		SIPPYMODEM_V32_RATE_SIGNAL_NO_EVENT;

	if (!sippymodem_v32_rate_signal_detector_is_valid(detector) ||
	    dibit >= 4 || event == NULL || offer == NULL)
		return SIPPYMODEM_V32_RATE_SIGNAL_INVALID_ARGUMENT;
	next = *detector;
	next.window = (next.window << 2) | dibit;
	if (next.dibits_seen < V32_DETECTOR_WINDOW_DIBITS)
		next.dibits_seen++;
	if (!next.aligned) {
		if (next.dibits_seen == V32_DETECTOR_WINDOW_DIBITS &&
		    acquire_alignment(&next, &decoded))
			observed = SIPPYMODEM_V32_RATE_SIGNAL_DETECTED;
	} else {
		next.dibits_since_boundary++;
		if (next.dibits_since_boundary ==
		    SIPPYMODEM_V32_RATE_SIGNAL_DIBITS) {
			next.dibits_since_boundary = 0;
			observed = observe_aligned_word(&next, &decoded);
		}
	}
	*detector = next;
	*event = observed;
	if (observed != SIPPYMODEM_V32_RATE_SIGNAL_NO_EVENT)
		*offer = decoded;
	return SIPPYMODEM_V32_RATE_SIGNAL_OK;
}

const char *sippymodem_v32_rate_signal_result_name(
	enum sippymodem_v32_rate_signal_result result)
{
	switch (result) {
	case SIPPYMODEM_V32_RATE_SIGNAL_OK:
		return "ok";
	case SIPPYMODEM_V32_RATE_SIGNAL_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V32_RATE_SIGNAL_INVALID_OFFER:
		return "invalid offer";
	case SIPPYMODEM_V32_RATE_SIGNAL_INVALID_WORD:
		return "invalid word";
	}
	return "unknown";
}
