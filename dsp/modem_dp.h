/*
 *
 *    Copyright (c) 2002, Smart Link Ltd.
 *    All rights reserved.
 *
 *    Redistribution and use in source and binary forms, with or without
 *    modification, are permitted provided that the following conditions
 *    are met:
 *
 *        1. Redistributions of source code must retain the above copyright
 *           notice, this list of conditions and the following disclaimer.
 *        2. Redistributions in binary form must reproduce the above
 *           copyright notice, this list of conditions and the following
 *           disclaimer in the documentation and/or other materials provided
 *           with the distribution.
 *        3. Neither the name of the Smart Link Ltd. nor the names of its
 *           contributors may be used to endorse or promote products derived
 *           from this software without specific prior written permission.
 *
 *    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *    OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 *
 *    modem_dp.h - modem data pumps interface definitions
 *
 *    Author: sashak@smlink.com
 */

#ifndef __MODEM_DP_H__
#define __MODEM_DP_H__

#include <g711.h>
#include <modem_defs.h>

#include <stdbool.h>


/*
 *    type definitions
 *
 */


/* data pump operations structure */
struct dp_operations {
	const char *name;
	int use_count;
	/* dp interface */
	struct dp *(*create)(struct modem *, enum DP_ID id,
			     int caller, int srate, int max_frag,
			     struct dp_operations *);
	int (*delete)(struct dp *);
	int (*process)(struct dp *, void *in, void *out, int cnt);
	/* The caller initializes exact_output to false. An implementation may
	 * set it only after producing every transmit codeword in this fragment;
	 * otherwise the linear transmit samples remain authoritative.
	 * received_mask is borrowed for cnt samples: false means generated playout,
	 * not a received zero symbol. NULL means provenance is unavailable. */
	int (*process_g711)(struct dp *, enum g711_law law,
			     const u8 *receive_codewords, void *receive_samples,
			     const bool *received_mask,
			     u8 *transmit_codewords, void *transmit_samples,
			     int cnt, bool *exact_output);
	/* Start an in-band retrain without releasing the data-link state. */
	int (*retrain)(struct dp *);
	/* Begin a bounded in-band cleardown.  A successful callback keeps the
	 * datapump runnable until process() or process_g711() returns
	 * DPSTAT_DISCONNECT after its final line symbol has drained. */
	int (*hangup)(struct dp *);
};

/* data pump structure */
struct dp {
        enum DP_ID id;
        struct modem *modem;
        unsigned status;
	struct dp_operations *op;
        void *dp_data;
};


/*
 *    prototypes
 *
 */

/* bit interface */
extern int modem_get_bits(struct modem *m, int nbits, u8 *buf, int n);
extern int modem_put_bits(struct modem *m, int nbits, u8 *buf, int n);

/* modem dp registration */
extern int  modem_dp_register(enum DP_ID id, struct dp_operations *op);
extern int modem_dp_deregister(enum DP_ID id, struct dp_operations *op);
extern struct dp_operations *modem_dp_get_registered_ops(enum DP_ID id);


#endif /* __MODEM_DP_H__ */
