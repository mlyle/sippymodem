# Error geometry of the 16-state PCM upstream map

Linear spacing alone does not describe a coded constellation. Widening one
family of neighboring pairs can narrow intervening boundaries and make other
valid codeword differences harder to distinguish. A useful design objective
must include those alternatives and retain separate power and peak limits.

The following model applies to the direct 36 kbit/s upstream template: 54
bits per 12 symbols, 16 positive points, a 16-state trellis, an identity
precoder and a memoryless prefilter. The Q15 identity coefficient still
rounds each point before transmit gain. It does not apply unchanged to
stateful precoders, intersymbol interference, or the 32/64-state trellises.

## From signed indices to a small graph

The mapping's radices are:

```
32 32 32 16 | 32 32 32 16 | 16 16 16 16
```

Their product is exactly 2^54, so there are no unused radix tuples. Ordinary
modulus-32 positions use all 32 signed indices. The last interval's three
ordinary modulus-16 positions select the inner 16 by minimum magnitude.
Each fourth position selects one parity coset of the full signed alphabet.

For signed indices eta0 through eta3, that coset requires their parity sum
to equal the incoming trellis output Y0. In the 16-state coder, the next
state depends only on these four parities. Upper index bits affect the
physical level but not this state transition. There are eight legal parity
words per state. Complementing all four parities retains the same next state.

A pair graph therefore needs the two trellis states and the three four-symbol
phases: 16 * 16 * 3 nodes. It must distinguish identical indices from different
indices of the same parity. Otherwise it omits uncoded higher-index errors.
The ordinary frame regression checks the parity reduction and the minimum
nonzero diverging/remerging parity distance of four. The distance check runs
to a graph fixed point rather than truncating events after a few symbols.

## An observation metric, not a BER estimate

Let P(c|i) be the probability of observed codec octet c for transmitted signed
index i, including the actual gain, point rounding and modeled impairment.
Define the pair overlap

```
B(i,j) = sum_c sqrt(P(c|i) * P(c|j)).
```

This coefficient lies between zero and one. Independent symbol observations
multiply it, so graph edges can add -log(B). A missing overlap is infinite
cost. Known robbed-bit forcing is included by merging the appropriate codec
bins before computing B. Unknown forcing phase or correlated noise needs a
different observation model.

Start with the first different index pair from any common trellis state;
otherwise an identical path would win at zero cost. Later identical indices
have zero cost. Nonnegative weights permit a shortest path without cycles
after that first edge, so the finite graph bounds events of arbitrary finite
length without a pulse-count or decoder-error sample restriction.

A trellis merge alone is insufficient source evidence. The frame mapper and
GPA inverse can construct source bits for a proposed physical index sequence.
Normally initialized native transmitters must emit it and remerge the complete
scrambler, differential and filter state after a common physical suffix.
This also checks radix capacity and point selection rather than assuming
every mathematical path is a valid transmitted source pair.

## A closed form when every position has the same channel

Let S be the largest overlap of distinct indices with the same parity, and
C the largest overlap of opposite-parity indices, over the full signed set.
For this template the worst finite-event overlap is

```
max(S, C^4).
```

An event containing a same-parity index difference has product at most S.
Every other nontrivial remerging event changes at least four parities, so
its product is at most C^4. Both bounds are attainable in the first full
four-symbol interval: one higher-index change preserves parity, while four
opposite-parity changes can complement the whole parity word and remerge.
The full-capacity frame mapping permits native source construction and a
common suffix for either event.

This covers all opposite-parity boundaries, including those between the
adjacent pairs selected by an eta-XOR-1 objective. It applies when every
position has the same observation channel, including a separate all-samples
robbed-bit stress model. Ordinary one-in-six robbing makes the channel
phase dependent; it requires the weighted graph and each position's actual
alphabet. Robbing every sample is not a model of ordinary one-in-six signaling.

