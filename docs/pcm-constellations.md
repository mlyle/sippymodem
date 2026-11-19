# Generated PCM downstream constellations

When the analogue role owns generated downstream parameters, it chooses the
highest enabled rate whose qualified levels and power budget permit a map.
For that rate it maximizes the smallest signed linear-level distance across
all six phases. Explicit caller-owned maps and maps received by the digital
role retain their negotiated levels, cardinalities and labels.

V.90 gives each phase its own cardinality, Mi. The six-radix product must
cover all 2^K modulus words; the mapper labels each set in descending Ucode
order. V.92 uses this downstream mapping through CPu. See
[ITU-T V.90, clauses 5.4 and 8.5.2](https://www.itu.int/rec/dologin_pub.asp?id=T-REC-V.90-199809-I!!PDF-E&lang=e&type=items).

The generator searches the existing common-cardinality design plus the
floor/ceiling sixth-root family, retaining the existing minimum of eight
levels. There are at most 64 assignments of the two cardinalities across six
phases. For example, five 57-level sets and one 58-level set carry K=35 where
an equal 58-level requirement cannot use masks containing only 57 levels in
some phases. With five sign bits this carries 53,333 bit/s.

An individual impaired phase can have fewer than the root's required levels
while the other phases still have spare capacity. For example, fixed robbed
bits on one phase can collapse approximately half its distinguishable levels.
Requiring almost equal cardinalities then rejects a feasible higher-rate map.
The generator adds fixed families for this case: count each phase's qualified
levels through the existing upper rail, cap the scarce phases at each size
from eight upward, and find the smallest common ceiling that lets the other
phases cover the required radix product. Enumerate that ceiling and its floor
only where they differ. The original family wins whenever it fits at the
same distance. This preserves existing maps unless spacing strictly improves.

Counting qualified levels at unit spacing can miss a phase whose levels are
clustered. After the first search, count each phase's greedily spaced levels
at one unit above the best distance. Try the asymmetric families from those
caps if they can improve the map under the same power ceiling. This lets
a phase trade capacity at a distance where its extra codes cannot all be
used. The second search only replaces a map at a strictly greater distance
with at least the previous noise reserve across the guaranteed postcursor
range. The
resulting map can use more of the allowed transmit power.

Each search fixes its families before bisecting. There are at most 103
families per search through the existing rail. The original family has at
most 64 assignments. Each added family has at most 32 because at least one
phase has a fixed cap. Both searches use the existing enabled rates, power
ceiling, sign redundancy, training constellation, and qualified masks.

For a proposed distance, greedily taking the lowest available positive level
at each step minimizes every ordered magnitude. The first magnitude must be
at least half the distance, so its negative counterpart also satisfies the
spacing requirement. Since all word-occurrence weights are nonnegative,
this gives the minimum power for that fixed cardinality and distance.
Feasibility is monotone, permitting integer bisection across the family.
Equal distance retains the common-cardinality baseline and original root
family. Other ties use the lower exact power ceiling, then stable family
and phase-pattern order. Each search finds the greatest distance within
its families before the replacement is checked against the earlier map.

## Exact power without enumerating words

Unequal radices do not visit every label equally. For W=2^K words and stride
A (the product of earlier radices), let R=floor((W-1)/A), q=floor(R/M), and
b=R mod M. Every label occurs A*q times; labels below b occur A additional
times, and label b occurs W-A*R additional times. Thus one phase's exact
weighted sum of squared magnitudes is:

```
A*q * sum(all squared levels)
  + A * sum(squared levels with descending labels below b)
  + (W-A*R) * square(level with descending label b)
```

Ascending cumulative sums of squared levels evaluate these three terms in
constant work per phase. The generator prepares one ordered level prefix
per phase and distance, then reuses it across all family assignments. A shared
16-bit limb accumulator preserves the complete numerator even when a direct
64-bit product would overflow. Admission uses the exact ceiling of total
power divided by 6*W, against the existing integer power limit.

The search has no heap allocation or retained cache. Its temporary cumulative
sums are local to one design call. Training retains fixed cardinalities, the
common-row training selector keeps its original levels, and all generated
data selections use the same six-phase entry point and regenerate capacity
from their current K. Generated configurations
accept mixed data rows while still validating capacity and canonical framing.
DIL qualification, its common-mask fallback, rate policy and B1 admission are
unchanged.

## Verification boundaries

Focused tests exercise mixed 57/58 capacity, exact power boundaries and failed
selection without partial output. CP and CPu round trips and configuration
checks cover both codecs. An independent small-K oracle enumerates subsets of
qualified levels, using full radix-cycle occurrence counts to reject any
better spacing within the family. The asymmetric oracle also exercises a
scarce nine-level phase in each of the six positions, both laws, two power
limits, and rejection without partial output. Separate integer identities cover
wide prefix products through K=42; existing exhaustive mapping tests also remain.

Design-call timing and full-call timing are different measurements. The
former includes complete rate sweeps to bound selection work; the latter
must also cover the DSP, framing, compression and training transitions.
Neither synthetic self-calls nor exact replay of a received recording proves
interoperability with every physical digital peer.

## Design the probe for phase-dependent memory

The DIL descriptor lets the analogue modem choose a signed reference or
training level at each point in a repeating pattern. Each segment starts
the pattern again. The descriptor also carries a reference and a segment
length for each Ucode chord. These fields can probe both the present
level and the preceding level at each of the six receiver phases.
[ITU-T V.90 §§8.3.1 and 8.4.1](https://www.itu.int/rec/dologin_pub.asp?id=T-REC-V.90-199809-I%21%21PDF-E&lang=e&type=items)
[ITU-T V.92 §8.6.1 and Table 20](https://www.itu.int/rec/dologin_pub.asp?id=T-REC-V.92-200011-I%21%21PDF-E&lang=e&type=items)

Represent the four signed reference and training choices by four symbols.
Make one vertex for each pair of phase and preceding symbol. Each vertex
has four outgoing edges, one per current symbol, into the next phase.
The graph is strongly connected and has equal in-degree and out-degree
at each vertex. An Euler circuit generates a 96-symbol pattern that
visits every ordered pair once in each phase. This balance also makes
the signed reference and training sequences separately zero-mean over
a full period.

Use 120 samples per segment when the receiver skips the first 18 samples
for settling. A full 96-symbol cycle then fits inside positions 18
through 118, even when the last sample goes to the next stage. The
144-segment probe retains the conventional training levels and takes
17,280 samples instead of 32,280, saving 1.875 seconds of probe time at
8,000 symbols per second. A 102-sample budget leaves at most 84
observations after settling.

Screen candidate patterns against both codec laws. For a given reference
level, the exact squared energy of each window is the sum of the training
and fixed reference contributions plus the count of the remaining
reference symbols times their squared level. This permits a direct
bound on the largest admissible reference level. Use the conventional
probe as the ceiling for mean-square traversal power and maximum 10 ms
and 20 ms window energy. Prefer the highest admissible reference level,
then minimize the largest spectral bin of the two signed pulse trains,
allowing for their level weights.

Transition coverage establishes which channel inputs were exercised.
The precision of a codec-cell fit also depends on the levels, noise,
and number of repetitions. In a paired test of the 120-sample design,
fixed-rate calls preserved the conventional outcome in 112 cases across
both laws and both roles. In the 68 cases that verified bidirectional
payload, the shorter probe advanced data readiness by 1.88–1.94 seconds.
The longer conventional probe gave tighter median gain and postcursor
bounds on the sampled codec models. Qualification of a replacement
also needs rate-flexible trials, media loss, and an independent digital
peer that decodes the requested descriptor.

## Probe the second postcursor within the short segment

At 96 symbols each wire phase gets exactly 16 observations. If all
three pairs from the current symbol and its two predecessors are
balanced, that phase takes each state `(a,b)` once and advances it to
`(b,c)`. The rule giving `c` from `(a,b)` is a four-by-four Latin square.
Enumeration of the 576 squares gives an even permutation of the
16 states in each case. Composing six even permutations cannot give
the odd permutation required for a single cycle through all 16
states at the starting phase.

Choose six such permutations whose composition has two cycles.
Exchange the outputs of two states with the same `b` in one phase,
one from each cycle. The exchange joins the cycles and preserves
every adjacent pair. It leaves 94 of 96 pairs at lag two. Both gaps
are in that one phase. Two is the minimum because all four row and
column totals in each pair table are fixed, so one missing pair
cannot be balanced by one duplicate at a different position.

The screened pattern uses the same 120-sample segments, 115 distinct
training levels and reference level as the adjacent-pair design.
Within a settled 96-sample interval, the signed current and two
prior levels have zero mean. Their normalized Gram matrix is the
identity in five phases. Across both laws and all trained levels the
worst condition number falls from 3.00 to 1.67. Mean probe power and
the largest 10 ms energy are equal for the two short patterns. The
largest 20 ms energy decreases by 2.4% with the new pattern.

In an exact codec-cell fit with signed two-tap channels, both short
probes identify all 288 planted bit-substitution modes. Across
3,456 held-out triples, the median prediction interval narrows
from 6.27 to 6.12 linear PCM units. The number of ambiguous received
cells falls from 1,316 to 1,296. The long probe gives a 5.04-unit
median interval and 1,268 ambiguous cells.

Eight clean SIP pairs using a separate digital binary verified
LAPM data at the same rates with the long and new short probes.
The short version finished 1.879–1.901 seconds sooner. Across 24
milder signed two-tap cases, the long and previous short probes
each verified two-way data in 23 cases and the new waveform in 22.
Both short probes reached at least 1,000 verified bytes per direction
in 18 cases. The four captured downstream training epochs from
one physical destination all admit a single-postcursor fit. The
second-lag design still needs a qualification criterion that
predicts native data delivery.

## Decision margins with two quantized postcursors

Linear minimum spacing does not by itself describe a sampled channel with
memory. Consider the specific integer model

```
z = trunc((32768*c + q1*(p1 - c) + q2*(p2 - c)) / 32768)
received = G711(clip16(z))
```

Here `c` is the current reconstruction and `p1` and `p2` are the preceding
two reconstructions. The first coefficient `q1` ranges from 0 to 32768.
The second coefficient `q2` ranges from -32768 to 32768. The receiver
uses its fixed native hard slicer after this additional codec. This models a
quantized causal channel. In particular, decoding and re-encoding mu-law
negative zero loses its sign even when both coefficients are zero.

For each signed point `c`, enumerate the encoder and native slicer to obtain
its inclusive raw-input decision region `[L(c), U(c)]`. Verify that this region
is contiguous and contains `c`. These regions include codec cells, polarity,
saturation and the slicer's tie rule; reconstructed-level midpoints alone
are insufficient. Wider linear spacing or a lower bit rate need not enlarge
all of these regions relative to the predecessor's peak amplitude.

For division that truncates toward zero, the inclusive numerator bounds are:

```
A = L*32768                 if L > 0, else (L-1)*32768 + 1
B = U*32768                 if U < 0, else (U+1)*32768 - 1
```

With additive disturbance before truncation, the exact symmetric reserve in
numerator units is the minimum of the lower and upper boundary supports
over all current points and phases. In each sector `s = sign(q2)`, write
`x = q1 + q2` and `y = |q2|`. If `P1` and `P2` are the peaks of the two
preceding phases, the supports are

```
32768*c - A - (P1 + c)*x + (s*P1 - P2)*y
B - 32768*c - (P1 - c)*x + (s*P1 - P2)*y
```

Each predecessor contributes linearly, so its extreme label gives the
worst case for each boundary. The outside rails need no constraints because
clipping keeps those labels. Reflecting a boundary and its neighbors keeps
the support's slopes, so the implementation keeps the tighter support.

The supports of a phase share their slope in `y`. Its lower envelope
therefore changes active support only at its breakpoints in `x`. The
selector divides each sector into strips at the first map's phase
breakpoints, then clips each strip to where all its active supports are
nonnegative. Within each strip, the region where one baseline support is
the minimum is a convex polygon.

The candidate's reserve is concave. Subtracting the affine baseline
support on each polygon preserves concavity, so checking the difference
at every polygon vertex suffices. Integer cross products give exact
comparisons, including on the lines shared by neighboring polygons.
This includes the first-order test along `q2 = 0`. A certificate that
exceeds the fixed work budget retains the first map at the original rate.

The stream-receiver regression obtains decisions through normal native receive
calls, exhausts every signed 16-bit encoder input, and independently enumerates
all symbol pairs and coefficients through the first failing coefficient. It
covers both laws, low-level polarity, a codec segment boundary and saturation.
The profile regressions additionally check maps whose wider alternatives
lose reserve under a second postcursor.

A channel-aware design can use these margins alongside exact radix capacity,
occupation-weighted power, peak limits and noise tolerance. Improving only
postcursor tolerance can reduce ordinary linear spacing; equal worst-case
noise radius also does not prove equal average error probability. A proposed
layout needs independent channel/noise holdouts and native control, B1 and
payload qualification. An automatically selected rate additionally needs a
receiver-owned, independently acquired channel bound that remains valid for
the epoch. Missing channel evidence must not be replaced with an assumed
postcursor coefficient.
