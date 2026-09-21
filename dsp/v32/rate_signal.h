// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_RATE_SIGNAL_H
#define V32_RATE_SIGNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V32_RATE_SIGNAL_BITS = 16,
	V32_RATE_SIGNAL_DIBITS = 8,
};

enum v32_rate_capability {
	V32_RATE_CAPABILITY_4800 = 1u << 0,
	V32_RATE_CAPABILITY_7200 = 1u << 1,
	V32_RATE_CAPABILITY_9600 = 1u << 2,
	V32_RATE_CAPABILITY_12000 = 1u << 3,
	V32_RATE_CAPABILITY_14400 = 1u << 4,
};

enum v32_rate_signal_type {
	V32_RATE_SIGNAL,
	V32_END_SIGNAL,
};

enum v32_rate_signal_result {
	V32_RATE_SIGNAL_OK,
	V32_RATE_SIGNAL_INVALID_ARGUMENT,
	V32_RATE_SIGNAL_INVALID_OFFER,
	V32_RATE_SIGNAL_INVALID_WORD,
};

enum v32_rate_signal_event {
	V32_RATE_SIGNAL_NO_EVENT,
	V32_RATE_SIGNAL_DETECTED,
	V32_END_SIGNAL_DETECTED,
};

struct v32_rate_offer {
	uint8_t rates;
	bool trellis_at_highest_rate;
};

struct v32_rate_signal_detector {
	uint32_t window;
	uint16_t locked_rate_word;
	uint8_t dibits_seen;
	uint8_t dibits_since_boundary;
	bool aligned;
};

/* Return the highest known capability bit, or zero if none is set. */
static inline uint8_t v32_rate_highest(uint8_t rates)
{
	if ((rates & V32_RATE_CAPABILITY_14400) != 0)
		return V32_RATE_CAPABILITY_14400;
	if ((rates & V32_RATE_CAPABILITY_12000) != 0)
		return V32_RATE_CAPABILITY_12000;
	if ((rates & V32_RATE_CAPABILITY_9600) != 0)
		return V32_RATE_CAPABILITY_9600;
	if ((rates & V32_RATE_CAPABILITY_7200) != 0)
		return V32_RATE_CAPABILITY_7200;
	if ((rates & V32_RATE_CAPABILITY_4800) != 0)
		return V32_RATE_CAPABILITY_4800;
	return 0;
}

/* Compare initialized offers by value, without imposing negotiation policy. */
static inline bool v32_rate_offers_match(
	const struct v32_rate_offer *left,
	const struct v32_rate_offer *right)
{
	return left->rates == right->rates &&
	       left->trellis_at_highest_rate ==
		       right->trellis_at_highest_rate;
}

/* Rate words are represented in transmission order: B0 is bit 15. */
enum v32_rate_signal_result v32_rate_signal_encode(
	enum v32_rate_signal_type type,
	const struct v32_rate_offer *offer, uint16_t *word);
enum v32_rate_signal_result v32_rate_signal_decode(
	uint16_t word, enum v32_rate_signal_type *type,
	struct v32_rate_offer *offer);

/* Read an aligned retained rate word. Output is meaningful only on success;
 * the path owner separately validates the detector's counters and window. */
static inline bool v32_rate_signal_detector_offer(
	const struct v32_rate_signal_detector *detector,
	struct v32_rate_offer *offer)
{
	enum v32_rate_signal_type type;

	return detector != NULL && detector->aligned && offer != NULL &&
	       v32_rate_signal_decode(
		       detector->locked_rate_word, &type, offer) ==
		       V32_RATE_SIGNAL_OK &&
	       type == V32_RATE_SIGNAL;
}

enum v32_rate_signal_result v32_rate_signal_dibit(
	uint16_t word, unsigned int index, uint8_t *dibit);

enum v32_rate_signal_result
v32_rate_signal_detector_init(
	struct v32_rate_signal_detector *detector);
bool v32_rate_signal_detector_is_valid(
	const struct v32_rate_signal_detector *detector);
enum v32_rate_signal_result
v32_rate_signal_detector_observe(
	struct v32_rate_signal_detector *detector, uint8_t dibit,
	enum v32_rate_signal_event *event,
	struct v32_rate_offer *offer);

#endif
