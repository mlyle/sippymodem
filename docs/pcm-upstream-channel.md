# V.92 upstream channel design

The digital receiver learns a channel during TRN2u and sends its upstream
mapping and filters in CPd. `v92_upstream_channel` owns the measurement;
`v92_cpd_channel` designs the filters, constellation, power normalization and
matching local receive plan. This is separate from the generated
[downstream constellations](pcm-constellations.md) and
[digital-to-digital channel learning](digital-channel-learning.md).

## Keep source and receive coordinates distinct

The analogue transmitter scales its TRN2u levels from its own LU. The digital
receiver reconstructs known targets using a measured receive level. Their
symbol classes agree, but their numerical amplitudes need not. A gain fitted
between those targets and raw codec cells therefore includes the coordinate
conversion; it is not automatically the physical gain for arbitrary CPd data
levels. Data uses the accepted CPd filters and 4G together with the analogue
transmitter's retained LU, including signed integer rounding.

There is also physical transmit memory after the training or data coder.
The analogue owner retains Su's fractional-delay filter and sample history
through TRN2u, control records, B1u and payload. At zero fractional phase it
is an exact fifteen-sample delay. At nonzero phase, neighboring symbols can
move one nominal symbol across codec-cell boundaries. A small timing shift
is not permission to treat that path as memoryless. Resetting this filter
at a protocol handoff would change the trained channel.

For the quantized transmit filter, before saturation,

```
y[n] = round(sum(c[k] * u[n - k], k = 0..30) / 32768)
```

Here `u` is the actual scaled coder output and `c` contains Q15 coefficients.
If every input in that history satisfies `abs(u) <= A`, an interval around
`c[15] * u[n - 15] / 32768` has radius

```
A * sum(abs(c[k]), k != 15) / 32768 + 0.5
```

The 0.5 bounds final rounding in amplitude units. Apply saturation to both
interval endpoints when clipping is possible. The amplitude bound must cover
all legal payload histories, including CPd prefilter activity; a training
peak alone does not establish it. This box bound is conservative and can be
much wider than the residual on a fixed training sequence.

A raw codec-cell model must account for this memory and the training-to-data
coordinate conversion before certifying CPd candidates. Distinct abstract
levels are insufficient. A memoryless symbol certificate needs disjoint
observed codec sets under every retained channel explanation and allowed
history. A sequence decoder may distinguish overlapping symbol sets, but
that needs a joint coding/channel argument rather than this scalar certificate.
Robbed-bit phase belongs to the original physical sample clock, including
missing samples. Re-encoding equalized samples or compressing that clock
changes the evidence rather than resolving impairment uncertainty.

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

The packed matrix product handles the first row separately and advances
the remaining rows in pairs. This lets the CPU work on independent sums
and reuses loaded input and output entries. Each result still receives the
ridge term and matrix columns in the original order. The separate first
row also preserves its native fused arithmetic on ARM.

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

## Qualify a second observation of short V.92 control records

The short rate exchange has a known TRN2u waveform before the analogue
modem sends status. A separate observer can measure residual gain, DC, and
one-symbol memory after the receive equalizer. The ordinary control parser
continues to own the timed exchange. The observer can supply another complete
status record when the established slicer misses it.

For each candidate sample alignment, fit

```
y[n] = g * x[n] + h * x[n - 1] + b
```

Here `x` is the known signed 4-PAM or 8-PAM training reference, normalized
to unit mean square over the alphabet, and `y` is the equalized observation
in units of the acquired training level. Fit both reference polarities.
A constant sign reversal leaves differential decisions unchanged after the
first symbol and its descrambler history. Use the signed fit to predict
received training. The protocol state still owns differential initialization.
[ITU-T V.92 Amendment 1, §8.7.6](https://www.itu.int/rec/dologin_pub.asp?id=T-REC-V.92-200107-I%21Amd1%21PDF-E&lang=e&type=items)

Use disjoint training windows to fit and validate the model. The prototype
searches offsets from −6 through +6 samples. It admits direct gain magnitudes
of 0.50–1.55, postcursor magnitudes up to 0.35 times the direct gain, and DC
magnitudes up to 0.40 times the direct gain. Apply the bounds before ranking
fits. Otherwise a one-sample displacement can move the direct response into
the postcursor and nearly tie the physical alignment. The retained fit must
predict the held-out window within 0.22 times the direct gain in RMS error,
and the next admissible alignment must have at least twice that error.
Qualification compares samples against known training before reading status.

First try gain and offset correction. For a channel with measurable memory,
use one state per possible preceding PAM level. For each new sample, compare
every predecessor `p` and current level `s`. Extend the predecessor's best
path by `(y[n] - g*s - h*p - b)^2`. The best predecessor per `s` suffices
because older symbols cannot affect the next observation in this model.
The direct gain and postcursor used here have the same physical polarity as
the measured signal. Subtract the minimum path cost before the next iteration.
More distant echoes or rapidly moving timing require a different model.

The bounded experiment used up to 160 new symbols and 96 symbols of overlap.
The overlap covers a complete pair of 36-symbol 4-PAM records and the
scrambler and differential history needed before them. Accept two consecutive
records on the calibrated 12-symbol frame phase only when a single traceback
produces both with the same payload, correct CRCs, and all required zero
fill. A single CRC-valid prefix can survive a corrupted or interrupted
record. Its CRC excludes the fill. A shared disturbance can also correlate
the CRC outcomes of successive records.
[ITU-T V.92, Table 27](https://www.itu.int/rec/dologin_pub.asp?id=T-REC-V.92-200011-I%21%21PDF-E&lang=e&type=items)

### Qualification results

In the test below, the nominal path needs one complete record and the
alternative needs a matching consecutive pair. The columns are cumulative.
An alternate recovery counts only if it succeeds at all five tested packet
alignments. The measured-waveform tests apply additional gain, DC, and
one-symbol residual echo to six equalized captures. They model the waveform
at the slicer input, independently of adaptation in the live receive filter.

| Test stream | Nominal slicer | With gain and DC | With one-symbol sequence detection |
| --- | ---: | ---: | ---: |
| Generated 4-PAM | 45/216 | 108/216 | 150/216 |
| Generated 8-PAM | 2/216 | 28/216 | 73/216 |
| Measured 4-PAM waveforms with added impairments | 105/216 | 141/216 | 178/216 |

All 28 stable-offset trials recovered their intended alignment and status.
Changing the clock between the two training windows withheld the fit in all
24 nonzero-step trials. Neither alternative accepted a pair among 192
controls with random traffic after training, partial status, bad CRC,
silence, clipping, an unqualified phase step, or changing status payloads.
The fixed kernel matched 144 small exhaustive path comparisons.

A clean 4-PAM recording can have a valid status CRC followed by fill
whose high-confidence symbols disagree with the complete record. A field
failure exhibited this pattern with no second complete status. Its training
fit had the same alignment as good exchanges and neither alternate decoder
recovered a complete pair. Keep the ordinary parser's fill and CRC
requirements for every accepted record.

The eight-state, 256-symbol decoder averaged 0.012 ms on Ryzen and 0.038 ms
on Pi 5. The highest single-window times were 0.188 ms and 0.046 ms, and the
highest three-window work totals were 0.240 ms and 0.125 ms. These measurements
cover the isolated sequence detector. Production integration needs call-owned
shadow state, a packet budget that includes fitting and parsing, and live
bidirectional qualification before the additional observation may affect
negotiation.
