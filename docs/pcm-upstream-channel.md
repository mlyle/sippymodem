# V.92 upstream channel design

The digital receiver learns a channel during TRN2u and sends its upstream
mapping and filters in CPd. `v92_upstream_channel` owns the measurement;
`v92_cpd_channel` designs the filters, constellation, power normalization and
matching local receive plan. This is separate from the generated
[downstream constellations](pcm-constellations.md) and
[digital-to-digital channel learning](digital-channel-learning.md).

## Fit the channel inverse

Let `t[n]` be the known training target and `r[n]` the measured receive
sample. The general designer fits:

```
t[n - D] = offset
         + sum(a[i] * r[n - i], i = 0..84)
         + sum(b[j] * t[n - D - 1 - j], j = 0..89)
```

The decision delay `D` is 60 samples. Measurement retains 12,000 symbols,
excluding the first 512 from the fit. Targets are known training samples,
not decisions made by the equalizer. Exact first moments and finite-window
boundary corrections center every shifted column over its actual fitting
window. Moving a correlation window must not silently assume stationarity.

The offset is unpenalized. It is eliminated before solving and is not sent
as a CPd filter coefficient. B1u later acquires its own receive calibration;
the training fit does not authorize an arbitrary payload offset or gain.

For a linear channel `H`, the fitted filters approximately satisfy
`Z2 * H = z^-D * (1 - P1)`. The measured-input coefficients determine Z2;
the earlier-target coefficients determine P1. Short forward-channel fits
can produce simpler inverses. The general fit replaces a qualifying short
inverse only when it reduces residual energy by at least 10%.

The general solver uses a packed symmetric Gram matrix and bounded conjugate
gradients. Its coefficient penalty is:

```
lambda = 0.001 * (85 * observed_energy + 90 * expected_energy) / 175
```

Energies share the system's normalization. This is a numerical design policy,
not a V.92 wire limit or a measured noise floor. The solver permits at most
700 iterations and rejects an uninformative inverse whose residual exceeds
5% of target energy. These bounds do not replace later signal qualification.

An isotropic coefficient penalty encodes a prior on coefficient magnitudes.
Rescaling only one group of input columns changes that prior. A comparison
using different measurement units must transform the penalty too, or label
the coordinate change explicitly. A diagonal-variance penalty can remove
that dependence, but it is a different design policy, not an equivalent
implementation of the current solver.

## Quantize and measure the transmitted mapping

The designer scales Z2 so its largest coefficient has magnitude 24,576,
quantizes the wire filters, and retains the corresponding gain in the local
receive plan. P1 coefficients use the wire's 14 fractional bits. Trailing
zero coefficients are removed; leading zeros retain the chosen delay.

Rate selection balances the moduli and chooses the constellation geometry
before measuring power. It runs the actual encoder through the complete
48-frame, 576-symbol B1u, including the selected filters and representative
choices, then sets 4G from the resulting prefilter energy. A tone amplitude
or coefficient norm is not a substitute for this measurement.

For a general inverse, let `epsilon` be its relative residual energy, `E_u`
the sum of squared B1u constellation points, and `gap` the smallest signed
constellation spacing. The current rate policy requires:

```
gap^2 >= 36 * epsilon * E_u / 576
```

This estimates three standard deviations to either nearest decision
boundary. It assumes that the training residual predicts the relevant data
error; it is not a BER guarantee. A single positive feed-forward tap instead
permits explicit codec modeling and retains the highest enabled rate for
independent B1u qualification.

Reducing a fitting penalty can lower `epsilon` while increasing feedback
activity and `E_u`. Their product can therefore get worse. Optimize the
quantized mapping's error and power together, rather than choosing the
lowest training residual or largest nominal rate alone. B1u is a finite
known sequence; payload can exercise other filter histories and legal
representatives. Successful power normalization must be followed by payload
checks, including saturation and coding continuity.

## Verify a design change

First reproduce the current sufficient statistics, wire CPd and local plan
from the same observations. An independent raw design matrix can check the
streaming covariance calculation without relying on its history indexing.

Use chronological held-out training samples to compare alternatives. Freeze
both coefficients and intercept at the fitting boundary; later samples
must not choose either. Report the quantized filters' predictions separately
from the floating-point fit, and examine short windows as well as averages.
Keep each model's iteration count and work cap visible.

Compare payload at matched rates to isolate filter changes, then evaluate
the normal rate selector. Exercise both codec laws, known-channel fixtures,
missing media, and native interoperability. A decoded B1u must still pass its
independent checks. In particular, a better TRN2u fit does not explain a
wrong coding epoch or unconfirmed parameter handoff.

Recorded-call replay can establish exact local signal reproduction. It
cannot establish peer reception, infer BER from a residual alone, or prove
that a changed protocol would have elicited the same recorded reply. Keep
replay qualification, model prediction, and live bidirectional data results
as separate evidence.
