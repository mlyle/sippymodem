// SPDX-License-Identifier: BSD-3-Clause
#ifndef V91_CONTROL_H
#define V91_CONTROL_H

#include "g711.h"
#include "pcm_constellation.h"

/* V.91 Tables 4 and 6. Packed records use MSB-first storage; numeric fields
 * retain their specified least-significant-bit-first wire order. */
enum {
	V91_INFO_BITS = 62, V91_INFO_BYTES = 8,
	V91_CP_MAX_BITS = 972, V91_CP_MAX_BYTES = 122,
};

struct v91_info {
	enum g711_law law;
	uint8_t maximum_power;
	bool custom_dil, control_channel, acknowledges_peer;
	bool receiver_power, transparent, require_transparent;
};

struct v91_cp {
	struct pcm_constellation constellations[6];
	uint8_t frame_constellation[6];
	uint8_t count, rate;
	bool acknowledges_peer, transparent;
};

bool v91_info_encode(const struct v91_info *info, uint8_t frame[V91_INFO_BYTES]);
bool v91_info_decode(const uint8_t frame[V91_INFO_BYTES], struct v91_info *info);
/* A zero length indicates an invalid record; valid lengths include zero fill
 * through the next six-bit boundary. Decode commits only after CRC/framing. */
size_t v91_cp_encode(const struct v91_cp *cp, uint8_t frame[V91_CP_MAX_BYTES]);
bool v91_cp_decode(const uint8_t *frame, size_t bits, struct v91_cp *cp);
size_t v91_cp_length(const uint8_t *header, size_t bits);
/* The optional 96-bit control sequence uses six unscrambled sign bits
 * every 64th data frame. Secondary data defaults to zero; receive ignores it. */
bool v91_control_encode(uint8_t frame[12], bool retrain, bool cleardown);
bool v91_control_decode(const uint8_t frame[12], bool *retrain, bool *cleardown);

/* Sliding differential INFO acquisition. Two adjacent, identical valid
 * records qualify a retrain request while another receive mode is active. */
struct v91_info_receiver {
	uint8_t frame[V91_INFO_BYTES], previous[V91_INFO_BYTES];
	uint32_t clock, last_frame;
	uint16_t sync;
	uint8_t position, previous_sign, consecutive;
};
void v91_info_receiver_init(struct v91_info_receiver *receiver);
bool v91_info_receiver_sample(struct v91_info_receiver *receiver, bool positive,
	struct v91_info *info);
#endif /* V91_CONTROL_H */
