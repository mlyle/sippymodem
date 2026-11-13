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

## Larger trellises need a complete event comparison

A larger trellis can separate isolated index errors without improving the
closest complete error event. For the direct 36 and 40 kbit/s geometries,
the 16-, 32-, and 64-state coders can be compared using the full pair graph,
with the actual subset labels and native state transitions. These two maps
have exactly full radix capacity. The trellis transition for a four-symbol
interval depends on each index modulo four; its legal parity sum still
equals incoming Y0.
Three four-symbol phases cover both the frame layout and six-symbol robbing
period. The graphs have 768, 3,072, and 12,288 nodes, respectively.

One exact diagnostic metric is the sum of squared differences between
reconstructed G.711 samples, after native prefilter rounding, transmit gain,
encoding, and any known bit forcing. This is Euclidean separation in the
post-codec observation space. It is not a likelihood for noise before the
codec, a BER estimate, or a substitute for the overlap metric above.

At upstream level 4,096, the following are minimum nontrivial finite-event
squared separations. Each entry lists **16 / 32 / 64 states**. The robbed-bit
column takes the worst of forcing bit zero or one at each of the six phases;
only one sample in six is forced.

| Mapping | Transparent | Worst one-in-six robbing |
| --- | --- | --- |
| 36k, mu-law | 589,824 / 589,824 / 737,280 | 507,904 / 491,520 / 491,520 |
| 36k, A-law | 262,144 / 262,144 / 327,680 | 131,072 / 131,072 / 131,072 |
| 40k, mu-law | 147,456 / 147,456 / 184,320 | 99,328 / 99,328 / 99,328 |
| 40k, A-law | 123,904 / 123,904 / 154,880 | 87,552 / 87,552 / 87,552 |

The comparison holds the geometry fixed. Both retaining the selected gain
and independently recomputing each coder's nearest native B1 gain give these
results: the representable F remains 61 at 36k and 32 at 40k. Native B1
energies differ, but the rounded gains do not. Uniform-source payload power
and peak are therefore equal across the three coders at each geometry.
This result does not establish behavior at other upstream levels or gains.

For a lower-bound certificate, assign each pair node a nonnegative potential
no larger than the claimed distance. Require every diagonal target to have
that distance, every first-different edge from a reachable diagonal state to
bound its destination potential, and every later edge to satisfy
h(destination) <= h(source) + edge cost.
The inequalities bound paths of arbitrary finite length. An attaining source
pair supplies the upper bound. All 312 combinations of law, rate, coder,
gain policy, and wire mode were checked this way, including 348,880,896 edge
inequalities. Separate C and Python constructions agreed on every edge cost.
Every attaining pair was emitted by normally initialized native transmitters
after B1; complete encoder states merged after a common physical suffix,
and eight common source frames then produced identical output. An independent
integer encoder reproduced the source witnesses and codec observations.

Four adjacent opposite-parity changes already limit the transparent 16- and
32-state cases. The 64-state coder raises this minimum by 25 percent in
squared distance, about 0.97 dB, but does not improve the worst robbed-bit
case here. Robbing can make some different indices produce identical wire
octets, allowing inexpensive intermediate trellis steps. Code families are
not nested: at 36k mu-law, the 32-state worst robbed-bit distance is slightly
lower than the 16-state distance.

Consequently, stronger isolated-symbol separation does not justify a blanket
increase in trellis size. Compare complete phase-dependent events, event
counts, actual channel likelihoods, and receiver cost before changing a
selection. These measurements leave rate and coder selection unchanged.

## Check persistence after an incorrect trellis decision

Positive minimum finite-event distance does not bound how long two different
trellis states can produce identical observations after an earlier divergence.
Inspect the zero-cost edges between off-diagonal nodes of the same pair graph.
A directed cycle permits arbitrarily long identical observations while the
states remain different. An acyclic graph admits a finite bound: assign each
node an integer rank that strictly increases along every such edge. A path
cannot contain more edges than the largest rank minus the smallest rank.
Include the sample/robbing phase in the node; merging phase-dependent edges
can create cycles that the physical sequence cannot follow.

For the 312 direct-template cases above, 128 have a zero-cost off-diagonal
cycle and 184 have acyclic certificates. All transparent cases are acyclic;
their longest paths contain 3, 4, and 5 four-symbol intervals for the 16-,
32-, and 64-state coders. These lengths count transitions that leave both
endpoints off the diagonal. Some robbed-bit cases have finite paths as long
as nine intervals. The following counts include the two gain policies;
each entry has 24 robbed-bit cases, excluding transparent transmission.

| Mapping | Cyclic cases: 16 / 32 / 64 states |
| --- | --- |
| 36k, mu-law | 0 / 0 / 0 |
| 40k, mu-law | 12 / 0 / 12 |
| 36k, A-law | 20 / 24 / 24 |
| 40k, A-law | 12 / 12 / 12 |

Every case has an attaining source-level witness from normally initialized
native transmitters. Cyclic witnesses repeat their zero-cost loop 40 times,
producing 480 or more identical wire octets, followed by complete encoder
state remerge and eight common source frames. Independent integer encoding
agrees with every emitted symbol. Rank inequalities certify all zero-cost
edges in the acyclic cases; explicit closed walks certify the cyclic cases.

Every first-different edge from a common state still has positive cost.
Entering the ambiguous region requires a prior divergence. A perfectly
observed clean-start stream therefore retains the earlier separation result.
The constructed payloads establish possible persistence, without estimating
its frequency in ordinary traffic or a field BER. In particular, these
results justify neither shortening the current decoder delay nor choosing
a coder solely from its minimum distance or state count.

## Qualify the geometry actually negotiated

The direct-template graph assumes an identity precoder, a memoryless
prefilter, and exactly full radix capacity. Recent successful 36k Conexant
calls in both laws instead selected 16-state coding, moduli
`[28,28,28,14]` repeated three times, a 90-tap precoder feedback section,
and an 85-tap prefilter. Passive capture preserved 620,560 actual startup
samples exactly in transmitted PCM and RTP octets. Three filter selections
were captured, including a repeated startup before one call connected.

Here the radix product is 29,027,283,136,151,552 for 54 source bits, about
1.61134 times the required capacity. Approximately 37.94% of the radix words
are unused for a fixed incoming differential state. Equal independent digit
probabilities would assign weight to impossible source words. Precoder
history also changes which equivalent signed representative is selected,
and the prefilter spreads its effect across later samples. Applying the
memoryless graph directly would discard both constraints.

One bounded design experiment keeps those learned filters and the full
32-positive-point alphabet, but substitutes the direct 54-bit full-capacity
moduli and recomputes the nearest native B1 gain. This makes the mapping
eligible for the existing independent trellis receiver. The measured
transmitter tradeoff across the three selections was:

| Selection | B1 F: current / full capacity | Payload mean power change | Largest observed amplitude change |
| --- | --- | --- | --- |
| mu-law | 62 / 61 | +2.03% | +13.19% |
| A-law, first startup | 62 / 60 | -1.41% | +11.65% |
| A-law, repeated startup | 62 / 61 | +2.01% | +6.68% |

