// SPDX-License-Identifier: BSD-3-Clause

#ifndef V21_H
#define V21_H

#include "fsk.h"

enum {
	V21_SAMPLE_RATE = 8000,
	V21_BIT_RATE = 300,
};

/* V.21 channel 1 is transmitted by the originating station; channel 2 is
 * transmitted by the answering station. Mark is binary one on both. */
extern const struct fsk_channel v21_channel_1;
extern const struct fsk_channel v21_channel_2;

#endif /* V21_H */
