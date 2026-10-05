// SPDX-License-Identifier: BSD-3-Clause
#ifndef MODEM_FRONTEND_H
#define MODEM_FRONTEND_H

struct modem;

/* Opt into AT parsing, result rendering and escape detection. The modem core
 * and public byte API do not install this compatibility frontend. */
void modem_print_version(void);
void modem_frontend_init(struct modem *modem);

#endif
