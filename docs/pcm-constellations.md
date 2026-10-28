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

These families are fixed before distance bisection. There are at most 103
families through the existing rail: at most 64 assignments in the original
family, and at most 32 in each added family because at least one phase has a
fixed cap. This deliberately bounded search does not enumerate arbitrary
six-cardinality vectors. It does not change the maximum enabled rate, power
budget, sign redundancy, training constellation, or DIL qualification.

For a proposed distance, greedily taking the lowest available positive level
at each step minimizes every ordered magnitude. The first magnitude must be
at least half the distance, so its negative counterpart also satisfies the
spacing requirement. Since all word-occurrence weights are nonnegative,
this gives the minimum power for that fixed cardinality and distance.
Feasibility is monotone, permitting integer bisection across the family.
Equal distance retains the common-cardinality baseline and original root
family. Other ties use the lower exact power ceiling, then stable family
and phase-pattern order. This is optimal within the stated family, not over
arbitrary six-cardinality maps.

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
