// SPDX-License-Identifier: BSD-3-Clause
#ifndef MODEM_PJSIP_CONFIG_H
#define MODEM_PJSIP_CONFIG_H

/* One shared endpoint must serve multiple active lines and retired SIP
 * transactions while a line is reused. Keep headers and SDK objects in sync. */
#define PJSUA_MAX_CALLS 32
/* RTP/RTCP keys also survive briefly after SDK transport teardown. */
#define PJ_IOQUEUE_MAX_HANDLES 256
/* Release SDK assertions log and return their checked resource error rather
 * than aborting the embedding process on temporary I/O-key exhaustion. */
#define PJ_DEBUG 0

#endif
