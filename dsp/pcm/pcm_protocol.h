// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_PROTOCOL_H
#define PCM_PROTOCOL_H

#include <stdbool.h>

enum pcm_protocol {
	PCM_PROTOCOL_V90,
	PCM_PROTOCOL_V92,
};

enum v90_modem_role {
	V90_ANALOG_MODEM,
	V90_DIGITAL_MODEM,
};

static inline bool pcm_protocol_is_valid(enum pcm_protocol protocol)
{
	return protocol == PCM_PROTOCOL_V90 || protocol == PCM_PROTOCOL_V92;
}

static inline bool pcm_modem_role_is_valid(enum v90_modem_role role)
{
	return role == V90_ANALOG_MODEM || role == V90_DIGITAL_MODEM;
}

#endif /* PCM_PROTOCOL_H */