Each arm starts with 48 B1 frames, then encodes 1,024 payload frames for
each of eight fixed or pseudorandom source patterns. Power and amplitude
include native prefilter rounding and F normalization; the common transmit
LU factor cancels in the comparison. An independent fixed-point encoder
reproduced all 617,472 symbols and recorded coding states, including
history-dependent representative selection. These are observed pattern
peaks, rather than a bound over all payloads.

Receiver eligibility establishes an implementation opportunity. Performance,
codec-cell separation, and interoperability still require a complete channel
and receiver experiment, with the matching receive gain updated for the new
F. Recorded peer responses cannot grade a counterfactual CPd selection.
Keep the negotiated geometry unchanged until that qualification demonstrates
both useful receiver savings and sufficient power, peak, and error margin.

## Enforce source capacity inside an independent-cost decoder

A partial-radix map need not enumerate every digit combination to reject
unused source words. Let `P` be the radix product, `X` the ordinary radix
integer before differential reflection, `U = 2^K - 1`, and
`H = floor((P - 1) / 2)`. A data word is legal when `X <= U`. Its outgoing
differential state is the incoming state XOR `X > H`. These are separate
constraints: preserving the source-range comparison alone is insufficient
for a general map.

Process digits from least to most significant. An unequal higher digit
replaces the comparison accumulated so far; an equal digit preserves it.
Carry both comparisons alongside the convolutional state and incoming
differential bit. Comparing one prefix integer against two fixed thresholds
has at most five reachable comparison pairs, rather than nine independent
pairs. Four-symbol branch costs can be factored through the existing
2D subset labels. Keeping two distinct ordinary radix words per equivalent
state also provides a within-frame runner-up; physical representatives of
the same word must not occupy both slots.

A private native prototype reproduced both ranked words, their costs and
physical witnesses against exhaustive enumeration of 1,296,080 legal walks.
The cases cover 16-, 32-, and 64-state coding, unused capacity, odd products,
ties, erasures, and saturated costs. A deliberate ablation which forgets the
half-capacity comparison loses a cheaper legal winner: one eight-symbol map
returns cost 71 instead of 68. Source-range checking alone must therefore
not become the generic receiver contract.

There is a useful guarded specialization. If the final, most significant
radix is even, `X > H` depends only on whether that digit lies in its upper
half. Earlier prefixes then need only the three source-range comparisons.
The captured `[28,28,28,14]` maps satisfy this condition. The specialization
rejects odd final radices and matches another 946,226 exhaustively enumerated
walks. Both versions also pass native mixed-radix witness checks above 64
bits. These checks establish the independent-cost block calculation, not
end-to-end decoding or the validity of shortening traceback.

The cost remains material. On a Ryzen 7 5825U, a single pinned worker was
measured with invariant-TSC counters over 300 consecutive 10 ms sample
batches per case. Three captured filter selections, three trellis sizes,
and clean, bounded-offset, and zero-observation inputs were exercised.
Only the 16-state selection was observed in those calls; larger coders are
counterfactual stress cases. Across those inputs:

| Private block decoder | Mean batch cost, ms | Worst batch, ms | Worst three consecutive batches, ms |
| --- | --- | --- | --- |
| Generic, 16 states | 1.73–1.85 | 3.30 | 7.51 |
| Even-final specialization, 16 states | 1.60–1.70 | 3.12 | 8.01 |
| Generic, 64 states | 9.97–10.44 | 13.01 | 34.07 |
| Even-final specialization, 64 states | 8.25–8.59 | 11.23 | 30.34 |

The three-batch figures sum processing time and exclude idle intervals.
The prototype allocates about 379 KiB of owned scratch, still sized for all
nine comparison codes. Point-cost construction is included in these times.
The current complete frame receiver took 0.66–0.92 ms on average in the same
harness, with a 1.75 ms worst batch and 4.24 ms worst three. Its bounded
search and additional decoding work differ from the prototype; these are
separate measurements, not an interchangeable implementation comparison.

In particular, the block model omits stateful precoder likelihood, GPA and
payload history, RM/control handling, and B1 acquisition policy. Retaining
scalar end-state metrics does not restore those histories. Synthetic clean
samples from the native transmitter verify coding witnesses, rather than
field BER or resilience to echo. Keep this prototype outside the production
receiver. A next design must reduce branch-combination and witness-copying
cost, preserve the required histories, and beat the existing receiver on
complete channel tests before changing negotiated maps or decoder policy.

## Bound finite precoder representatives before changing the map

Observed pattern peaks do not bound every payload. For the three captured
filter selections above, `z1` and `p2` are absent and the signed alphabet is
`u(eta) = 512*eta + 256`, initially with `-32 <= eta < 32`. The precoder is
`x = round(u + sum(p1[i]*x_history[i])/16384)`, with half-way cases rounded
away from zero. It selects the equivalent representative with smallest
rounded `abs(x)`. Treating `u` as an arbitrary bounded input loses this
selection rule: all three filters have `sum(abs(p1)) > 16384`, so a simple
absolute-feedback contraction cannot establish the useful bound.

Instead, assume every history sample lies in `[-X,X]`. The exact Q15 history
accumulator lies in `[-2*B*X,2*B*X]`, where `B = sum(abs(p1))`. For each digit
and fourth-symbol coset, enumerate its finite equivalent representatives.
The greatest nearest-representative distance over that interval occurs at
an endpoint or a midpoint between adjacent representatives. Check these
points with integer rounding. If every resulting magnitude is at most `X`,
induction from zero filter history proves the bound for every later source
word, including B1 and control words under that fixed selection. Enlarging
the tested set to every residue class safely includes all trellis choices.

The captured partial-capacity maps pass with `X = 7168`. Their finite
alphabets reach at least 2304 units in each direction for every residue
class. The full-capacity proposal increases the covering radius to 8192,
but leaves only 256 units of minimum outer reach. That proof passes for two
filters and fails for the third. Injected history values within the trial
cube produce native outputs of `+8258` and `-8258`, matching an independent
integer calculation. Those histories have not been shown source-reachable;
this rejects the proposed invariant, rather than demonstrating a bad call.
An ablation omitting interval endpoints incorrectly accepts that case.

One additional positive level, 16640, and its negative counterpart raise the
minimum outer reach to 768 without changing the full-capacity moduli, source
rate or covering radius. This guarded proposal proves `abs(x) <= 8192` for
all three filters. The native independent receiver accepts its 33-positive-
point alphabet. Native CPd encoding and decoding round-trip all three maps;
the additional 17 payload bits fit existing fill at the tested 30-bit frame
alignment. B1 energy and the selected F values match the unguarded
full-capacity proposal.

