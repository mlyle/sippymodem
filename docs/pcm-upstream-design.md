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

## Apply each model's robbed-position mask before combining support

Keep each fractional phase, gain/DC box and physical mask paired while
constructing observation bounds. Applying the OR of all surviving masks to
every box invents combinations that acquisition did not retain. It can
unnecessarily widen support and reduce the set of separable constellation
points. Compute each model's masked bounds first, then take their per-phase
signed interval hull. This hull remains conservative; it does not preserve
holes or prove that one stationary model explains an entire data stream.

The compact inverse-threshold representation can do this without storing a
curve for every model or source magnitude. Retain four monotone curves for
each of six physical positions. For an increasing codec-rank function, let
`T[k]` be the first source magnitude whose rank reaches `k`. Robbing expands
each lower endpoint to an even rank and each upper endpoint to an odd rank:

- Lower endpoint: `T_even[k] = T[k + (k & 1)]`.
- Upper endpoint: `T_odd[k] = T[k - (k & 1)]`.

Retain the rank-zero and rank-256 sentinels. In particular, an upper endpoint
of rank zero expands to rank one. Inverting negative ranks around 255 gives
the same parity rules. Merge lower-envelope thresholds by maximum and
upper-envelope thresholds by minimum, separately for each physical position.
One base threshold needs at most two updates in each of the six positions;
there is no model-list scan inside a query or continuation transition.

A private native adapter accepts every qualified producer model by value,
including its own mask, and supports all 8,200 alternatives. Its six sets of
four curves occupy 12,336 bytes; the complete adapter occupies 12,528 bytes.
The fitter and support owner remain outside per-sample call-state copies.
Advance at most 128 bounded transitions per call. Fence publication with the
fitter job generation as well as call, coding, model and physical-binding
identities. Recheck source readiness when constructing and consuming a
candidate batch: cancelling a fit must invalidate an already prepared batch.
These checks require the caller's serialized, qualified fitter owner; identity
fields do not establish physical source or phase binding by themselves.

Independent forward enumeration checks 9,437,184 complete support cells across
both laws, all 64 masks and all six phase rotations. Another 4,128,768 checks
cover actual native FIR rounding and saturation, gain/DC endpoints and robbed
octets. The integrated host and Pi runs preserve all 3,979 fitted models,
withhold four corrupted-held cases, and verify 138 research candidates against
the independent signed bounds. Host budgets of one and 128 transitions give
identical threshold curves and work counts. Capacity, aliasing, cancellation and stale
result checks pass; host ASan/UBSan and focused Memcheck checks also pass.

Two verified runs per host give these elapsed reference-counter measurements
for the support adapter at a budget of 128 transitions. All raw samples are
retained; three-step sums never cross a job boundary.

| Host | Mean step | Largest step | Largest three-step sum |
| --- | ---: | ---: | ---: |
| Ryzen | 2.27--2.39 us | 631.82 us | 635.41 us |
| Pi 5 | 3.96 us | 31.54 us | 40.39 us |

The Ryzen scheduling outlier is included. The Pi reaches 77.1 degrees C with
no active firmware flags at sampled instants. These are support-component
measurements, excluding fitting, geometry construction and driver I/O; they
do not establish whole-packet or live training deadlines. Physical binding,
native coding/B1 admission and CPd promotion remain unqualified. The prototype
does not change production selection or live rates.

## Bind training rows without conflating the three clocks

The native TRN2u receiver's reference runs ten symbols ahead of its delayed
training decisions. The channel model's causal 31-tap FIR independently has a
fifteen-sample centre. Keep both offsets separate from the physical receive
clock used to label robbed-bit positions. A delayed decision target is not the
source waveform for the incoming sample.

A private native adapter copies the current training generator by value,
including its coding history and differential sign. If its next physical
source symbol is `S[n]`, a row at physical clock `T+r` carries `S[n+15+r]`.
Thirty unobserved prefix rows fill the model FIR. The fifteen-symbol source
lead cancels that FIR's centre; the observation's phase remains `(T+r)%6`.
Consequently this bound support consumer applies no additional mask-phase
shift. The preceding causal-source fixtures still use a fifteen-sample shift;
changing the generic consumer globally would confuse the two representations.

Check each accepted native input against the copied coding state and physical
and receiver counters. Erasures advance both clocks without supplying fit
coverage. Require fifteen actual trailing training inputs to witness the final
source lookahead; at closure the source and native generator states agree.
A short Ru/TRN2u exchange cannot manufacture these observations. Preserve the
preceding receive plan when the actual exchange cannot qualify a new model.
Fence publication and late support use with the live call, coding, model and
physical-binding stamps and fitter generation. Current discontinuities cancel
the job; stale requests leave the current owner intact. The capture owner uses
128 bytes and retains no receiver or fitter pointers.

