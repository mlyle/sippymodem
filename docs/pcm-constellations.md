# PCM upstream constellation selection

The direct V.92 selector chooses amplitudes for a scalar G.711 channel. It
keeps the requested rate, modulus schedule, trellis and identity filter, then
searches a bounded family of point sets. The filtered-channel designer has a
separate model and selection path.

## Measure noise before the codec

Distance between reconstructed G.711 levels is an incomplete measure of
robustness. Two well-separated reconstruction values can have source samples
close to a shared quantizer boundary. Small analogue noise can then make their
received codes identical.

For adjacent source samples `a < b`, define `d(a,b)` as the smallest integer
noise peak for which their quantized output ranges overlap. Equivalently, it
is the first `e >= 0` for which `Q(a + e) >= Q(b - e)`. A peak smaller than
`d` preserves their distinction. Both signs need checking because G.711 input
intervals have signed rounding asymmetry.

TRN1u reports a quantized transmit level, LU. Until training refines it, the
actual level can lie anywhere in that codec interval. Score a constellation
by the minimum `d` over every adjacent pair and every integer LU in that
interval. This measures a bounded pre-codec noise budget, rather than a
reconstructed-code gap at one assumed level.

## Preserve power and uncertainty constraints

Every candidate is normalized using the actual signed B1u symbol frequencies
and the transmitter's fixed-point filter rounding. The representable gain is
chosen to make the scaled mean-square prefilter output nearest unity. This
follows the power convention in [V.92, clause 8.8.3 and Table 30](https://www.itu.int/rec/dologin_pub.asp?id=T-REC-V.92-200011-I!!PDF-E&lang=e&type=items).
Full-cell candidates that clip or alias within the LU interval are rejected.

At 40–45.333 kbit/s and 48 kbit/s, full-cell selection maximizes this noise
budget within the existing codec-window and linear/cubic families. At
46.667 kbit/s, the selector first tries the existing policy of separating
entire symbol ranges across LU uncertainty. If that fails, it uses the
full-cell noise objective. Below 40 kbit/s, nominal selection is retained.
The existing nominal fallback at 40 and 41.333 kbit/s is also retained.

The search need not score every candidate expensively. Its minimum over the
LU interval cannot exceed its score at the reported LU. If that cheap upper
bound cannot beat the retained candidate, exact interval scoring can be
skipped. This pruning preserves the optimum within the searched family.

## What this establishes

Tests perturb adjacent symbols in opposite directions, through both codecs,
at every LU in each witness interval. They also check B1u power normalization.
This establishes scalar separation under the tested bounded noise. It does
not establish a bit error rate, account for every channel impairment, or prove
global optimality over all representable constellations. Native call tests and
physical modem calls remain necessary to assess training and payload behavior.