With `p2` absent, the FIR output also obeys
`abs(v) <= round(X*sum(abs(z2))/32768)`. Applying the native F and LU scaling
gives unclipped local sample bounds no greater than 7149 for the captured
maps and 8028 for the guarded proposal. These exclude local signed-16-bit
sample clipping from zero history with those fixed coefficients and gains.
They do not bound channel overshoot, remote codec saturation, or echo.

Qualification includes 552 native injected-history controls and 1666 small
alphabet cases exhaustively checking 626,052 point costs against the sparse
interval calculation. Stream checks cover 926,208 native symbols across nine
mapping/filter arms; every proved bound holds throughout its applicable arms.
An independent fixed-point encoder reproduces
the 308,736 newly generated guarded symbols, including coding states and
energies. None uses the guard points: those streams remain identical to the
unguarded proposal. The bound therefore supplies a design safeguard, without
an observed power, BER or speed improvement. Keep the proposal out of
production until complete channel, receiver-cost and interoperability tests
justify selecting it; the production negotiation remains unchanged.

## Preserve the symbol schedule in the amplitude invariant

The full-capacity map admits a tighter proof without extra guard points.
Its twelve moduli are `[32,32,32,16,32,32,32,16,16,16,16,16]`.
The convolutional coset doubles the representative spacing on each fourth
symbol. Thus a period of candidate bounds is
`X = [8192,8192,8192,8192,8192,8192,8192,8192,4096,4096,4096,8192]`.
Treating all history positions as 8192 discards these three smaller bounds.

At phase `j`, bound the exact history accumulator by
`H[j] = 2*sum(abs(p1[k])*X[(j-1-k) mod 12])`. Apply the same finite-set
endpoint and adjacent-midpoint checks to `[-H[j],H[j]]`, using that phase's
representative classes. Every phase maps into its proposed bound for all
three captured filters. Zero history satisfies every phase, so induction
proves the bounds for every source word under the fixed mapping, including
B1 and RM/RM-prime frames. Neither particular payload patterns nor a particular
trellis path establish this result; checking all residue classes includes
all of their possible choices.

For the previously failing filter, the largest phase-specific accumulator
limit is 272,498,688, versus 278,986,752 for the independent cube. Its injected
8258-output corner violates the smaller history bounds and cannot arise from
zero history under this mapping. The extra outer points are also unnecessary
for these filters: their nearest decision midpoint remains at least 132
output units beyond the permitted history interval, leaving a strict margin
even after integer rounding. They are never selected. Guarded and unguarded
source encoders therefore have identical coding histories, B1 energies and
payload waveforms at the same gain; their advertised CPd records still differ.

Qualification checks all nine mapping/filter arms, 57,456 native corner and
random-history cases, and 38,016 distinct native source symbols, including
adaptive digit choices inverted into actual source bits. An independent
encoder reproduces every field and coding state of those trajectories.
An exhaustive small-alphabet check compares 2,137,050 point costs across
750 systems and 2250 phase cases with the sparse extrema calculation.
This resolves the conservative amplitude-bound gap. It does not establish
better BER, lower receiver cost, channel headroom or interoperability, and
does not change production mapping selection.

## Compare complete wire sequences under separate power limits

Native B1 normalization does not equalize payload power between maps.
The full-capacity proposal used about 14% less payload power in a first
matched-channel pilot. Increasing its source level to equalize payload power
also increases B1 power. Treat these as two separate constraints: choose the
largest integer source level whose native B1 power is at most `B` and whose
calibration-stream mean payload power is at most `D`. Check the next integer
level as well; native sample rounding prevents relying solely on square-root
rescaling. A finite calibration mean is not an all-payload power bound.

A controlled comparison uses the native channel designer for
`H(z) = 1 + 0.25*z^-1 + 0.0625*z^-4`, both companding laws, and the existing
36k partial-radix map versus the full-capacity 54-bit schedule. It retains the
same designed filters and point alphabet within each pair and recalculates
the native B1 gain. Illustrative limits are 17 million squared PCM units per
B1 symbol and 18 million for the mean payload symbol. Eight source streams
choose the levels; three other seeds and both polarities check reception.
These are experiment budgets, not normative transmit-power limits. The
held-out general-channel payload means are 18.03 and 17.75 million,
respectively, so the finite validation powers are close rather than identical.

All 72 clean cases across flat, short, and general channel designs pass.
The native E2u/B1/payload API is exercised with a seeded inverse-channel
training handoff; blind TRN2u acquisition and the outer call owner are outside
this fixture. Applying captured wire filters to an unrelated identity channel
with an assumed receive gain failed the pilot and is excluded from grading.

On the general channel, the full-capacity map naturally selects the independent
decoder. Seven paired runs per law and held-out seed give 1,008 measured
80-sample packets per map on each host. Counter-based elapsed times are:

| Host and map | Mean packet, ms | Largest packet, ms | Largest three-packet sum, ms |
| --- | --- | --- | --- |
| Ryzen 7 5825U, current | 1.041 | 2.881 | 6.192 |
| Ryzen 7 5825U, full capacity | 0.448 | 2.034 | 4.576 |
| Pi 5, current | 1.712 | 4.200 | 11.045 |
| Pi 5, full capacity | 0.771 | 2.855 | 7.007 |

The three-packet sums exclude idle time. These are B1/payload receive costs;
E2u, CPd construction, transmit work, SIP and RTP are outside packet timing.
They do not establish whole-modem capacity or a hard worst-case bound.

Robustness prevents promotion. A 30-profile matrix, including clean controls,
checks DC, noise, all six one-in-six robbed-bit phases with either forced
value, postcursor changes, known echo subtraction, and combined impairments.
The current map passes 269/360 cases and full capacity 246/360, with 25 paired
wins and 48 losses. Sixteen proposal cases pass the native B1 predicate yet
produce 192 wrong payload bits, including 22 wrong frames without ambiguity
or uncertainty warnings. No current-map case has wrong payload bits in this
matrix. These are finite source-bit results, not physical call success rates
or a demonstrated failure of the complete call/LAPM owner. Robbing introduced
after clean B1 also produces proposal errors, so B1 calibration alone does
not explain the weakness.

One mu-law proposal witness changes twelve source bits. Re-encoding both
valid source sequences through the native transmitter and known channel
produces received code streams differing at only two sample positions. Their
final scrambler, trellis, differential, filter, output-queue and channel states
are equal, making this a closed event for that fixed periodic channel.
The same source perturbation changes 625 received codewords in the current
map over the checked interval; its final states still differ. This comparison
does not determine the current map's minimum distance. The proposal streams
remain distinguishable, so this is not an exact wire collision. Selecting the
history decoder also reproduces the erroneous decision in this witness.

Keep the proposal outside production. A useful design objective must price
complete quantized wire events, their persistence and robbed-bit phase,
alongside both power constraints and receiver cost. Radix capacity,
amplitude bounds, clean payload tests and decoder eligibility do not supply
that evidence by themselves. Native source builds on both hosts reproduce
the selected regression; three receive cases pass Valgrind without errors.

## Require identifiable evidence for a codec phase

