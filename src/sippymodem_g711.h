// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_G711_H
#define SIPPYMODEM_G711_H

#include <stdint.h>

uint8_t sippymodem_g711_encode_pcmu(int16_t sample);
int16_t sippymodem_g711_decode_pcmu(uint8_t octet);

uint8_t sippymodem_g711_encode_pcma(int16_t sample);
int16_t sippymodem_g711_decode_pcma(uint8_t octet);

#endif
