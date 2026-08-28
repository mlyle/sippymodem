// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_STATION_H
#define V34_STATION_H

#include <stdbool.h>

enum v34_station_role {
	V34_CALLING_STATION,
	V34_ANSWERING_STATION,
};

static inline bool v34_station_role_is_valid(enum v34_station_role role)
{
	return role == V34_CALLING_STATION ||
	       role == V34_ANSWERING_STATION;
}

#endif