A known-phase codec-cell model still needs a way to identify that phase.
Sparse training can make a wire operation invisible. At native four-point
TRN2u level 2048 on a flat mu-law channel, the four signed samples are
`-2748,-916,916,2748`, encoding as `39,4f,cf,b9` in hexadecimal. Every codeword
already has its low bit set. Forcing that bit to one at any one-in-six phase
therefore leaves every possible training sequence unchanged. Longer training
cannot distinguish those channel hypotheses. In a native valid-source
example at the same level, that operation changes 69 current-map payload
codewords, or 81 proposal codewords, over 1,152 payload samples. These counts
demonstrate a future observation difference, not payload decoding errors or
a power-matched comparison between maps.

An exhaustive native check of complete, non-clipping alphabets gives:

| Training points | Law | Integer LU choices | Choices with an invisible forced value |
| --- | --- | ---: | ---: |
| 4 | mu-law | 24,422 | 13,555 |
| 4 | A-law | 24,422 | 13,236 |
| 8 | mu-law | 21,449 | 3,336 |
| 8 | A-law | 21,449 | 3,290 |

The check enumerates both signs and every magnitude, rather than one finite
training prefix. These are mathematical counts over native integer levels,
not a distribution of modem operating levels or call probabilities. More
training levels reduce this flat-channel ambiguity, while increasing the
number of signal histories that a finite observation window must cover.

A private inference pilot conditions observed codewords on five consecutive
known TRN2u symbols. With a stationary channel whose memory is at most four
previous symbols, identical histories must have the same pre-operation
codeword, even with unknown fixed gain, offset and polarity. For each of
thirteen hypotheses (unchanged, or either forced value at one of six phases),
intersect the possible original characters across repeated histories.
Select only a unique hypothesis with enough distinct operation witnesses.
Freeze its character sets before checking two later, disjoint windows.
Carry the physical sample phase across window boundaries and media gaps.

Using 4,096 acquisition samples after a 512-sample settling prefix and two
2,048-sample validation windows, the pilot qualifies 889 of 936 noiseless
synthetic cases without a wrong selection. It declines 35 ambiguous cases
and 12 cases with insufficient acquisition witnesses. The declared channel
families are flat, one 25% postcursor, and
`1 + 0.25*z^-1 + 0.0625*z^-4`. Another 368 controls check fixed gain, offset,
polarity, missing observations and reference relabeling. The 32-unit noise,
unmodeled seventh-lag, later gain-change and later phase-change controls all
decline qualification.
This exact-history pilot is deliberately limited to the declared memory and
stationarity assumptions; it supplies no arbitrary-channel false-alarm bound.

For the earlier general-channel payload matrix, 96 distinct training cases
identify the phase/value without reading payload or its expected bits.
Applying those frozen results to 288 native receive cases reproduces the
supplied-phase oracle's functional results: zero wrong payload bits, with
ordinary B1 readiness still rejecting the same cases. Current and proposal
complete passes remain 96/144 and 90/144 respectively. Deliberately shifting
the data phase origin by one sample restores the selected 12-bit mu-law and
6-bit A-law payload errors. Phase ownership belongs to the physical sample
epoch, including control transitions and receiver delays.

Retain an explicit unknown operation when training does not distinguish the
hypotheses. Dense B1 observations can provide additional evidence, but their
precoder representatives and coding state must remain with the native frame
owner. A production detector still needs noise-aware channel fitting, bounded
native work, lifecycle and phase-transfer tests, and physical interoperability.
Keep the full-capacity proposal outside production while those gaps remain.

## Carry coefficient uncertainty while identifying wire operations

Known B1 targets add information that sparse TRN2 can omit. The accepted CPd,
LU, native precoder reset and physical sample epoch must own those targets;
held-out decoded payload is not an input to the learner.

For a proposed six-phase operation vector, use the exact network-character
preimage from `g711_octet_interval_lsb`. A forced bit joins two adjacent
original cells. Preserve the separate mu-law zero characters. With a scalar
channel, a known target `t`, cell `[L,U]` and bounded pre-codec noise `e`, each
observation supplies a strip in the shared gain/offset plane:

```text
L - 0.5 - e <= gain * t + offset <= U + 0.5 + e
```

Closed half-sample endpoints conservatively include rounding ties. Repeated
targets tighten the same strip across different codec phases and across
training/B1. They cannot be assigned independent latent character values.
Intersect the strips for each retained operation vector, preserving an
unknown result when more than one survives.

The full fixed family has 729 vectors. Once every physical phase has an
observation, at most 64 remain before channel fitting: a forced phase can
produce only its forced low bit, so observing both bit values excludes both
forcing modes. Missing a phase leaves the operation unknown. This reduction
uses no at-most-one-robbed-phase assumption.

A private exact-arithmetic study uses native 36k direct mappings, both laws,
four/eight-point TRN2, two levels per law, both polarities and controlled
gain/DC/noise. Acquisition is 288 TRN2 samples plus B1 indices 4..127. The
first four B1 observations are discarded to separate unknown intervening
channel history. Two later 224-sample B1 windows validate the frozen model.
The scalar prior allows either gain sign, magnitude 1/16..16, offset
-32768..32767, and noise peak four. These are study assumptions, not AT
settings or general limits on a telephone channel.

Freezing the centroid of a feasible coefficient polygon loses uncertainty:
a valid held-out character may require a different coefficient still allowed
by acquisition. Retaining the polygon avoids that error. A temporary
intersection checks whether one coefficient can explain all validation
observations; it does not replace the acquisition polygon or change the
already selected operation vector. To confirm a forced phase, test the whole
validation window after removing that operation. Infeasibility supplies
collective evidence even when no individual character rejects every allowed
coefficient by itself.

| Synthetic qualification | Frozen point | Polygon, individual witnesses | Polygon, collective witnesses |
| --- | ---: | ---: | ---: |
| Clean full-family cases | 5716/5832 | 5585/5832 | 5652/5832 |
| Bounded scalar variations | 467/720 | 712/720 | 714/720 |

Every in-class case retains its true profile; no qualified selection is
wrong in the tested matrix. The extra clean declines show the cost of fresh
forcing evidence; the polygon rule is not a universal acceptance improvement.
Unmodeled postcursors, excess noise and later gain changes decline in the
selected controls. Physical-index relabeling preserves the model, while
missing observations retain their original indices. Accepted phase-change
controls have uniform operations, for which that change has no effect.

Independent exact elimination of offset agrees with 5,840 coefficient
projections on the host, including validation intersections. A compact
15-C-module source closure reproduces all 64 native traces on a cold host
build and the Pi. The Pi also reproduces all 7,344 cases for all three oracle
variants exactly. These are arithmetic and native-input checks, not packet
performance measurements.

The four/eight-point comparisons preserve identical native B1 targets and
codewords at each law/level. At mu-law LU2048, eight-point training removes
116 acquisition ambiguities, but 64 fully forced profiles still lack fresh
collective B1 evidence. Its native alphabet peak is 3128 versus 2748 for
four-point training, at nearly the same alphabet RMS. Raising LU also changes
the level-selected CPd mapping, so those results cannot isolate an amplitude
effect or justify a new transmit-power default.