These are worst-pair design criteria, not bit-error predictions. They omit
event multiplicity, receiver estimation errors, finite traceback decisions,
echo and timing motion. A proposed layout still needs independent power and
clipping checks, native decoder qualification on unseen streams and relevant
impairments, and interoperability evidence. Upstream channel estimation is
described in [the channel design notes](pcm-upstream-channel.md).

## Preserve competing-path counts as well as minimum cost

For this 16-state kernel, label a pair transition by the XOR of its four
true and alternative parity bits. Begin with diagonal versus off-diagonal
state pairs, then repeatedly split classes whose labelled successor-class
sets differ. The partition sizes are 2, 6, 18, 20, 20. Each member of a
final class has the same successor-class set for every label, so the
minimum-cost graph can use 20 classes and three phases: 60 nodes.

This is a labelled graph equivalence, not an assumption that XOR of numeric
encoder states determines their next XOR. That simpler reduction fails.
The native frame regression derives the partition from the actual trellis
primitives without a stored transition table. It separately checks that
every member also has identical **counts** of transitions to each labelled
successor class. Set equivalence alone would not justify summing paths.

Two classes contain the diagonal pairs, with eight true states each. The
other classes have eight or 32 members. They are not equally probable.
Uniform independent source bits give eight equiprobable legal parity words
per true state; the native transition counts also preserve a uniform
distribution over the 16 true states.

A single worst-pair overlap can improve while more likely, competing error
events become more numerous. To expose that tradeoff, consider an overlap
sum over events from their first divergence to their first remergence.
For each position's actual signed alphabet, let n be its number of indices
of either parity, and define

```
L(p,q) = (1/n) * sum_{i of parity p} sum_{j of parity q} B(i,j)
U = max(L(0,0), L(1,1))
C = L(0,1) = L(1,0).
```

The equality for C follows from symmetric B and equal parity-set sizes.
U includes identical-index weight one. Replacing either same-parity
orientation by U gives a conservative bound without assuming the two
codec channels have identical same-parity sums. A four-symbol label gets
the product of C at its changed parities and U at its unchanged parities.
Multiply each labelled transition count by this product and divide by
eight for the true word's probability. Keep the three physical phases.

Partition that weighted matrix into diagonal and off-diagonal blocks.
Let A be its 54-node off-diagonal block, b the initial weighted entries
into it, and c the weights returning to a diagonal class. Average initial
mass over the 16 common true states and three phases, giving each of the
six diagonal phase/classes mass 1/6. Let a be the immediate diagonal-return
weight, subtracting the all-identical physical path, whose weight is one.
When the spectral radius of A is less than one, the event sum is bounded by

```
a + b * (I - A)^-1 * c.
```

Do not truncate the series and present the result as a complete sum, or
report a finite bound when the convergence condition is unproved. This
criterion includes event multiplicity, unlike the worst-pair criterion.
It is still not a BER estimate: its independent memoryless channel and
uniform-source assumptions do not model finite decoder traceback, beam
pruning, mismatched metrics, echo, or timing changes. Native decoding and
actual call qualification remain necessary even when both criteria improve.

## Compare training and payload power independently

The same uniform-source assumption gives an exact payload occupation for
this template. Its nine full-alphabet positions include the three parity
positions; the three remaining positions use the inner signed alphabet.
Each inner signed index therefore has probability 15/384 per symbol, and
each outer signed index has probability 9/384. Equivalently, combined
positive/negative magnitudes have weights 5/64 and 3/64, respectively.
The fourth-symbol parity is balanced by the three independent ordinary
parities for any incoming state; this marginal does not require stationary
state probabilities.

Compute physical power after the actual Q15 prefilter and transmit scaling,
including signed rounding. Check the 576-symbol native B1 occupation
separately: B1 and payload do not have the same distribution. A proposed
layout must satisfy each power bound and the peak bound independently,
retain the native nearest gain normalization, and be checked across the
gain uncertainty. A few gain samples can miss narrow codec-cell regressions.
