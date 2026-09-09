// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_CLEARDOWN_H
#define V92_CLEARDOWN_H

#include <stdbool.h>
#include <stdint.h>

/* V.92 Amendment 1, 9.11. End positions are physical 8 kHz symbol counts,
 * not the earlier instants at which a buffered coder accepts a CP record.
 * The line owner finishes its current record and drains queued output after
 * ready becomes true. Only a CRC-checked CP, not SUV, acknowledges cleardown. */
struct v92_cleardown {
	uint64_t cp_end;
	uint64_t acknowledgement_end;
	uint16_t round_trip_delay;
	bool active;
	bool initiator;
	bool peer_requested;
	bool acknowledged;
};

bool v92_cleardown_is_valid(const struct v92_cleardown *state);
/* The line owner validates state and decoded records before these operations. */
void v92_cleardown_start(struct v92_cleardown *state,
	uint16_t round_trip_delay, bool initiator);
void v92_cleardown_receive(struct v92_cleardown *state,
	bool zero_rate, bool acknowledges, uint64_t transmitted_symbols);
void v92_cleardown_sent(struct v92_cleardown *state,
	uint64_t end_symbol, bool acknowledges);
bool v92_cleardown_is_ready(const struct v92_cleardown *state,
	uint64_t transmitted_symbols);

#endif /* V92_CLEARDOWN_H */