This is an oracle for scalar-channel identifiability. It does not decode
native payload, provide a production selector, measure its packet cost, or
prove a noisy FIR/whole-call false-alarm bound. Production work should extend
the shared codec-interval fitter for richer channels, preserve per-call
coding/phase ownership and uncertainty, and retain ordinary B1 and decoded
payload validation. The full-capacity mapping still has its separately
measured wire-event weaknesses.


## Project scalar coefficient bounds with exact native arithmetic

A scalar gain/DC feasibility test need not clip rational polygons. Retain the
per-target lower/upper strips and eliminate the common DC term. For doubled
cell bounds `l_i` and `u_i`, two observations require:

```text
l_i <= 2 * t_i * gain + 2 * offset <= u_i
2 * (t_j - t_i) * gain <= u_j - l_i
```

Every lower/upper pair therefore supplies one exact rational gain bound.
Include the DC prior as a zero-target strip and intersect the permitted
positive and negative gain intervals separately. A zero left coefficient
with a negative right side is infeasible; closed equality can retain a single
point. Neither zero-area feasible sets nor an unresolved gain sign should
be discarded as numerical failures.

Keep the original strips after projecting. A gain interval alone loses the
coupling to offset and is insufficient for later validation. Temporary joint
constraints must explain both held windows with one acquisition-compatible
coefficient pair. Removing a forced phase is tested separately in each held
window against the unchanged acquisition strips, preserving the collective
counterfactual rule above. Missing samples retain their physical indices.

With signed-16 targets and the study's codec/noise bounds, a projected bound
has numerator magnitude at most 131084 and positive denominator at most
131070. Comparing two bounds needs integer products below 2^35, so signed
64-bit arithmetic suffices without compiler-specific wider integers or
floating-point tolerances. These bounds depend on this scalar model and its
fixed priors; adding FIR taps requires a different elimination strategy.

A private native prototype reproduces all 7,344 collective-oracle cases on
the host and Pi: feasible profile lists, decisions, selected gain projections,
and held-window counterfactuals agree exactly. Its host ASan/UBSan run agrees
too. A compact source package regenerates all 64 native input traces and the
entire observation stream; expected profiles are absent from solver input.

| Whole offline scalar qualification | Median | Largest observed |
| --- | ---: | ---: |
| Host, two builds | 0.136–0.152 ms | 0.821 ms |
| Pi | 0.270 ms | 0.963 ms |

These timings exclude input generation/I/O and native payload decoding.
They are not packet-deadline or multi-call capacity measurements. The tested
cases need at most 81,024 lower/upper-pair checks and 41 distinct targets,
including the DC prior and temporary held constraints. The general bounded
workspace allows many more targets than this matrix exercises. Production
integration still needs incremental work limits, an unknown result while any
candidate is unresolved, authoritative call/phase ownership, and unchanged
ordinary B1/payload verification. The prototype does not establish a noisy
FIR false-alarm bound or physical interoperability.

## Bound projection work with exact envelopes

The pairwise offset elimination above has an equivalent envelope form:

```text
max_i(l_i - 2 * t_i * gain) <= min_j(u_j - 2 * t_j * gain)
```

Sort the target coordinates and build the maximum lower-bound envelope and
the minimum upper-bound envelope. Their active lines change only at exact
rational intersections. Walking those changes across each permitted gain
interval finds the same closed feasible set, including single-point contact.
With distinct targets, deterministic heapsort and two monotone hulls replace
the quadratic pair scan with O(n log n) work. The original strips remain the
authority for offset coupling and subsequent held-window checks.

Projection is only part of the cost. Building envelopes alone slowed the
usual qualification matrix. Both implementations still searched a list for
each observation's target strip. Index the known physical targets once, then
retain a target-to-strip index in each temporary group. This removes repeated
searches across profiles and counterfactual checks. Target identities do not
depend on received cells: preparing their indices must not use held
observations to constrain acquisition or choose a profile.

A second private native prototype combines envelopes with target indexing.
All 7,344 oracle cases retain identical feasible profiles, qualification
decisions, exact gain bounds and held-window counterfactuals on the host and
Pi. Independent rational arithmetic on the host also agrees on 2,022 boundary
and randomized projections. Another 2,816 shuffled groups exercise up to 861
distinct targets on both hosts; host ASan/UBSan checks cover those projections
and the complete qualification matrix. A fresh build from sources and the
fixed input fixture reproduces the decisions and projections.

Six alternating runs per arm give the following complete offline fitting-job
costs. Take the median runtime for each case before summarizing the matrix;
these are neither media-packet costs nor scheduler-independent upper bounds.

| Host / implementation | Median case | 99th percentile | Slowest case median |
| --- | ---: | ---: | ---: |
| Host, pair scan | 0.155 ms | 0.448 ms | 0.558 ms |
| Host, indexed envelopes | 0.136 ms | 0.379 ms | 0.471 ms |
| Pi, pair scan | 0.270 ms | 0.787 ms | 0.957 ms |
| Pi, indexed envelopes | 0.253 ms | 0.625 ms | 0.633 ms |

At the general workspace limit, isolated 861-target projection medians fall
from 2.07 to 0.060 ms on the host and 4.12 to 0.090 ms on the Pi. This larger
synthetic alphabet is separate from the qualification matrix's 41-target
maximum. Envelope scratch costs about 45 KiB, with a further 1.7 KiB target
index per group. Production integration still needs owned resumable scratch
and bounded steps; completing a whole offline job quickly does not establish
a per-packet work limit. An unfinished candidate must remain unknown.

## Resume exact projection within an explicit work budget

Envelope construction can pause without changing the feasible set. Keep the
input strips, sorting indices, two envelopes, active segments and partial
result in one reusable owner. Copy each accepted strip into that owner;
retain no caller buffers. Publish a result only after both gain signs have
finished. Use a new job generation on restart, rejecting stale operations
without touching the replacement job. Bind that job to the physical coding
and sample epoch separately; a job token alone does not prove provenance.

Split execution into constant-work transitions: one heap level, target
validation, envelope append or pop, active-line advance, or segment
intersection. Each step consumes at most its supplied transition budget;
zero budget changes nothing. Exhausting a budget means unfinished work,
never infeasibility. Validate capacity, flags and integer bounds before
accepting input. Require distinct target coordinates after collection;
duplicates are an input error, while contradictory bounds are a valid
empty feasible set. Preserve closed endpoint contacts.

A private native implementation reproduces the same 7,344 qualification
cases, 2,816 shuffled projections and 2,022 independent rational-oracle
cases on the host and Pi. Another 2,048 cases cover the full accepted
integer range, each with five different budgets: answers and total work
agree across all partitions. Lifecycle checks cover cancellation at each
execution stage, restart, stale generations, zero budget and capacity.
Host address/undefined-behavior sanitizer checks preserve the qualification
and shuffled-projection results too.