Host and Pi checks cover 64 positive native training cases: both codec laws,
four/eight-point alphabets, both initial signs, all six physical phases,
concealed inputs, and collection restarted after already accepted training.
The declared channel survives every positive fit. Both corrupted held groups
are withheld under both laws. Four-point native Ru handoffs under both laws
withhold a fresh fit at the short boundary and retain coding when training
continues sufficiently. These checks are not full SIP or E2u exchanges.
The matrix verifies 62,968 rows, 57,200 observed support cells, 16 lifecycle
cases and 192 source/support alias boundaries. Using delayed decision targets
mislabels 51,524 rows; adding the FIR centre again changes 813,688 signed
support cells. These are alignment diagnostics, not BER measurements.
The complete native source closure passes ASan/UBSan and Memcheck, and the Pi
reproduces the host results with verified source hashes.

Actual call-owner and RTP/source integration, qualified host-conditioning and
echo-error bounds, physical-capture validation, native coding/B1 admission,
CPd promotion and live scheduling remain required. The prototype supplies a
native clock witness; identity fields alone cannot establish those remaining
facts. Production selection and live rates remain unchanged.

## Condition raw observations with bounded additive corrections

Do not encode an echo-cleaned sample and treat that new octet as captured
wire evidence. Subtraction and G.711 quantization do not commute. Keep the
original octet, physical phase and availability, together with a declared
interval for the additive contribution in the raw codec's linear domain.
This interval is an assumption until a separate echo/conditioning owner has
qualified and bound it to the actual observation epoch.

Let `C(code)` be the raw octet's continuous input cell, including its robbed-bit
alternative when applicable and the declared noise allowance. For an additive
contribution in `[e_low,e_high]`, constrain the unconditioned forward prediction
`gain * filtered_source + dc` to:

```
[C(code).lower - e_high, C(code).upper - e_low]
```

Keep outward numerical bounds and the infinite ends of saturated cells.
Zero correction uses the original cell unchanged. A research extension stores
the interval by value in each training row; invalid or reversed bounds reject
without advancing either owner. Concealed inputs still advance the witnessed
source and physical clocks without contributing observations. The row is
16 bytes and the fitter owner is 716,816 bytes, outside per-sample DSP copies;
the native capture owner remains 128 bytes.

A conditioned training fit does not establish an unconditioned data alphabet.
Mark the fitted observation domain explicitly. The research raw-rank support
adapter declines conditioned fits, preserving its previous output. Before
using these fits for constellation selection, account for the data decoder's
raw-cell correction and uncertainty, as well as the conditioning epoch.
Reusing the smaller post-subtraction codec cell would repeat the original
mistake at that boundary.

Host and Pi checks cover 15,728,640 raw-cell cases over both laws, signed
inputs, forced robbed-bit values, noise, offset intervals and saturation.
A smaller subtraction experiment finds 15,477 inputs whose true source falls
outside the cell fabricated by re-encoding the cleaned sample; these are
synthetic counterexamples, not field error rates. Forty-eight native training
cases retain the declared model across both laws, four/eight-point sources,
both initial signs, three physical rotations and two correction widths.
Both corrupted held groups are withheld under both laws, and omitting the
required bounds rejects both law controls. The preceding 64 unconditioned
native cases still pass. Invalid-bound rollback and stale conditioning-epoch
checks protect the new boundary; ASan/UBSan and focused Memcheck pass.

These checks supply the additive intervals. They do not estimate or prove an
actual modem's echo-error bound, certify arbitrary host preprocessing, or
qualify physical captures and CPd promotion. Production rates and waveforms
are unchanged.


## Keep the data observation's conditioning estimate

A safe global echo interval is not a substitute for retaining the estimate
available to the decoder. For the scalar observation `code(s + e + n)`, let
`s` be a candidate source, `e` the known estimate, and `n` an independently
bounded residual in `[-R,R]`. If the original raw code has integer preimage
`[L,U]`, candidate `s` permits estimates in `[L-s-R,U-s+R]`.

Two candidates are confusable at the same known estimate only when their
estimate intervals intersect together. Discarding that estimate instead
allows each alternative to use a different estimate from the global range.
The resulting union can erase much of the usable alphabet even when the
residual bound is small. Continue using the original raw code cell, including
its model-specific robbed-bit alternative; do not re-encode a cleaned sample.

A separate integer scalar study chooses fixed symmetric alphabets across
all estimates in `[-1024,1024]`, with peak source amplitude 16000 and residual
bound 8. The counts below are positive levels; each has a negative partner.

| Codec and physical position | Global estimate union | Decode with estimate |
| --- | ---: | ---: |
| PCMU, intact | 7 | 58 |
| PCMU, robbed | 6 | 31 |
| PCMA, intact | 7 | 59 |
| PCMA, robbed | 6 | 32 |

Intact and robbed positions keep separate alphabets. These counts assume
unity gain, no FIR or DC, integer sources and corrections, and no clipping
inside the selected signal bounds. They are not V.92 constellations or line
bitrates. Monotone raw preimages permit a greedy minimum-successor construction;
independent dynamic programming agrees on 240 small-domain cases.

