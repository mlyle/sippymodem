// SPDX-License-Identifier: BSD-3-Clause

#ifndef BYTE_FINGERPRINT_H
#define BYTE_FINGERPRINT_H

#include <stddef.h>
#include <stdint.h>

/* Non-cryptographic guard for immutable, instance-owned encoded bytes. */
static inline uint32_t byte_fingerprint_mix_u32(uint32_t hash, uint32_t value)
{
	return (hash ^ value) * UINT32_C(16777619);
}

static inline uint32_t byte_fingerprint(const uint8_t *bytes, size_t count)
{
	uint32_t hash = UINT32_C(2166136261);

	for (size_t index = 0; index < count; ++index)
		hash = byte_fingerprint_mix_u32(hash, bytes[index]);
	return hash;
}

#endif