For `n` targets, a conservative bound is
`2*n*ceil(log2(n)) + 30*n + 32` transitions, including sorting. This bounds
algorithmic work, not execution time. The 861-target bound is 43,082;
the synthetic curved-envelope case uses 16,224. At 64 transitions per step
on Pi, that case averages 0.578 microseconds, with an observed maximum of
2.148 microseconds and a three-step sum maximum of 3.444 microseconds.
The unsliced projection averages 133.969 microseconds. Timing excludes
input loading and idle time; scheduling outliers remain in the samples.
Other budgets have larger outliers, so these are not hard deadline bounds.

The owner occupies 55,296 bytes on both measured hosts, including input
and envelope storage. Reuse it outside the modem hot stack. This prototype
only bounds projection: target indexing, observation collection, candidate
enumeration and held-window validation still need resumable scheduling.
It does not establish a production selector, modem-packet cost or field
reliability improvement. Retain original acquisition strips, unresolved
candidates, ordinary B1 validation and decoded-payload qualification.

## Bound the complete scalar learning job

Projection budgeting is insufficient if observation collection, target indexing
or held-window checks still run to completion in one media callback. Extend the
same owned continuation to every stage: initialize codec cells, count phase
coverage, index targets, enumerate profiles, collect strips, project, copy the
acquisition constraints, and validate both held windows and counterfactuals.
Share one bounded heapsort mechanism between target indexing and projection.
The fixed six-phase checks remain constant work; loops over observations,
targets or candidates advance only within the supplied transition budget.

The complete private prototype owns up to 860 observations and 861 target IDs,
including the DC prior. It copies input rows by value and keeps the original
physical phase labels, including relabeled controls and erasures. Enumerate all
729 six-phase profiles; observed low-bit masks admit at most 64 for fitting.
A partially evaluated or unvisited profile stays unknown. Acquisition
feasibility is not final qualification: both held windows must still pass
against the frozen acquisition constraints. Publish the final result only
when the whole job finishes. Cancellation and replacement use monotonic job
generations; call and coding-epoch identities remain separate caller-supplied
provenance that a production adapter must verify.

On the host and Pi, all 7,344 existing qualification cases match the original
pairwise oracle exactly at budgets of 1, 64 and 100,000 transitions. Both answers
and total work are invariant under those partitions. Another 64 synthetic
cases use the full 861-target workspace, both codec laws, polarity, DC, noise,
forced-bit profiles, erasures and held-phase relabeling. They match the oracle
at budgets of 1 and 128, again with identical work. Host address/undefined-
behavior sanitizers and a fresh source-only build reproduce these results.
Lifecycle checks cover cancellation at each of 15 execution stages, stale
operations, zero work, full input queues, generation exhaustion and two
interleaved owners.

The complete owner occupies 114,184 bytes on both hosts and reuses its
projection scratch. Allocate and initialize it outside the media hot path.
Codec cells are cached per owner and law, with their first initialization
spread over 768 transitions. A conservative accounting for the maximum
workspace and 64 compatible profiles bounds a job below four million
constant-work transitions. This is an algorithmic bound, not a time bound.
The existing matrix needs at most 93,042 transitions per job; the largest
synthetic case needs 833,275, with another 768 on first use of a codec law.

Isolated Pi measurements for these two cases, 32 jobs per case and budget,
include the first cold initialization and retain all scheduling outliers:

| Transition budget | Largest step, existing case | Largest step, synthetic case | Largest three-step sum, synthetic case |
| --- | ---: | ---: | ---: |
| 64 | 11.6 us | 20.7 us | 28.6 us |
| 512 | 35.3 us | 29.3 us | 49.3 us |
| 2,048 | 44.0 us | 55.3 us | 125.3 us |
| 8,192 | 167.3 us | 154.5 us | 458.1 us |

The hardware timer measures each processing invocation; the three-step sum
excludes idle time and never crosses a job boundary. Input loading, allocation
and result retrieval are outside these intervals. The synthetic case needs
13,020 warm invocations at budget 64, but 102 at budget 8,192. If a scheduler
supplied one such budget every 10 ms, those would consume about 130 seconds
and 1.02 seconds of scheduling slots respectively. That is a scheduling
example, not measured modem training latency. The observed maximum depends
on the machine and workload; these figures establish neither hard deadlines
nor whole-modem CPU headroom.

This completes a bounded scalar prototype, not its production integration.
A native adapter still needs authoritative target/sample ownership, coding-
epoch invalidation, an explicit training deadline and work allocation, and a
conservative outcome when the deadline expires with candidates unresolved.
Ordinary B1 and decoded-payload validation remain necessary. No payload BER,
FIR-channel admission guarantee or physical-call reliability gain follows
from matching this scalar oracle.

## Bind learning to the native receive clock

The native Phase 4 channel measurement consumes echo-cleaned, polarity-
normalized samples. Its known targets retain the original linear training
levels, while the scalar learner's intervals describe the original received
G.711 codewords. These are different coordinates. Re-encoding the cleaned
sample does not recover the observed codec cell or its robbed-bit evidence.
Preserve the original octet, availability and physical sample index together;
keep echo correction and its uncertainty explicit in any extended model.

A private observer follows this ownership through complete native software
calls. The CPt handoff supplies the receive counter and buffered early TRN2u
span. Their difference binds the next training input to the physical clock.
The FIR decision delay then locates the original octet corresponding to each
known target. The final ten pending targets still belong to TRN2u when the
receiver starts consuming control. The existing early-sample bound is 216;
a 227-entry sidecar covers that overlap, the ten-sample delay and the current
sample without retaining a complete training waveform. This sidecar belongs
to the call owner and must preserve missing samples' clock positions.

Across 32 native calls, the observer checks 936,000 observations in 78 separate
training epochs, including both codec laws, polarity reversal, echo, marked
erasures, and either forced low bit at each of six phases. Original octets
and availability match the simulated wire at every recorded index. Delayed
cleaned samples match the actual FIR history, and target symbol classes match
the independently recorded transmitter waveform. This last check establishes
symbol alignment, not exact source amplitudes: some epochs transmit multiple
nearby levels for the same nominal target. Eight uninstrumented controls have
identical media and payload streams. A fresh build of the complete native core
reproduces four representative calls, including failed startup cases.

For a qualification check before CPd selection, use only TRN2u: 512 acquisition
observations at indices 512..1023 and two held windows of 128 observations at
4096..4223 and 8192..8319. Keep every epoch separate. The original-byte scalar
model qualifies three epochs, leaves 68 ambiguous, rejects five as infeasible,
and rejects two in held validation. These repeated epochs are not independent
physical reliability trials. Sparse training remains insufficient to authorize
most selections; ambiguity must retain the existing conservative path.

