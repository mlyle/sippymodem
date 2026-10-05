// SPDX-License-Identifier: BSD-3-Clause
#include "ds0_probe.h"
#include <string.h>

int ds0_probe_init(struct ds0_probe *p, bool allow56, bool allow64)
{
	if (p == NULL || (!allow56 && !allow64)) return -1;
	memset(p, 0, sizeof(*p));
	p->allowed[0] = allow56; p->allowed[1] = allow64;
	struct ds0_config cfg; ds0_config_default(&cfg);
	cfg.rate_policy = DS0_RATE_FORCE_56000;
	if (ds0_init(&p->rx[0], &cfg) != 0) return -1;
	cfg.rate_policy = DS0_RATE_FORCE_64000;
	return ds0_init(&p->rx[1], &cfg);
}

enum ds0_rate ds0_probe_sample(struct ds0_probe *p, uint8_t octet, bool available)
{
	if (p == NULL) return DS0_RATE_NONE;
	if (!available) {
		ds0_rx_gap(&p->rx[0]); ds0_rx_gap(&p->rx[1]);
		p->head = p->count = p->retained = 0;
		p->selected = DS0_RATE_NONE;
		return DS0_RATE_NONE;
	}
	p->history[p->head] = octet;
	p->head = (p->head + 1u) % DS0_PROBE_HISTORY;
	if (p->count < DS0_PROBE_HISTORY) p->count++;
	if (p->selected != DS0_RATE_NONE) {
		/* Preserve the remainder of the selecting DSP fragment too. */
		if (p->retained < DS0_PROBE_HISTORY) p->retained++;
		else p->selected = DS0_RATE_NONE;
		return p->selected;
	}
	for (unsigned i = 0; i < 2; ++i) {
		if (!p->allowed[i]) continue;
		ds0_rx_codec(&p->rx[i], &octet, 1, NULL, NULL);
		const struct ds0_hdlc_rx *r = i == 0 ? &p->rx[i].rx56 : &p->rx[i].rx64;
		unsigned bits = i == 0 ? 7u : 8u;
		/* Exclude partial codewords at both verified-run boundaries. */
		if (r->clean_frame && r->verified_bits >=
		    DS0_PROBE_MIN_CODEWORDS * bits + 2u * (bits - 1u)) {
			p->selected = r->rate;
			p->retained = ((size_t)r->clean_bits + bits - 1u) / bits + 1u;
			if (p->retained > p->count) p->retained = p->count;
			if (p->retained > DS0_PROBE_HISTORY - DS0_PROBE_TAIL_CODEWORDS)
				p->retained = DS0_PROBE_HISTORY - DS0_PROBE_TAIL_CODEWORDS;
			return p->selected;
		}
	}
	return DS0_RATE_NONE;
}

size_t ds0_probe_copy(const struct ds0_probe *p, uint8_t *out, size_t capacity)
{
	if (p == NULL || out == NULL || p->selected == DS0_RATE_NONE ||
	    p->retained == 0 || p->retained > capacity) return 0;
	size_t start = (p->head + DS0_PROBE_HISTORY - p->retained) % DS0_PROBE_HISTORY;
	for (size_t i = 0; i < p->retained; ++i)
		out[i] = p->history[(start + i) % DS0_PROBE_HISTORY];
	return p->retained;
}
