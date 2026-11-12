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