Two deliberately invalid adapters demonstrate why input ownership is part of
qualification. Re-encoding echo-cleaned samples qualifies five epochs instead
of three. Erasing every 37th sample while preserving the physical clock leaves
the outcomes unchanged, but compressing that clock qualifies 35 epochs,
including 32 robbed-bit epochs incorrectly selecting an unchanged-bit profile.
Shifting only the held targets by one symbol qualifies none. All 390 cases
match the original exact pairwise oracle at work budgets of 1 and 8,192;
work counts also match between partitions, and address/undefined-behavior
sanitizers preserve the results. Correct arithmetic cannot repair invalid
sample labels or invented codec observations.

The native call matrix also exposes an unresolved startup limitation. Within
its fixed 55-second observation span, forcing a zero bit every sixth sample
delivers bidirectional payload at all six phases with mu-law and three phases
with A-law. Forcing a one bit delivers none at either law's six phases. These
are deterministic fixture results with the current receiver, not field success
rates or proof that a longer call cannot connect. The learner remains private;
this study changes neither rate selection, training deadlines nor call grades.
Production integration still needs per-call/coding-epoch invalidation, bounded
work scheduling, and ordinary B1 and decoded-payload qualification.

## Construct levels from frozen observation bounds

A finite catalog can fail to admit a rate even when another level geometry
separates under the same channel evidence. Keep catalog failure distinct from
an impossibility result. A complementary constructor can work directly from
the frozen phase, gain, offset and robbed-bit explanations.

For a proposed bound `A` on every emitted sample, propagate each signed level
through every retained explanation and every allowed filter history. Include
integer rounding, codec cells and the declared additive error. Order codec
characters by their input cells, not their unsigned byte values. Conservative
lower and upper ranks then describe the possible observations of `+x` and
`-x`. A shared alphabet must separate both signs and work at its weakest
physical robbed-bit position.

When those endpoints are monotone in `x`, select the earliest next positive
level whose positive interval follows the previous positive interval and
whose negative interval precedes the previous negative interval. The first
level must also separate its own two signs. This gives an ordered candidate
without assuming uniformly spaced linear levels or codec reconstruction
points. It is a constructor within the chosen amplitude bound and common
alphabet family, not a proof of optimal modem capacity.

Power normalization belongs inside this search. Uniformly shrinking the CPd
levels generally makes B1 normalization enlarge them again. For each rate,
obtain valid moduli and check their exact product. Choose the starting level
near the occupation-weighted B1 power target, then recompute the native
prefilter energy, representable gain and emitted samples. Recheck separation
at those actual samples and reject any amplitude-bound violation. A monotone
pre-normalization search does not make quantized normalization monotone.
Reuse B1 magnitude frequencies only where unchanged mapper index choices
have been established; `v92_pcm_frame_measure_b1u()` remains the independent
normalization check.

Keep the existing catalog alongside this constructor: a finite amplitude
sweep or a common alphabet can miss a better layout, especially when only
some physical positions are robbed. Freeze selected levels and their complete
observation support before payload validation. Native B1 and payload decoding,
CPd wire roundtrips and independent source-extremum checks protect different
boundaries. They do not establish the assumed noise bound, future channel
stationarity, physical BER or a production acquisition deadline. The current
production selector is unchanged by this construction study.

## Use identified robbed-bit positions separately

A common alphabet pays the robbed-bit penalty at every position. If frozen
training explanations identify affected physical positions, construct each
phase's signed support separately. In a one-in-six model, five positions can
use finer alphabets while the affected position keeps wider separation. Retain
the union of every surviving phase hypothesis; a preferred hypothesis alone
does not justify removing uncertainty.

Bind this to the actual sample clock and filter delay. A source symbol at
phase `j` reaches the codec at `(j + delay) % 6`; the declared 31-tap study
filter has delay 15. Rotating the classifier without that binding produces
incorrect or unclassified observations even when its level arithmetic is
correct. This is not a universal fixed delay for the production receiver.

The twelve moduli must fit their phase alphabets, including the reduced
trellis intervals, and their exact product must cover the selected data bits.
Measure B1 occupation separately by phase before combining power: summing
point-index frequencies across different alphabets loses their amplitudes.
Recompute one shared gain, then recheck every emitted signed level against
its phase's support and the common source-amplitude bound.

A bounded private construction study uses eight frozen native-training cases
with one-in-six robbing, both codec laws, and the same stationary FIR, gain,
offset and additive-error bounds as the common-alphabet study. All eight
retain one physical robbed-position hypothesis. Separate alphabets improve
the best catalog/common-constructor rate in six cases and preserve it in two;
two reach 72 bits per twelve samples, or 48 kbit/s. This is conditional
capacity under the supplied model, not a physical-call rate result.

The selected maps survive native CPd wire roundtrips at two alignments and
actual TRN2 history carried into B1 and 4,096 payload frames per case: all
2,072,576 payload bits decode correctly. Independent native filter extrema
and gain/offset corners agree with every signed classifier support. Removing
the true channel, earlier selected rates and descriptive case identifiers
reproduces the same maps and supports. Payload mean-square power is
0.956--1.009 times B1 power in these streams, with no source clipping; those
measurements do not establish an arbitrary payload's power or line compliance.

This finite constructor takes about twenty seconds for the eight-case host
study, including native geometry preparation. It needs bounded scheduling
before live use. Production integration also needs qualified physical-phase
ownership, coding-epoch invalidation and conservative ambiguous-model
handling. The production selector, defaults and live rates remain unchanged.

## Keep constellation candidates across phase hypotheses

When the robbed position is uncertain, widen every source phase that any
surviving physical-position hypothesis can affect. Keep the common-support
construction as a fallback. Select against the complete union before payload;
choosing a preferred phase and validating it afterward loses that protection.

Do not require all clean positions to have the same alphabet size. Native B1
occupation, twelve radices and one integer normalization gain interact. A
smaller alphabet at an additional clean position can improve this joint
trade-off. A layout discovered while considering a worse channel may therefore
also improve a better channel. Keep it as a candidate and recheck its complete
signed support there. Within a shared candidate set, adding uncertainty cannot
increase the admitted maximum rate. An apparent increase from separate finite
searches identifies a search restriction, not new channel capacity.

A private study reuses the eight stationary training-fit boxes above and
introduces hypothetical physical-position sets `{3}`, `{3,4}`, `{1,3,5}` and
`{0,1,2,3,4,5}`. The added positions were not observed surviving those original
training fits. Each set widens its affected source positions using the declared
15-sample filter delay. Rechecking winners across these sets improves two
single-position constructions by two bits per twelve-sample frame without
changing their channel assumptions. All 32 final maps retain at least the
previous common-support rate.

In one A-law case, the constructed rates are 48 kbit/s for the known position,
46.667 kbit/s for either tested two- or three-position set, and 38.667 kbit/s
when any position is possible. These are conditional construction results,
not physical-call rates or optima over arbitrary uncertainty sets. The finite
32-case search takes about 29 seconds on the study host, before independent
validation; it is not suitable for synchronous live training work.