Host, Pi and sanitizer results agree across 96 larger cases. Checks cover
393,216 exact preimages, 6,144 directly enumerated pair comparisons, and
146,774,602 native encoder observations of the selected alphabets. Negative
witnesses demonstrate collisions both when the estimate is discarded and
when the true residual reaches 9 after declaring a bound of 8.

Preserve conditioning identity and its justified uncertainty through actual
candidate decoding. This study does not qualify a physical echo-error bound
or continuous-valued preprocessing. Keep declining conditioned fits at the
existing raw-rank publication boundary until that decoder-domain contract,
its real channel model, and its call/coding epochs are qualified together.

## Bound gain uncertainty in the conditioned data alphabet

Keeping the decoder's additive estimate does not make the fitted channel gain
exact. Alternatives may use different gains inside the retained uncertainty
bound unless the decoder jointly constrains that gain across its whole
observation sequence. Keep the observed estimate shared; retain the permitted
variation of quantities that remain unobserved.

A scalar extension uses `G711(floor(g*s + e + n))`, with known integer `e`,
real residual `n` bounded by `R`, and positive real gain in a declared interval.
The floor is explicit synthetic preprocessing, not an assertion about every
host or physical codec. For raw integer preimage `[L,U]` and candidate forward
range `[l_s,u_s]` before adding `e`, the feasible estimate interval is:

```
L - u_s <= e < U + 1 - l_s
```

Preserve its open upper endpoint. Competing candidates collide when their
feasible intervals share an allowed integer estimate. Native input enumeration
agrees with this exact rational construction, and all 32 unity-gain controls
retain the earlier study's counts.

At peak 16000, estimate extent 1024 and residual bound 8, PCMU's intact
positive-level count falls from 58 with known unity gain to 52 for uncertainty
`+/-1/256`, then 40 for `+/-1/64`. The corresponding robbed-position counts
are 31, 29 and 25. These are fixed symmetric scalar alphabets, not line rates
or V.92 parameter selections.

Host and Pi agree on 160 larger cases, 180 small-domain optimality checks,
902,936 selected-pair separation checks and 4,898,780 native observations.
Eight exact rational counterexamples show collisions after omitting or
exceeding the gain bound; the sanitizer outputs agree. This supplies bounds
for the experiment rather than qualifying an actual modem's estimation error.

Before publishing a conditioned data alphabet, bind the correct host
quantizer, gain and correction uncertainty to the native decoder and its
coding epoch. Joint sequence inference can use stronger information than
this per-symbol union, but needs its own qualified contract. Keep the research
support adapter's rejection of conditioned fits until this boundary is qualified.
Production selection remains unchanged.

## Share unknown gain across a complete observation sequence

A union over the allowed gain for each symbol discards information when one
unknown gain actually applies throughout a sequence. Competing candidate
sequences may still use different gains; each explanation must keep its own
gain consistent across all positions. Known additive estimates and the known
robbed-position pattern remain shared between explanations.

For the same synthetic observation `G711(floor(g*s_i + e_i + n_i))`, a positive
source `s_i`, raw integer codec preimage `[L,U]` and real noise bound `R` give:

```
(L - e_i - R) / s_i <= g < (U + 1 - e_i + R) / s_i
```

Intersect the gain constraints across positions, preserving their open and
closed endpoints; negative sources reverse the inequality and endpoints.
A candidate needs one admissible gain for the complete observed wire vector.
This is a sequence contract, not permission to reuse a fitted point gain as
though it were exact for all alternatives.

For example, with intact PCMU or PCMA, zero estimate and noise, and gain in
`[15/16,17/16]`, the individual symbols 13500 and 14900 have overlapping wire
supports. Yet the two-symbol words `(13500,14900)` and `(14900,13500)` have
disjoint supports under a constant gain. In contrast, a word and its exact
1.04-scaled copy can coincide using gains 1.04 and 1. Joint inference does not
resolve arbitrary scale ambiguity. Repeating the same pair three times adds
no worst-case separation in the repeated-coordinate controls.

Exact gain-cell enumeration and an independent inverse-cell interval search
agree on 636 selected two- and six-symbol comparisons. They cover both laws,
known intact, one-of-six robbed and all-robbed patterns, both forced bit
values, zero or alternating additive estimates, and residual bounds 0 and 8.
In 128 comparisons a marginal collision disappears under the constant-gain
constraint. These counts describe selected examples, not alphabet capacity,
V.92 codebooks, BER or achievable rates. They do not model precoder/filter
memory or establish an actual channel's gain stability.

