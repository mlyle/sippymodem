# Learning digital PCM channels

The digital-to-digital path measures received octets with `pcm_octet_probe`
and selects a mapping with `v91_profile`. This differs from analogue V.92
upstream selection, whose [noise and power model](pcm-constellations.md)
works before the codec. A digital path can have pads, law conversions,
polarity changes, or robbed bits even when its RTP transport is lossless.

## Select distinguishable receive sets

For each transmitted octet and each of six frame positions, the probe stores
the set of received octets. The selector considers magnitude codes in
ascending order, retaining a code only when both signs have repeated
observations and their output sets overlap neither each other nor any
previously retained code. The receive table then maps each retained output
back to its transmitted octet. Unknown outputs remain unknown.

At the nominal 8,000-symbol/s clock, a six-symbol frame carries six sign bits
and at most `floor(log2(product of six magnitude-set sizes))` magnitude bits,
subject to the mapper's rate and power limits. A clear path can carry 48
bits/frame, or 64 kbit/s. Losing one magnitude bit in every position gives
42 bits/frame, or 56 kbit/s; losing it in just one position gives 47
bits/frame, or about 62.667 kbit/s. Equal constellation sizes in all six
positions would unnecessarily discard capacity on that last channel.

## Cover the signaling clock

The current digital training profile produces 6,144 octets: every possible
octet appears exactly once at each position of a 24-sample cycle. Thus each
octet appears four times at each six-symbol frame position. At the nominal
clock, one traversal takes 768 ms.

This coverage matters when the channel's transformation varies over four
successive frames. For a stationary deterministic transformation at each of
the 24 positions, the traversal observes every input/output pair that can
occur later. A pad between input and output robbed-bit stages can make their
combined transformation differ from either stage alone. The native probe
test checks complete training coverage in both laws, then checks every
selected magnitude, both signs, and all four signaling positions, in
addition to random encoded payloads.

The probe deliberately merges observations into six receive sets; it does
not retain the 24-position history. Its repeated-observation counter saturates
at two. Neither that counter nor the receive sets establish coverage of a
longer or changing signaling cycle. Noise, gain drift, a clock slip, or a
changed signaling state can invalidate previously measured distinctions.

## Conditional models must preserve uncertainty and capacity

An exact scalar-pad model can intersect codec-cell preimages to retain every
gain compatible with training. Predictions must union the outputs of every
remaining gain and impairment hypothesis. A best-fit gain alone can exclude
reachable outputs, and a compatible model does not prove its channel-family
assumptions.

Overly broad hypotheses also have a cost. If robbed bits may appear at any
one of six positions, with an unconstrained state that need not appear
during training, the predicted union can include both low-bit values at
every position. Even perfect identity observations then fail to justify
64 kbit/s under that model. Selecting a preferred phase would conceal the
uncertainty rather than resolve it.

An alternative model can retain the 24-position clock and require fixed
identity/force-zero/force-one operations before and after a static pad at
each signaling position. That assumption is stronger and can be contradicted
by training. It must remain explicit; periodic coverage cannot justify an
arbitrary later state change. Such a model is experimental and is not used
by the production selector. Modeled possible outputs must stay separate
from measured outcomes and observation counts. Any integration also needs
bounded work, call-owned state, and invalidation when training restarts.