The frozen maps pass native CPd roundtrips at alignments 17 and 24, followed by
actual TRN2 history, B1 and 4,096 payload frames at every declared position:
96 runs verify 23,969,792 payload bits without errors. Independent native FIR
extrema, gain/offset corners and literal codec intervals agree with every
signed support. Removing true-channel fields, descriptive identifiers and
catalog choices reproduces the selected maps. Separate structured payload
streams range from 0.917 to 1.081 times B1 mean-square power without clipping;
this does not establish arbitrary-payload power compliance. Production phase
ownership, bounded acquisition and coding-epoch invalidation remain required.
The production selector and live rates are unchanged.

## Bound support preparation and candidate lifetimes

Represent a monotone codec-rank bound by its first source magnitude at each
rank threshold, using peak + 1 when the threshold is never reached. Four
ascending curves cover positive lower/upper bounds and reflected negative
upper/lower bounds. A union's lower-envelope thresholds take the maximum
across models; upper-envelope thresholds take the minimum. This retains the
complete surviving model union without storing a cell at every source level.
Codec LSB forcing swaps adjacent ranks; widen the affected phase's lower
rank to even and upper rank to odd before testing separation.

Compile one frozen fractional-FIR/gain/DC box at a time, using native filter
coefficients, outward projection and the shared saturated sample quantizer.
Reject an unsupported non-monotone model rather than silently dropping it.
Copy its values and retain no source-buffer pointer. Limit each continuation
to 128 bisection or merge iterations; publish support only after every
expected model contributes. Invert the thresholds to find separated levels.

Keep call, coding, model and physical-phase-binding generations with support
and every candidate batch. The owner explicitly begins or cancels preparation.
Reject stale work without modifying newer support, and reject a late result
without cancelling current work. Retain the used geometry weights, counts and
normalization level by value. Recheck generations before CPd promotion. A
returned-echo delay estimate alone does not establish outgoing phase binding.

A private native prototype uses 2,056 bytes for four 257-entry threshold arrays
and 2,208 bytes for its complete support owner, including the current model.
The preceding literal-cell representation used up to 607.2 KiB at the studied
peaks, or 1.125 MiB at its maximum peak. Candidate batches, geometry and the
upstream fitter's storage are additional; keep training ownership outside a
per-sample copied call state.

Eight synthetic fit sets supply 927 model boxes. All 658,992 virtual cells,
minimum levels and separation links match the earlier arrays. All 5,896
geometry steps retain 6,342 admissions, with complete candidate comparisons
against the earlier native constructor and frozen semantic digests. Host and
Pi checks include 105,025 lifecycle/bounds assertions and 1,920 native FIR,
gain/DC, noise and LSB corner checks. All 64 robbed-position masks are tested;
maximum model count and peak are exercised separately. Host ASan/UBSan and
Memcheck also pass.

Threshold lookup trades additional computation for memory. This Pi 5 sample
measures the following individual work units with hardware counters:

| Work unit | Mean | Maximum | Maximum sum of three consecutive units |
| --- | ---: | ---: | ---: |
| Copy and compile one model | 1.59 us | 4.52 us | 8.02 us |
| Up to 128 projection/bisection/merge iterations | 4.01 us | 24.41 us | 37.15 us |
| Construct one geometry's candidate batch | 51.06 us | 421.80 us | 931.13 us |

Measured model preprocessing and support projection total 0.409 seconds across
these eight fit sets; the largest individual fit takes 160.04 ms. All 5,896
candidate batches total 0.301 seconds. These are elapsed work observations,
not complete media packets or hard real-time bounds. They exclude fixed owner
initialization, channel learning, input I/O and native B1 geometry preparation.

The inputs use one previously selected peak per synthetic fit. Production
integration still needs eligible native learner output that preserves every
surviving alternative, qualified physical-phase provenance, actual coding/B1
geometry admission, CPd promotion and whole-call training deadlines. The
production selector and live rates remain unchanged.

## Publish every frozen channel explanation

The scalar learner's single qualified projection is not a replacement for the
fractional-channel model union. A native producer must retain each surviving
fractional phase, gain/DC box and physical robbed-position mask together.
Do not publish a preferred model or discard an alternative because a downstream
consumer cannot represent it.

A private native prototype copies the known source waveform, observed octets,
availability and supplied physical-phase labels into one owner. It computes
one phase's predictions with the native 31-tap FIR and reuses that scratch for
every requested robbed-position hypothesis. Two outward interval-tightening
passes, each limited to twelve sweeps, retain conservative acquisition boxes;
the first uses at most 1,024 acquisition rows and the second uses them all.
Saturated codec-cell preimages extend to infinity. The declared phase grid,
gain and DC priors, nonnegative gain, and additive-error bound remain model
assumptions.

Keep acquisition boxes frozen while checking two fresh held windows against
their union. Held data must not silently refit them. Return models only after
both windows pass; incomplete, cancelled and stale jobs cannot publish them.
Copy model results by value and retain independent call, coding, model and
physical-binding identities. These identities do not themselves prove that
the adapter supplied the correct source waveform or physical clock labels.

The owner occupies 516,816 bytes, including capacity for 20,000 source/observed
rows, 1,025 declared phases, eight physical-mask hypotheses and all 8,200 model
alternatives. Allocate it outside media processing and per-sample call copies.
Each continuation transition handles bounded FIR, observation or bookkeeping
work; it never scans an unbounded observation or candidate list internally.

Host and Pi checks reproduce all 3,979 boxes from the preceding 24 synthetic
native-TRN2 fits and reject four corrupted held-window controls. Host budgets
of 1, 128 and 8,192 transitions produce identical answers and work counts.
Another 96 cases cover saturated observations, physical-phase relabeling and
erasures. Independent exact rational clipping retains every vertex of 389
nonempty gain/DC polygons inside the published boxes. Capacity and lifecycle
checks preserve all 8,200 alternatives. Host ASan/UBSan and focused Memcheck
checks pass without memory errors or retained allocations.

Two representative fits, four repetitions each, give these Pi 5 elapsed
hardware-counter measurements. Three-step sums stay within a job and exclude
the driver's input, output and idle time; no timing outliers are removed.

| Transition budget | Mean step range | Largest step | Largest three-step sum |
| --- | ---: | ---: | ---: |
| 128 | 6.0--7.4 us | 37.9 us | 59.2 us |
| 1,024 | 47.7--58.9 us | 126.7 us | 303.3 us |
| 8,192 | 381.5--470.8 us | 762.5 us | 1,928.3 us |
| 65,536 | 3.08--3.76 ms | 5.08 ms | 14.96 ms |

The larger-work fixture still takes about four seconds of total processing.
The Pi run reaches 82.1 degrees C and records current firmware flag `0x8`.
These component measurements do not prove a live training deadline or
multi-call headroom. Reduce total work and qualify actual acquisition scheduling
before integration. Preserve model-specific robbed masks when constructing
per-phase observation support: replacing their correlation with one global
mask changes the represented set. Native coding/B1 admission, CPd promotion
and physical-epoch ownership remain required. The production selector and
live rates are unchanged.