Host and Pi results and witnesses agree. Checks include 131072 native codec
inputs, 4448 native witness observations, 4072 strict-boundary controls and
216 repeated-pair controls; sanitized codec enumeration agrees too. Explicit
native counterexamples restore an alias when gain may drift between symbols,
even though every gain remains inside the declared interval. A future decoder
must therefore retain a qualified shared-gain or gain-drift constraint with its
coding, filtering and conditioning epoch, including in traceback alternatives.
Do not infer that contract merely from an immutable training fit. Keep declining
conditioned fits at the existing publication boundary; production selection
remains unchanged.

## Qualify robbed-bit phase before using a phase-specific metric

A fixed one-in-six robbed-bit channel is a shared channel parameter. If its
phase is unknown, complete competing explanations can use different fixed
phases, just as they can use different gains inside the qualified gain
interval. Requiring both alternatives to use the true phase assumes that
training has already identified it. Allowing a fresh phase at every symbol
would instead describe a different, stronger impairment.

A simple native-code example needs no gain or noise uncertainty. Under PCMU,
levels 13947 and 13435 encode as 148 and 149. Consider six-symbol words with
level 13435 only at position zero or only at position one, and 13947 elsewhere.
For every common fixed robbed position they remain distinguishable. If each
explanation chooses its own position, forcing the LSB to zero there makes
both observations six copies of code 148. PCMA has the analogous code
188/189 example. These physical level words are not asserted to be legal
complete V.90/V.92 encoder paths.

Exact forward gain-cell enumeration and separate inverse-cell intersections
agree on 6912 phase-pair comparisons in 192 selected cases. Permitting unknown
phase introduces 64 additional ambiguities. Choosing levels from different
LSB pairs protects the unit-gain examples but loses that protection in some
cases with gain uncertainty. Neither result establishes a codebook capacity,
a line rate, or an error probability.

Known training can identify phase, but it must expose the impairment. At unit
gain with no additive uncertainty, one six-symbol probe leaves six of the
12 phase/forced-bit hypotheses producing unchanged output. A second probe
with complementary LSBs distinguishes all 12 in the selected native-code
controls. This is a sufficient example, not a minimum training duration or
proof under unknown gain and noise. Carry the qualified phase or surviving
hypotheses through physical alignment and coding epochs, and verify them on
held-out observations before freezing a phase-specific channel metric.

## Expose probe bits throughout the gain interval

Complementary LSBs at nominal gain need not remain complementary after channel
gain and quantization. For example, the PCMU levels 13947 and 13435 encode as
148 and 149 at unit gain. At gain `12668/13435`, inside `[15/16,17/16]`, their
floored inputs 13150 and 12668 both encode as 150. Forcing zero at any robbed
slot becomes invisible even without noise, using the same gain for both
explanations. Nominal bit coverage alone cannot qualify an impairment probe.

Consider six copies of each level as a twelve-sample probe. In the synthetic
bounded-noise model above, with zero additive estimate, gain in
`[63/64,65/64]` and noise in `[-64,64]`, it leaves overlapping supports among
intact and the twelve fixed phase/forced-bit hypotheses. Repeating it to make
24 samples preserves the ambiguity. Instead, append six copies each of 8827
and 8315 for PCMU, or use 13055/12543 followed by 8959/8447 for PCMA. These
selected 24-sample probes separate all thirteen hypotheses over that entire
uncertainty range, even when competing explanations use different gains.
They use the same nominal peak and less mean-square source energy than the
repeated probe.

This protection has bounds. Under gain in `[15/16,17/16]` and noise in
`[-64,64]`, even the two-amplitude probes retain ambiguities. None of six
selected third-amplitude additions removes all of them. Repetition cannot
remove a worst-case support collision under independently bounded noise:
repeat its witness with the same gain. That result does not describe averaging
under a stochastic noise model.

Exact forward gain-cell rectangles and separate inverse-cell intersections
agree on 10452 comparisons in 134 selected cases. Host and Pi results and
native witnesses agree, with explicit half-open endpoint controls. These
physical training words do not establish legal encoder paths, minimum probe
length, capacity, BER or the uncertainty of an actual call. Design probe
amplitudes jointly against the qualified gain/noise range, retain surviving
channel hypotheses, and verify the resulting metric on held-out observations.
Existing V.91 DIL already probes every octet and both signs; its training and
production rate selection remain unchanged.

## Check native training against the complete signalling pattern

