// SPDX-License-Identifier: BSD-3-Clause
#ifndef MODEM_STATE_H
#define MODEM_STATE_H

/* Internal call states shared with the AT compatibility frontend. */
enum modem_state {
	STATE_MODEM_IDLE = 0x1,
	STATE_DP_ESTAB = 0x2,
	STATE_EC_ESTAB = 0x4,
	STATE_MODEM_ONLINE = 0x5,
	STATE_COMMAND_ONLINE = 0x6,
	STATE_EC_DISC = 0x7,
	STATE_DP_DISC = 0x9,
};

enum MODEM_RESULT {
	RESULT_OK = 0,
	RESULT_CONNECT = 1,
	RESULT_RING = 2,
	RESULT_NOCARRIER = 3,
	RESULT_ERROR = 4,
	RESULT_UNUSED_5 = 5,
	RESULT_NODIALTONE = 6,
	RESULT_BUSY = 7,
	RESULT_NOANSWER = 8,
};

#endif
