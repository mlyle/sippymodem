// SPDX-License-Identifier: BSD-3-Clause
#include "pcm_octet_probe.h"
#include <string.h>

void pcm_octet_probe_init(struct pcm_octet_probe *probe)
{
	if (probe != NULL)
		memset(probe, 0, sizeof(*probe));
}

bool pcm_octet_probe_observe(struct pcm_octet_probe *probe, unsigned interval,
	uint8_t expected, uint8_t received)
{
	if (probe == NULL || interval >= 6)
		return false;
	probe->outputs[interval][expected][received / 8] |= 1u << (received % 8);
	uint8_t *count = &probe->observations[interval][expected];
	if (*count < 2)
		++*count;
	return true;
}

bool pcm_octet_probe_finish(const struct pcm_octet_probe *probe, enum g711_law law,
	struct pcm_constellation sets[6], struct pcm_octet_channel *channel)
{
	struct pcm_constellation next[6] = { 0 };
	struct pcm_octet_channel learned = { 0 };
	if (probe == NULL || sets == NULL || channel == NULL || !g711_law_is_valid(law))
		return false;
	for (unsigned i = 0; i < 6; ++i) {
		for (unsigned code = 0; code < 128; ++code) {
			uint8_t positive, negative;
			bool collision = false;
			if (!g711_ucode_octet(law, code, true, &positive) ||
			    !g711_ucode_octet(law, code, false, &negative))
				return false;
			if (probe->observations[i][positive] < 2 ||
			    probe->observations[i][negative] < 2)
				continue;
			const uint8_t *pos = probe->outputs[i][positive];
			const uint8_t *neg = probe->outputs[i][negative];
			for (unsigned byte = 0; byte < 32; ++byte)
				if ((pos[byte] & neg[byte]) ||
				    ((pos[byte] | neg[byte]) & learned.known[i][byte]))
					collision = true;
			if (collision)
				continue;
			pcm_constellation_add(&next[i], (uint8_t)code);
			for (unsigned received = 0; received < 256; ++received) {
				unsigned byte = received / 8, bit = 1u << (received % 8);
				if ((pos[byte] | neg[byte]) & bit) {
					learned.known[i][byte] |= bit;
					learned.decoded[i][received] = pos[byte] & bit ? positive : negative;
				}
			}
		}
		if (pcm_constellation_is_empty(&next[i]))
			return false;
	}
	memcpy(sets, next, sizeof(next));
	*channel = learned;
	return true;
}

bool pcm_octet_channel_normalize(const struct pcm_octet_channel *channel,
	unsigned interval, uint8_t received, uint8_t *normalized)
{
	if (channel == NULL || normalized == NULL || interval >= 6 ||
	    !(channel->known[interval][received / 8] & (1u << (received % 8))))
		return false;
	*normalized = channel->decoded[interval][received];
	return true;
}
