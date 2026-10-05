// SPDX-License-Identifier: BSD-3-Clause
#ifndef DS0_CODEC_H
#define DS0_CODEC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hdlc_rx.h"
#define DS0_SAMPLE_RATE 8000u
#define DS0_RATE_TIMEOUT_SAMPLES (8u * DS0_SAMPLE_RATE)
#define DS0_MAX_FRAME_OCTETS 2048u

enum ds0_rate { DS0_RATE_NONE, DS0_RATE_56000 = 56000, DS0_RATE_64000 = 64000 };
enum ds0_rate_policy { DS0_RATE_AUTO, DS0_RATE_FORCE_56000, DS0_RATE_FORCE_64000 };
struct ds0_config {
    enum ds0_rate_policy rate_policy;
    size_t rx_frame_limit;
    uint8_t robbed_bit_fill;
};
struct ds0_rx_counters { uint32_t valid, fcs_fail, short_frame, invalid, overflow; };
struct ds0_counters {
    struct ds0_rx_counters rx56, rx64;
    uint32_t auto_switches, rate_timeout_fallbacks, media_gaps;
};
struct ds0_hdlc_rx {
    enum ds0_rate rate;
    struct hdlc_rx framing;
    /* Verified prefix ends at a flag/frame boundary or legal mark idle. */
    uint32_t clean_bits, verified_bits;
    bool clean_frame;
    uint8_t frame[DS0_MAX_FRAME_OCTETS + 2u];
};
struct ds0 {
    struct ds0_config cfg;
    struct ds0_counters counters;
    struct ds0_hdlc_rx rx64, rx56;
    enum ds0_rate tx_rate, selected_rate;
    uint64_t samples_since_start;
    bool saw_valid_frame, rate_timeout_done;
    uint8_t tx_flag_phase;
};
typedef void (*ds0_rx_frame_cb)(void *, const uint8_t *, size_t, enum ds0_rate);
void ds0_config_default(struct ds0_config *cfg);
int ds0_init(struct ds0 *, const struct ds0_config *);
/* Each codeword carries bits 7..0 at 64k, or bits 7..1 at 56k. */
size_t ds0_tx_frame(struct ds0 *, const uint8_t *, size_t, uint8_t *, size_t);
size_t ds0_tx_idle(struct ds0 *, uint8_t *, size_t);
int ds0_rx_codec(struct ds0 *, const uint8_t *, size_t, ds0_rx_frame_cb, void *);
/* A missing codeword invalidates partial frames; it cannot be replaced by silence. */
void ds0_rx_gap(struct ds0 *);
void ds0_advance_samples(struct ds0 *, size_t);
#endif
