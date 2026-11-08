// SPDX-License-Identifier: BSD-3-Clause
#include "v92_survivor.h"
#include <limits.h>
#include <string.h>

_Static_assert(V92_PCM_MAPPING_MIN_BITS >= 23, "two future words cover GPA memory");

struct branch {
	uint64_t cost, alternate;
	struct v92_survivor_word word;
	int16_t eta[4];
};
static uint64_t add_cost(uint64_t a, uint64_t b)
{
	return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

static int word_equal(struct v92_survivor_word a, struct v92_survivor_word b)
{
	return a.lo == b.lo && a.hi == b.hi;
}

static struct v92_survivor_word word_or(struct v92_survivor_word a, struct v92_survivor_word b)
{
	return (struct v92_survivor_word){a.lo | b.lo, a.hi | b.hi};
}

static unsigned word_bit(struct v92_survivor_word a, unsigned at)
{
	return at < 64 ? (a.lo >> at) & 1 : (a.hi >> (at - 64)) & 1;
}

static void word_set(struct v92_survivor_word *a, unsigned at, unsigned bit)
{
	if (at < 64)
		a->lo |= (uint64_t)bit << at;
	else
		a->hi |= (uint64_t)bit << (at - 64);
}

static unsigned positive_mod(int eta, unsigned modulus)
{
	return (eta % (int)modulus + (int)modulus) % modulus;
}

static unsigned branch_index(enum v34_trellis_selection s, unsigned y0,
			     const struct v34_convolutional_input *i)
{
	unsigned index = y0 | (i->y1 << 1) | (i->y2 << 2);
	if (s == V34_TRELLIS_32_STATE)
		index |= i->y4 << 3;
	if (s == V34_TRELLIS_64_STATE)
		index |= (i->y3 << 3) | (i->y4 << 4);
	return index;
}

bool v92_survivor_init(struct v92_survivor *s, const struct v92_pcm_mapping *m)
{
	if (!s || !v92_pcm_mapping_is_valid(m) ||
	    m->data_bits_per_frame < V92_PCM_MAPPING_MIN_BITS ||
	    m->data_bits_per_frame > V92_PCM_MAPPING_MAX_BITS)
		return 0;
	struct v92_survivor *output = s, initialized;
	s = &initialized;
	memset(s, 0, sizeof *s);
	s->selection = (enum v34_trellis_selection)m->cpd.trellis;
	s->states = v34_trellis_state_count(s->selection);
	s->branches = v34_trellis_branch_count(s->selection);
	s->data_bits = m->data_bits_per_frame;
	unsigned bits = 0;
	for (unsigned i = 0; i < 12; i++) {
		unsigned radix = m->cpd.modulus[i];
		if (!radix || (radix & (radix - 1)))
			return 0;
		s->offset[i] = bits;
		while (radix > 1) {
			radix >>= 1;
			s->bits[i]++;
			bits++;
		}
		s->count[i] = m->cpd.constellation[m->cpd.frame_constellation[i % 6]].count;
	}
	if (bits != s->data_bits)
		return 0;
	for (unsigned i = 0; i < 12; i++)
		for (unsigned bit = 0; bit < s->bits[i]; bit++) {
			word_set(&s->rm_word, s->offset[i] + bit, i % 4 < 2);
			word_set(&s->prime_word, s->offset[i] + bit, i % 4 >= 2);
		}
	for (unsigned state = 0; state < V92_SURVIVOR_STATES; state++) {
		s->metric[state] = state ? UINT64_MAX : 0;
		for (unsigned i = 0; i < V92_SURVIVOR_MARGINS; i++)
			s->context[state].margin[i] = UINT64_MAX;
		for (unsigned branch = 0; branch < s->branches; branch++) {
			struct v34_convolutional_input input = {
			    .y1 = (branch >> 1) & 1,
			    .y2 = (branch >> 2) & 1,
			    .y3 = s->selection == V34_TRELLIS_64_STATE ? (branch >> 3) & 1 : 0,
			    .y4 = s->selection == V34_TRELLIS_32_STATE	 ? (branch >> 3) & 1
				  : s->selection == V34_TRELLIS_64_STATE ? (branch >> 4) & 1
									 : 0};
			s->next[state][branch] =
			    state < s->states && ((state & 1) == (branch & 1))
				? v34_convolutional_next_state_owned(s->selection, state, &input)
				: UINT8_MAX;
		}
	}
	for (unsigned word = 0; word < 256; word++) {
		unsigned e[4], parity = 0;
		for (unsigned i = 0; i < 4; i++) {
			e[i] = (word >> (2 * i)) & 3;
			parity ^= e[i] & 1;
		}
		struct v34_grid_point first = {2 * e[0] + 1, 2 * e[1] + 1},
				      second = {2 * e[2] + 1, 2 * e[3] + 1};
		uint8_t a, b;
		struct v34_convolutional_input input;
		if (v34_grid_subset_label(&first, &a) || v34_grid_subset_label(&second, &b) ||
		    v34_convolutional_input_from_subset_labels(a, b, &input))
			return 0;
		s->classes[word] = branch_index(s->selection, parity, &input);
	}
	*output = *s;
	return 1;
}
/* The caller supplies the matching control boundary. Metrics, physical
 * trellis, pending payload and its B1 evidence survive the coding reset. */
void v92_survivor_reset_control_owned(struct v92_survivor *s)
{
	for (unsigned n = 0; n < s->states; n++)
		if (s->metric[n] != UINT64_MAX) {
			s->context[n].gpa = 0;
			s->context[n].sign = 0;
			s->context[n].rm_frames = 0;
			s->context[n].rm_prime_frames = 0;
		}
}
/* Coding-digit alternatives with identical physical next state cannot gain
 * later independent physical evidence. Retain their cost gap instead of
 * erasing pending-source disagreement. Partial-word/GPA differences mark the
 * current and two following source words conservatively, covering a changed
 * frame reflection followed by the23-bit GPA memory. */
static void retain_future_warning(struct v92_survivor_context *c, unsigned fill, uint64_t gap)
{
	for (unsigned i = fill; i <= fill + 2; i++)
		if (gap < c->margin[i])
			c->margin[i] = gap;
}

static void build_branches(const struct v92_survivor *s, unsigned group,
			   const uint64_t costs[12][V92_SURVIVOR_POINTS], struct branch *b)
{
	struct {
		uint64_t cost, alternate;
		int eta;
		unsigned digit;
	} choice[4][4];
	for (unsigned i = 0; i < 4; i++)
		for (unsigned residue = 0; residue < 4; residue++) {
			unsigned symbol = 4 * group + i;
			uint64_t best = UINT64_MAX, alternate = UINT64_MAX;
			int selected = 0;
			unsigned digit = 0;
			for (int eta = -(int)s->count[symbol]; eta < (int)s->count[symbol]; eta++)
				if (positive_mod(eta, 4) == residue) {
					uint64_t cost = costs[symbol][eta + s->count[symbol]];
					unsigned value =
					    positive_mod(i == 3 ? (eta - (eta & 1)) / 2 : eta,
							 1u << s->bits[symbol]);
					if (cost < best) {
						best = cost;
						selected = eta;
						digit = value;
					}
				}
			/* Recompute the nearest different coding digit after the winner changes. */
			alternate = UINT64_MAX;
			if (best != UINT64_MAX)
				for (int eta = -(int)s->count[symbol]; eta < (int)s->count[symbol];
				     eta++)
					if (positive_mod(eta, 4) == residue) {
						unsigned value = positive_mod(
						    i == 3 ? (eta - (eta & 1)) / 2 : eta,
						    1u << s->bits[symbol]);
						if (value != digit &&
						    costs[symbol][eta + s->count[symbol]] <
							alternate)
							alternate =
							    costs[symbol][eta + s->count[symbol]];
					}
			choice[i][residue].cost = best;
			choice[i][residue].alternate = alternate;
			choice[i][residue].eta = selected;
			choice[i][residue].digit = digit;
		}
	for (unsigned j = 0; j < s->branches; j++)
		b[j] = (struct branch){.cost = UINT64_MAX, .alternate = UINT64_MAX};
	for (unsigned word = 0; word < 256; word++) {
		struct branch candidate = {.alternate = UINT64_MAX};
		uint64_t delta = UINT64_MAX;
		for (unsigned i = 0; i < 4; i++) {
			unsigned r = (word >> (2 * i)) & 3, symbol = 4 * group + i;
			uint64_t cost = choice[i][r].cost;
			candidate.cost = add_cost(candidate.cost, cost);
			candidate.eta[i] = choice[i][r].eta;
			if (cost != UINT64_MAX && choice[i][r].alternate != UINT64_MAX &&
			    choice[i][r].alternate - cost < delta)
				delta = choice[i][r].alternate - cost;
			for (unsigned bit = 0; bit < s->bits[symbol]; bit++)
				word_set(&candidate.word, s->offset[symbol] + bit,
					 (choice[i][r].digit >> bit) & 1);
		}
		if (candidate.cost == UINT64_MAX)
			continue;
		candidate.alternate = add_cost(candidate.cost, delta);
		struct branch *slot = b + s->classes[word];
		if (candidate.cost < slot->cost) {
			struct branch old = *slot;
			*slot = candidate;
			uint64_t alt =
			    word_equal(old.word, candidate.word) ? old.alternate : old.cost;
			if (alt < slot->alternate)
				slot->alternate = alt;
		} else {
			uint64_t alt = word_equal(slot->word, candidate.word) ? candidate.alternate
									      : candidate.cost;
			if (alt < slot->alternate)
				slot->alternate = alt;
		}
	}
}

static int advance_group(struct v92_survivor *s, unsigned group, const struct branch *b,
			 uint64_t *removed)
{
	uint64_t metric[V92_SURVIVOR_STATES];
	uint8_t origin[V92_SURVIVOR_STATES], selected[V92_SURVIVOR_STATES];
	struct v92_survivor_context next[V92_SURVIVOR_STATES];
	for (unsigned n = 0; n < s->states; n++) {
		metric[n] = UINT64_MAX;
		origin[n] = selected[n] = 0;
	}
	for (unsigned state = 0; state < s->states; state++)
		if (s->metric[state] != UINT64_MAX)
			for (unsigned branch = 0; branch < s->branches; branch++) {
				unsigned n = s->next[state][branch];
				if (n == UINT8_MAX)
					continue;
				uint64_t value = add_cost(s->metric[state], b[branch].cost);
				if (value < metric[n]) {
					metric[n] = value;
					origin[n] = state;
					selected[n] = branch;
				}
			}
	uint64_t minimum = UINT64_MAX;
	for (unsigned n = 0; n < s->states; n++)
		if (metric[n] != UINT64_MAX) {
			if (metric[n] < minimum)
				minimum = metric[n];
			unsigned branch = selected[n];
			next[n] = s->context[origin[n]];
			next[n].encoded = word_or(next[n].encoded, b[branch].word);
			memcpy(next[n].eta + group * 4, b[branch].eta, sizeof b[branch].eta);
			if (b[branch].alternate != UINT64_MAX)
				retain_future_warning(next + n, s->fill,
						      b[branch].alternate - b[branch].cost);
		}
	if (minimum == UINT64_MAX)
		return 0;
	for (unsigned state = 0; state < s->states; state++)
		if (s->metric[state] != UINT64_MAX)
			for (unsigned branch = 0; branch < s->branches; branch++) {
				unsigned n = s->next[state][branch];
				if (n == UINT8_MAX || b[branch].cost == UINT64_MAX ||
				    (origin[n] == state && selected[n] == branch))
					continue;
				uint64_t value = add_cost(s->metric[state], b[branch].cost);
				if (value == UINT64_MAX)
					continue;
				uint64_t gap = value - metric[n];
				/* Once physical histories merge, their future physical costs agree.
				 * Retain the gap in native score units for either caller policy. */
				const struct v92_survivor_context *loser = s->context + state;
				struct v92_survivor_context *winner = next + n;
				struct v92_survivor_word word =
				    word_or(loser->encoded, b[branch].word);
				for (unsigned i = 0; i < V92_SURVIVOR_MARGINS; i++) {
					uint64_t inherited = loser->margin[i];
					if (i >= s->fill && i <= s->fill + 2u &&
					    b[branch].alternate != UINT64_MAX &&
					    b[branch].alternate - b[branch].cost < inherited)
						inherited = b[branch].alternate - b[branch].cost;
					uint64_t alt =
					    i < s->fill &&
						    (!word_equal(winner->pending[i],
								 loser->pending[i]) ||
						     (((winner->nondata ^ loser->nondata) >> i) &
						      1))
						? gap
						: add_cost(gap, inherited);
					if (alt < winner->margin[i])
						winner->margin[i] = alt;
				}
				if (gap == 0)
					winner->code_mismatches |= loser->code_mismatches;
				if (loser->gpa != winner->gpa || loser->sign != winner->sign ||
				    !word_equal(word, winner->encoded) ||
				    loser->rm_frames != winner->rm_frames ||
				    loser->rm_prime_frames != winner->rm_prime_frames)
					retain_future_warning(winner, s->fill, gap);
			}
	for (unsigned n = 0; n < s->states; n++) {
		s->metric[n] = metric[n] == UINT64_MAX ? UINT64_MAX : metric[n] - minimum;
		if (metric[n] != UINT64_MAX)
			s->context[n] = next[n];
	}
	*removed = minimum;
	return 1;
}

static unsigned request_state(const struct v92_survivor_context *c)
{
	if (c->rm_frames < V92_PCM_RM_QUALIFY_FRAMES)
		return V92_PCM_FRAME_RM_NONE;
	return c->rm_prime_frames == 0	 ? V92_PCM_FRAME_RM_SIGNAL
	       : c->rm_prime_frames == 1 ? V92_PCM_FRAME_RM_PRIME
					 : V92_PCM_FRAME_RM_COMPLETE;
}
/* Request evidence uses observed modulus words, never expected B1 bits.
 * Independent physical costs can merge coding/control alternatives. A
 * tied current word cannot qualify a request; it starts a fresh run. Soft
 * distance uncertainty stays in native score units for the caller. */
static void observe_request(struct v92_survivor *s, struct v92_survivor_context *c, bool b1u)
{
	bool confident = !b1u && c->margin[s->fill] > 0;
	bool rm = confident && word_equal(c->encoded, s->rm_word),
	     prime = confident && word_equal(c->encoded, s->prime_word);
	if (rm) {
		if (c->rm_prime_frames)
			c->rm_frames = 0;
		if (c->rm_frames < V92_PCM_RM_QUALIFY_FRAMES)
			c->rm_frames++;
		c->rm_prime_frames = 0;
	} else if (prime && c->rm_frames == V92_PCM_RM_QUALIFY_FRAMES) {
		if (c->rm_prime_frames < 2)
			c->rm_prime_frames++;
	} else
		c->rm_frames = c->rm_prime_frames = 0;
	if (request_state(c) != V92_PCM_FRAME_RM_NONE)
		c->nondata = (uint16_t)((1u << s->fill) - 1);
}
/* Validated epoch owner: scratch/output are disjoint from state/costs.
 * All fallible work is staged; no pointer survives the call. */
bool v92_survivor_advance_owned(struct v92_survivor *s,
				const uint64_t costs[12][V92_SURVIVOR_POINTS], bool b1u,
				const struct v92_survivor_word *expected,
				struct v92_survivor_output *out, struct v92_survivor *scratch)
{
	*scratch = *s;
	struct branch branches[V92_SURVIVOR_BRANCHES];
	uint64_t total = 0;
	for (unsigned group = 0; group < 3; group++) {
		uint64_t removed;
		build_branches(scratch, group, costs, branches);
		if (!advance_group(scratch, group, branches, &removed))
			return false;
		total = add_cost(total, removed);
	}
	for (unsigned n = 0; n < scratch->states; n++)
		if (scratch->metric[n] != UINT64_MAX)
			observe_request(scratch, scratch->context + n, b1u);
	unsigned best = 0;
	for (unsigned n = 1; n < scratch->states; n++)
		if (scratch->metric[n] < scratch->metric[best])
			best = n;
	struct v92_survivor_output result = {.cost = total,
					     .margin = UINT64_MAX,
					     .uncertainty_margin = UINT64_MAX,
					     .ready = scratch->fill == V92_PCM_RX_DELAY_FRAMES};
	memcpy(result.eta, scratch->context[best].eta, sizeof result.eta);
	result.rm = request_state(scratch->context + best);
	for (unsigned n = 0; n < scratch->states; n++)
		if (scratch->metric[n] != UINT64_MAX &&
		    scratch->metric[n] == scratch->metric[best] &&
		    request_state(scratch->context + n) != result.rm)
			result.rm = V92_PCM_FRAME_RM_NONE;
	if (result.ready) {
		const struct v92_survivor_context *chosen = scratch->context + best;
		result.word = chosen->pending[0];
		result.margin = chosen->margin[0];
		result.ready = !(chosen->nondata & 1);
		result.code_mismatch = (chosen->code_mismatches & 1) != 0;
		for (unsigned n = 0; n < scratch->states; n++)
			if (scratch->metric[n] != UINT64_MAX) {
				const struct v92_survivor_context *other = scratch->context + n;
				uint64_t gap = scratch->metric[n] - scratch->metric[best];
				uint64_t margin = !word_equal(result.word, other->pending[0]) ||
							  ((chosen->nondata ^ other->nondata) & 1)
						      ? gap
						      : add_cost(gap, other->margin[0]);
				if (margin < result.margin)
					result.margin = margin;
				if (gap == 0 && (other->code_mismatches & 1))
					result.code_mismatch = true;
			}
	}
	/* Keep exact cost gaps in the supplied score units. Squared-distance
	 * uncertainty and likelihood ambiguity have distinct caller policies. */
	const struct v92_survivor_context *chosen = scratch->context + best;
	for (unsigned i = 0; i <= scratch->fill + 2u; i++)
		if (chosen->margin[i] < result.uncertainty_margin)
			result.uncertainty_margin = chosen->margin[i];
	for (unsigned n = 0; n < scratch->states; n++)
		if (scratch->metric[n] != UINT64_MAX) {
			const struct v92_survivor_context *other = scratch->context + n;
			uint64_t gap = scratch->metric[n] - scratch->metric[best];
			bool differs = !word_equal(chosen->encoded, other->encoded) ||
				       chosen->gpa != other->gpa || chosen->sign != other->sign;
			for (unsigned i = 0; i < scratch->fill; i++)
				differs |= !word_equal(chosen->pending[i], other->pending[i]) ||
					   (((chosen->nondata ^ other->nondata) >> i) & 1);
			uint64_t margin = UINT64_MAX;
			for (unsigned i = 0; i <= scratch->fill + 2u; i++)
				if (other->margin[i] < margin)
					margin = other->margin[i];
			margin = differs ? gap : add_cost(gap, margin);
			if (margin < result.uncertainty_margin)
				result.uncertainty_margin = margin;
		}
	for (unsigned i = 0; i < 12; i++)
		result.frame_cost =
		    add_cost(result.frame_cost, costs[i][result.eta[i] + scratch->count[i]]);
	for (unsigned n = 0; n < scratch->states; n++)
		if (scratch->metric[n] != UINT64_MAX) {
			struct v92_survivor_context *c = scratch->context + n;
			struct v92_survivor_word decoded = {0}, raw_word = {0};
			for (unsigned bit = 0; bit < scratch->data_bits; bit++) {
				unsigned raw = word_bit(c->encoded, bit) ^ c->sign;
				unsigned source = raw ^ ((c->gpa >> 4) & 1) ^ ((c->gpa >> 22) & 1);
				word_set(&raw_word, bit, raw);
				word_set(&decoded, bit, source);
				c->gpa = ((c->gpa << 1) | raw) & 0x7fffff;
			}
			bool code_mismatch = !expected || !word_equal(raw_word, *expected) ||
					     c->margin[scratch->fill] == 0;
			c->sign = word_bit(c->encoded, scratch->data_bits - 1);
			c->encoded = (struct v92_survivor_word){0};
			unsigned slot = scratch->fill;
			if (slot == V92_PCM_RX_DELAY_FRAMES) {
				c->code_mismatches >>= 1;
				c->nondata >>= 1;
				slot--;
			}
			if (code_mismatch)
				c->code_mismatches |= (uint16_t)(1u << slot);
			if (request_state(c) != V92_PCM_FRAME_RM_NONE)
				c->nondata |= (uint16_t)(1u << slot);
			if (scratch->fill == V92_PCM_RX_DELAY_FRAMES) {
				memmove(c->pending, c->pending + 1,
					(V92_PCM_RX_DELAY_FRAMES - 1) * sizeof c->pending[0]);
				memmove(c->margin, c->margin + 1,
					(V92_SURVIVOR_MARGINS - 1) * sizeof c->margin[0]);
				c->margin[V92_SURVIVOR_MARGINS - 1] = UINT64_MAX;
				c->pending[V92_PCM_RX_DELAY_FRAMES - 1] = decoded;
			} else
				c->pending[scratch->fill] = decoded;
		}
	if (scratch->fill < V92_PCM_RX_DELAY_FRAMES)
		scratch->fill++;
	*s = *scratch;
	*out = result;
	return 1;
}

bool v92_survivor_is_valid(const struct v92_survivor *s)
{
	if (!s || s->states != v34_trellis_state_count(s->selection) ||
	    s->branches != v34_trellis_branch_count(s->selection) || !s->states ||
	    s->data_bits < V92_PCM_MAPPING_MIN_BITS || s->data_bits > V92_PCM_MAPPING_MAX_BITS ||
	    s->fill > V92_PCM_RX_DELAY_FRAMES)
		return false;
	bool normalized = false;
	unsigned total = 0;
	for (unsigned i = 0; i < 12; i++) {
		if (s->bits[i] > 8 || s->offset[i] != total || !s->count[i] ||
		    s->count[i] > V92_CPD_MAX_CONSTELLATION_POINTS)
			return false;
		total += s->bits[i];
	}
	if (total != s->data_bits)
		return false;
	for (unsigned n = 0; n < s->states; n++)
		if (s->metric[n] != UINT64_MAX) {
			const struct v92_survivor_context *c = s->context + n;
			normalized |= s->metric[n] == 0;
			if (c->gpa > 0x7fffff || c->sign > 1 || c->encoded.lo || c->encoded.hi ||
			    c->rm_frames > V92_PCM_RM_QUALIFY_FRAMES || c->rm_prime_frames > 2 ||
			    (c->rm_prime_frames && c->rm_frames != V92_PCM_RM_QUALIFY_FRAMES) ||
			    ((c->nondata | c->code_mismatches) >> s->fill))
				return false;
			for (unsigned i = 0; i < V92_PCM_RX_DELAY_FRAMES; i++) {
				const struct v92_survivor_word w = c->pending[i];
				if (i >= s->fill && (w.lo || w.hi))
					return false;
				if (s->data_bits < 64 ? (w.hi || (w.lo >> s->data_bits))
						      : (w.hi >> (s->data_bits - 64)))
					return false;
			}
		}
	return normalized;
}