One robbed position in six need not force the same bit on every occurrence.
[G.704 (10/1998), 3.1.3.2](https://www.itu.int/rec/dologin_pub.asp?id=T-REC-G.704-199810-I%21%21PDF-E&lang=e&type=items)
assigns separate A/B bits in a twelve-frame multiframe and A/B/C/D bits in a
24-frame multiframe. A stable pattern therefore needs a phase and its whole
repeating bit word. This model holds those bits fixed over the observations;
it does not cover signalling changes, slips or multiple robbed positions.

Test exposure using actual encoder samples, including scrambling, coding,
prefilter history and Q15 scaling. For each position `i`, take the observation
as `G711(floor(g*s[i] + n[i]))`, followed by the signalling-bit replacement.
The gain `g` is shared across the word; each `n[i]` is independently bounded.
For a native codec cell `[L,U)` and noise bound `R`, a positive sample permits
`(L-R)/s[i] <= g < (U+R)/s[i]`; negative samples reverse the endpoints. Intersect
unions of cells having the required LSB across each repeating position,
retaining endpoint inclusion. A surviving gain constructs an intact wire word
that the signalling replacement leaves unchanged. With a common pre-overwrite
channel support, this also exhausts intact-versus-fixed-pattern intersections
with independently chosen gains, as shown below.

A concrete native example uses the 40k PCMU direct mapping, 16-state trellis,
upstream level 4096 and gain normalization from its B1 energy. Within gain
`[63/64,65/64]`, its first 48 B1 samples expose all constant-bit and twelve-slot
patterns without noise, but two 24-slot patterns remain transparent. One has
zero-based robbed phase 1, repeating bits `1110`, and gain `118609/116864`.
Its eight affected octets are `231,231,47,192,175,231,211,42`; replacement
changes none. A constant forced-bit check would miss this ambiguity.

For that trellis and level, all four selected 576-sample native B1 words have
no same-gain transparency witness without noise. With independently bounded
noise `[-32,32]` in 16-bit PCM units and the same gain interval, the surviving
pattern counts are:

| Mapping | Constant bit, out of 12 | Four-bit word, out of 96 |
| --- | ---: | ---: |
| PCMU 36k | 1 | 8 |
| PCMU 40k | 7 | 49 |
| PCMA 36k | 0 | 0 |
| PCMA 40k | 0 | 0 |

These are worst-case support collisions, not their probabilities. The native
study covers both laws, 36k/40k, all three trellises, three upstream levels,
gain intervals `[63/64,65/64]` and `[15/16,17/16]`, and noise bounds
`0,8,32,64,128`. Three of 36 waveforms saturate and
are excluded from the bounded analysis; the remaining 33 yield 3960 prefix
cases. Independent forward gain-cell enumeration checks 120 selected native
cases, with additional endpoint controls and constructive native-code witnesses.
Native waveform generation shares encoder primitives with its grouping check;
it is not an independent standards implementation. Payload prefixes use one
deterministic bit stream. These results neither qualify a physical call's noise
bound nor establish BER, a safe rate or a shorter B1 duration. Retain unresolved
phase/pattern alternatives until held-out evidence separates them, and carry
their union into constellation metrics. Production negotiation is unchanged.

## Use projection identity only for intact-versus-robbed comparisons

Let `S` contain the possible wire words before signalling replacement for one
known transmitted word and the declared channel uncertainty. A fixed overwrite
`P` is idempotent: `P(P(z)) = P(z)`. Therefore
`S ∩ P(S) = {z ∈ S : P(z) = z}`. Every output of `P` is fixed by `P`; conversely,
a fixed word in `S` is also its own image. The transparency search above is
thus complete for intact versus that fixed pattern, even when the competing
explanations may choose different gains or noise within the same support.
It need not enumerate two gains for this particular comparison.

This identity requires the same pre-overwrite support and a fixed idempotent
operation at the observation boundary. It does not identify two different
robbed patterns or distinguish different transmitted payload words. Those
comparisons can still require independent gains in the competing explanations.

An actual native 36k PCMU example uses the 16-state trellis, upstream level
4096 and its first 24 B1 samples. Within `[63/64,65/64]`, two robbed models
produce the same word with these parameters; forced bits are listed in order
of successive robbed slots, and phase is zero based:

| Robbed phase | Repeating forced bits | Gain |
| --- | --- | --- |
| 1 | 1111 | 5756/5811 |
| 5 | 1101 | 5755/5811 |

Before replacement, octets at indices 13 and 17 are `(40,168)` for the first
model and `(41,169)` for the second. Both produce `(41,168)` after replacement;
the remaining observed octets agree too. There is no common-gain alias for
this pair anywhere in that interval. A same-gain pairwise check would miss it.

Exact forward supports compare 97 unique hypotheses: intact plus six phases
of all sixteen four-bit patterns, including shorter repeating patterns.
Across 24 selected native cases, independent-gain comparisons find 56 extra
ambiguous pairs, counting each case separately. They cover both laws,
36k/40k, that trellis
and level, three B1 prefix lengths and the two gain intervals above, without
noise. All eight selected full 576-sample B1 cases separate these hypotheses;
that does not qualify a noisy call or justify shortening training.

Host and Pi agree on 111744 pair comparisons per host, with inverted-support
and direct set-intersection checks sharing the codec and waveforms, plus
native-code witnesses. The projection identity also passes 5308416 finite-set
checks over 81 overwrite
operators, including controls rejecting its extension to different projections,
different base supports or non-idempotent operations. This strengthens the
intact-versus-robbed analysis; it establishes no BER, rate or payload guarantee.

## Budget gain uncertainty when qualifying signalling exposure

Noiseless separation need not survive a small additive uncertainty. Under the
same model, bracket the noise bound at which an intact word can also explain
a fixed one-in-six signalling overwrite. The gain stays constant throughout
each word. Noise may vary independently at each sample; this is worst-case
support, not an averaging or stochastic-noise calculation.

For the full 576-sample native B1 words with 16-state trellis and upstream
level 4096, each table entry gives `L / U` in signed 16-bit PCM input units.
No intact-versus-fixed-pattern collision exists for any noise bound at or
below `L`; a constructive collision exists at bound `U`. The gap is exactly
`1/256` unit. The two gain intervals are closed.

| Native word | Gain `[63/64,65/64]`: L / U | Gain `[15/16,17/16]`: L / U |
| --- | --- | --- |
| PCMU 36k | 28.234375 / 28.23828125 | 28.234375 / 28.23828125 |
| PCMU 40k | 10.46484375 / 10.46875 | 0.26171875 / 0.265625 |
| PCMA 36k | 47.87109375 / 47.875 | 47.63671875 / 47.640625 |
| PCMA 40k | 45.33203125 / 45.3359375 | 44 / 44.00390625 |

These are properties of these generated words and their normalization, not
general codec rankings. In particular, the wider gain interval makes the
selected PCMU 40k word ambiguous with much less additive uncertainty.
Treat gain confidence and additive uncertainty jointly when qualifying a
training-derived impairment model.

Even a full word need not expose every pattern without noise. The native
40k PCMU word at upstream level 2048 and 16-state trellis has a witness at
gain `496839/491194`: forcing four repeating ones at zero-based robbed phase
zero changes none of its 576 observed octets. This gain lies in both intervals
above. Across all three trellises and three upstream levels, nine of the 66
full-word gain-range cases already have a noiseless collision. All 66 selected
48-sample prefix cases do. Three saturated native waveforms are excluded
before this analysis, leaving 33 waveforms and 132 cases in total.

Exact rational inverse intervals and separate forward gain-cell enumeration
agree at all 189 tested endpoints, with 48 additional boundary controls per
host. Every feasible endpoint has a native-code transparency witness. Host
and Pi results agree exactly, and the reviewed native generator sources still
match the current encoder. These calculations share the codec and qualified
waveforms; they are not an independent standards implementation.

The comparison is complete for intact versus each fixed overwrite with the
same pre-overwrite support, by the projection identity above. It does not
qualify separation between two robbed models or different payloads. Retain
unresolved signalling hypotheses and test their union on held-out data;
training duration alone does not justify choosing one. No physical noise
bound, BER, safe rate, shorter B1 or production negotiation change follows
from these model-specific bounds.

## Compare two robbed explanations before selecting a channel model

A signalling model may fit the known training word while another robbed
model explains the same observations with a different gain. Compare their
complete output supports; the projection identity for intact-versus-robbed
comparisons does not establish this pairwise result. Keep one gain per word
in each explanation and independently bounded noise at every sample.

An exact finite comparison is possible under the model above. Split each
closed gain interval at every codec-cell boundary shifted by either noise
endpoint and divided by each nonzero transmitted sample. Include every cut
and one interior point between consecutive cuts. Every sample's possible
codeword set stays constant within each open interval. The Cartesian product
of these representatives therefore covers both independently chosen gains,
including endpoint-only intersections. Apply each fixed signalling overwrite
to its codeword sets and intersect the resulting supports at every sample.

The selected 576-sample native B1 words use the 16-state trellis and upstream
level 4096. Across both laws, 36k/40k, both gain intervals and noise bounds
0, 8 and 32, all 4656 pairs of the 97 hypotheses were checked in each of
24 cases. The 96 robbed hypotheses cover all six phases of all sixteen
four-bit patterns. Different patterns at the same robbed phase cannot agree:
they force different observed bits at a position present in the word.

For the selected PCMU words, bracket the first collision between two robbed
models. Entries are `L / U` in signed 16-bit PCM input units: no pair collides
at or below `L`, and a constructive collision exists at `U`. Every gap is
`1/256` unit; both gain ranges are closed.

| Native word | Gain `[63/64,65/64]`: L / U | Gain `[15/16,17/16]`: L / U |
| --- | --- | --- |
| PCMU 36k | 66.67578125 / 66.6796875 | 56.44921875 / 56.453125 |
| PCMU 40k | 10.46484375 / 10.46875 | 0.26171875 / 0.265625 |

The two comparison questions have different answers. At noise bound 32,
the selected PCMU 36k word has eight intact-versus-robbed ambiguities in
each gain range but no ambiguity between two robbed models. The selected
PCMU 40k word has 774 ambiguous robbed-model pairs in each gain range;
a common-gain restriction finds only 771. Equal-gain witnesses happen to
attain the first-collision brackets above, but that does not make an
equal-gain comparison complete at other uncertainty bounds.

Host and Pi agree exactly. Direct wire-word hashing checks every pair in
eight additional noiseless native-prefix cases. Separate Cartesian support
enumeration checks 52 selected noisy or endpoint pair cases; native codec
witnesses also verify the six independent-gain-only pair instances and the
four bracket endpoints. These checks share qualified native waveforms and
codec definitions, not an independent standards encoder. The reviewed native
generator inputs still match the current encoder.

These are known-word support bounds, not payload-distance, BER or physical
noise measurements. They support carrying every unresolved channel explanation
into held-out decisions, rather than selecting one because training is long.
They do not establish a safe rate, shorter B1 or a codec ranking, and change
no production negotiation policy.

## Bound pre-codec noise on complete error events

A bounded-noise radius answers a different design question from summed squared
separation or a stochastic overlap. For two transmitted samples, find the
infimum of radii permitting a common observed codec octet, with separate
noise in each explanation before floor/G.711. Search exact cell preimages,
including their half-open upper edges. For a known shared robbed phase, forcing
bit zero or one gives the same equality classes: octets agree after replacement
exactly when their values with bit zero masked agree. The phase still matters.

Combine sample costs with a maximum, then minimize over legal branch pairs and
complete diverging/remerging paths. A minimax graph replaces the summed edge
metric above. Integer-scaled radii retain an additional open-endpoint flag.
Clipped node potentials satisfy `h(destination) <= max(h(source), edge)`;
first-different seeds bound their destinations and every diagonal target has
the claimed cost. An attaining native source pair supplies the opposite bound.
This covers arbitrary finite events rather than a selected bit-flip bank.

For the full-capacity, memoryless direct mappings at upstream level 4096,
normalize each coder's native B1 gain independently. At known unity channel
gain, all three trellises give the same minimum radius below. Units are signed
16-bit PCM input units; open means collision requires a strictly larger radius,
while closed means collision already exists at that radius.

| Mapping | Transparent | Worst known one-in-six phase |
| --- | --- | --- |
| 36k, mu-law | 119, open | 119, open |
| 40k, mu-law | 76, open | 76, open |
| 36k, A-law | 140, closed | 140, closed |
| 40k, A-law | 89, closed | 85, closed |

A larger coder can require errors at more positions without increasing their
maximum required amplitude. For example, attaining transparent 36k events
change four symbols with 16/32 states and seven with 64 states, but all have
the same radius here. This leaves the earlier summed-distance result intact;
neither criterion alone describes a stochastic channel or decoder BER.

Gain samples can miss an interior weakness. The 36k mu-law radius is 135.8125
at gain `63/64` and 134.1875 at `65/64`, versus 119 at unity. These are separate
known-gain channels, not a solution for a continuous gain interval or competing
explanations with independently fitted gains.

Across both laws, both rates, three coders, three known gains and seven wire
modes, host and Pi agree on all 252 graphs. Independent C/Python constructions
agree on 677376 branch weights; 281788416 potential inequalities and a separate
threshold-reachability algorithm check the optima. Every native source witness
fully remerges coding state and produces eight common continuation frames;
independent integer encoding and native forward noise/codec witnesses agree.
These checks share the reviewed native encoder and codec definitions, not an
independent standards implementation.

Use these bounds to reject incomplete constellation objectives. They do not
qualify uncertain signalling models, stateful learned filters, continuous gain
uncertainty, native finite traceback, actual channel noise or physical interop.
No safe rate, BER, shorter decoder delay or negotiation change follows.

## Screen level banks with both event bounds and the native receiver

Complete-event separation is a useful design constraint, but maximizing one
separation measure is insufficient. A bounded 36k study compared the current
linear bank against 260 cubic banks per law. Keep the 54-bit modulus capacity,
identity prefilter and 16-state coder; normalize every candidate from its actual
48-frame B1u waveform and round-trip its native CPd record. Admit candidates
only when actual scaled B1u energy and the equal-weight per-interval signed
alphabet mean square are no larger than the baseline. The latter is a declared
power comparison, not a normative power or payload-distribution proof.

For positive index `i=0..15`, let `x=2*i+1`. Generate the family by rounding
`P*x*(4*31^2+s*x^2)/((s+4)*31^3)` to the nearest positive integer, with
`P` in `{8192,12000,20000,30000}` and integer `s=0..64`. Include the exact
current bank separately. At LU 4096 the retained candidate has `P=20000`,
`s=2` and native gain field F=27. Its B1u energy is 9,339,177,147 versus
9,792,004,992, its alphabet mean square is 15,624,841.9375 versus 16,491,472,
and its peak sample increases from 7,564 to 8,437. These are different power
and peak constraints; the larger peak remains a qualification cost.

The objective is the minimum complete-event bounded-noise radius over three
separate known shared gains, `63/64`, `1`, and `65/64`, and transparent or any
one known one-in-six robbed phase. It does not combine independently uncertain
channel explanations or certify the continuous interval between gain samples.
Using the pre-codec floor/G.711 model from the preceding section gives:

| Law | Current bank | Candidate bank |
| --- | --- | --- |
| mu-law | 119, open | 164, open |
| A-law | 116.6875, closed | 168.640625, closed |

The search rejects 110 candidates by power and bounds 405 by a single tested
channel that cannot beat the incumbent. Independent branch construction,
clipped potentials, threshold reachability and fully remerging native source
pairs check the finalists, all pruned banks and three earlier incumbents.
All 660 retained certificates were reread; host and Pi agree on their native
source witnesses. This establishes the result within the declared finite
family and channel models, rather than a global constellation optimum.
Normalizing the same positive table with the 32-state coder instead gives
F=28 and exceeds both comparison budgets. Recheck power for every coder.

The native receiver supplies a counterexample to selecting the candidate
universally. For each law and noise peak, use sixteen predeclared deterministic
payload/noise seeds, clean B1u, and bounded noise introduced after eight payload
frames. Check all 400 payload frames exactly. The direct raw-constellation
path starts at a valid first-B1u handoff, receives actual wire octets, and gives:

| Noise peak | Law | Current exact-payload passes | Candidate passes |
| --- | --- | --- | --- |
| 192 | mu-law | 7/16 | 15/16 |
| 192 | A-law | 16/16 | 15/16 |
| 224 | mu-law | 0/16 | 0/16 |
| 224 | A-law | 10/16 | 0/16 |

Both banks pass all sixteen cases at noise peak 160 in both laws. The separate
E2u/filter-output fixture also regresses at A-law peak 224, from 9/16 to 0/16;
its mu-law peak-192 results are 11/16 and 15/16. Keep these receive paths
separate. The raw fixture does not qualify E2u alignment. Noise rounding in
these fixtures uses the existing nearest-integer quantizer; the event bound
uses floor. These case counts are neither BER estimates nor field call rates.

A companion complete-event distance check in squared decoded G.711 units does
not explain away the A-law failure: at unity gain its transparent distance rises
from 262,144 to 495,616, and its worst known robbed-phase distance from 131,072 to 437,248.
For mu-law those pairs are 589,824 to 518,400 and 507,904 to 512,704. Neither
this metric nor the bounded-noise radius alone predicts the implemented
receiver's response to noise introduced after training. The failures do not
by themselves identify a traceback, acquisition or decision-metric bug.

Retain the current selector. Any proposed replacement must also qualify actual
receiver coordinates, training uncertainty, transient noise, physical gain and
peak headroom. No default, wire timing, admission threshold or physical-call
grade changes follow from this study.


## Account for probabilities of error events

A follow-up keeps the same 192 raw-receiver conditions and compares four
scoring arms. The native arm reproduces every original grade and first-miss
trace. The Gaussian arm supplies the synthetic noise standard deviation
`peak/sqrt(3)` in frozen calibrated coordinates. An integer-coordinate arm
instead uses the known physical gain and exact transmitter rounding to measure
distance to the received integer G.711 cell. A fourth arm sums the exact cell
mass over the fixture's 65,536 possible 16-bit noise draws, scoring by
`-2e6*log(probability)`. The latter assumes independent draws for sequence
ranking; the deterministic LCG stream is not evidence of independence.

These are diagnostic oracles: they know the channel, noise level and onset
after clean B1u. Every arm retains the native coding and traceback. Each table
entry is current/candidate exact-payload passes, each out of sixteen:

| Peak/law | Native | Gaussian | Integer distance | Discrete marginal |
| --- | --- | --- | --- | --- |
| 192/mu-law | 7/15 | 7/15 | 12/15 | 15/15 |
| 224/A-law | 10/0 | 6/0 | 10/0 | 12/1 |

The identity prefilter's Q15 rounding precedes F/LU scaling in the physical
oracles, including large points changed by that rounding. Exhaustive encoding
checks 25,165,824 noise draws and 98,304 exact octet masses, including distinct
PCMU zero cells. Corrected physical coordinates and matching marginal scoring
help some cases but do not rescue the candidate's A-law regression. Gaussian
scoring alone is not a supported cure for noise appearing after training.

A separate finite model makes the probability tradeoff explicit. Give each
of the 32 signed points equal prior probability and let `M(i,o)` count the
65,536 noise draws producing network octet `o` from point `i`. The best
memoryless point classifier has error probability

```
1 - sum_o max_i M(i,o) / (32 * 65536).
```

At A-law peak 224, this value increases from `83975/2097152` (4.00424%) to
`121790/2097152` (5.80740%), despite the candidate's improved worst-case event
bound. This declared uniform-point model is neither coded BER nor the actual
payload prior: the mapping also contains inner-alphabet positions. It gives a
concrete reason to include probabilities of confusion and event multiplicity
alongside minimum separation. It does not establish the complete
coded-error mechanism or a globally optimal bank. Retain the current selector;
future candidates need both probabilistic models and native decoding checks.
