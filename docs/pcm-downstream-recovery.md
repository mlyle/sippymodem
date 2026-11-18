# Qualifying a V.92 downstream constellation change

An analogue receiver can assess its preceding downstream map during an
already requested rate exchange. Keep that map when new training gives
adequate decisions. This preserves the throughput of clean 56 kbps
connections and keeps the eligibility test separate from the policy that
requests a rate exchange.

## Measure degradation in the preceding map

Regenerate the known TRN2d symbols from the negotiated training map. For
each expected symbol that also belongs to the preceding data constellation,
measure whether the preceding slicer returns its signed label. Bind the
label to the received symbol's six-phase index. A change within its
decision cell preserves the label. Use the slicer's ordinary linear PCM
midpoints and tie rule, including its treatment of signed zero.

The paired-call prototype uses symbols 48–719 and 924–1751 as two
disjoint windows. It requires at least 128 eligible symbols per window
and at least four wrong labels in the same phase in each window. Requiring
the evidence to recur in a phase lets a change confined to one phase
qualify. These counts are local experimental settings. They measure errors
on known symbols and supply no estimate of payload BER.

The first window can start fitting a replacement. The second must
independently qualify the degradation before the candidate CPu can be
published. Use the decision coordinates of the active receiver. The
modeled fixtures preserve the observed octet through conditioning, so
channels with an active downstream equalizer still need a paired check.

## Set the calibration lifetime by physical evidence

Retain a calibration only in the physical coordinates in which its
qualified DIL and B1d evidence was obtained. These include the receive
law, the channel epoch, and the six-phase clock origin. A short exchange
can preserve those coordinates across a data-map change. Full retraining
retires the old DIL and equalizer in the current call state machine. A new
call, unknown clock discontinuity, or change in observation coordinates
also requires fresh qualification before phase-specific evidence can be
reused.

For each phase, bound the gains and DC that predict the observed codec
intervals from the current known symbol and its predecessor. Use the
first window to fit, with the retained bounds as constraints. If that
intersection is certified infeasible, provisionally fit the first
window without the old constraints. An exhausted solver or missing
observations withhold selection for that exchange.

A provisional fit can supply a new map only when the second window
independently contradicts the old constraints in the affected phase and
the combined training windows admit a common fresh model. A gain step,
timing slip, or dropout between the windows can veto the map by breaking
that common fit. Leave the prior calibration unavailable for reuse in
phases with certified persistent contradiction. Any durable replacement
needs fresh known-symbol qualification, including B1d.

Map selection still respects the peer's rate mask and the preceding
transmit-power ceiling. An admissible candidate is sent through the
normal CPu exchange. The peer must accept it and the receiver must pass
the ordinary B1d check before returning to data.

## Paired-call evidence

In 192 native paired rate exchanges across A-law, μ-law, and both
polarities, keeping the preceding map throughout passed 64 cases. The
degradation gate preserved that map in those same 64 cases at 56 kbps.
It selected new maps for the other 128 cases, which passed at 44–46.667
kbps downstream and held 40 kbps upstream. Each pass checked at least
1,500 payload frames in each direction.

A gain change confined to one phase also qualified for a fresh fit in a
paired exchange, with bidirectional payload passing after the new map.
The experiments inject channel models into native paired stations.
They qualify the candidate trigger and publication rule for those
channels. Production use needs analogue-side field evidence, a call-owned
calibration lifecycle, and whole-packet timing through the fitting and
selection work.

## Active downstream equalization

Use the conditioned octet to score the old map and the original G.711
octet to constrain physical gain, postcursor, and DC. For a replacement
map, project the raw-code interval through the frozen receive filter,
rounding and requantizing as the receiver does. A shorter interval in
the raw plane can still cross a decision boundary after conditioning.

Four native pairs trained an active analogue equalizer, with μ-law at
three line gains and A-law at one. They each verified initial data at
56 kbps down and 40 kbps up. On an unchanged short exchange the raw
trigger lowered all four downstream rates to 30.667–42.667 kbps. The
conditioned trigger kept all four at 56 kbps. Projecting candidate
levels through the active filter recovered 16/16 persistent global or
single-phase gain changes, at 33.333–45.333 kbps down with 40 kbps up.
Keeping the old map passed 0/16.

Fresh fits can matter even when the retained and current constraints
intersect. In three small-gain trials an anchored map reached B1d at
49.333 kbps but failed it. Bounding candidate levels with the fresh
training fit reduced those selections to 40–42.667 kbps and passed all
three. The revised procedure passed 7/8 small-gain trials. One μ-law
case produced zero wrong old-map labels in both TRN2d windows and 617
wrong bits during B1d at 56 kbps. Training support for the levels used
later remains a separate admission requirement. All four transient
controls retained 56 kbps. Late gain and one-sample timing changes
withheld replacement.

## Verify the next rate exchange

Rd uses the highest level of each phase in the newly selected map.
A map with adequate midpoint spacing for data can leave that marker
outside the exact codec cell used for acquisition. In 14 of 16 modeled
calls that recovered from persistent drift, the unmodified Rd detector
missed the marker at the following short exchange on the same call.

The paired prototype retires only the phases with independently
contradictory raw evidence. It qualifies replacement bounds from new
B1d and verifies payload before reusing them. On a later locally
requested exchange, the fresh bounds and owned phase can translate a
compatible raw marker sample into the corresponding mapped label.
The ordinary periodic run and reversal checks still govern acceptance.
That path passed the second exchange in all 16 persistent-drift cases
at the map selected in the first exchange. The four unchanged controls
kept 56 kbps through both exchanges.

The active filters in those trials were effectively scalar. The
paired material-memory trials below use the same station and clock
ownership. Physical-link qualification still requires evidence from
an analogue receiver on a live connection.

## Bound the shared predecessor through the receive filter

When the channel and equalizer both have memory, the current raw
input and the preceding raw input depend on the same transmitted
predecessor. Giving each FIR history input an independent full-scale
range loses that dependence and can eliminate every permissible rate.
Partition the allowed predecessor range. In each partition, bound the
current input and the first history input using the same predecessor
interval. Apply the codec to those two ranges before weighting them by
the frozen FIR taps. Bound the oldest tap by its own phase and peak.
Take the union of the per-partition sums and apply the receiver's
final rounding and quantization. This retains the dominant cancellation
without requiring a search over full symbol histories.

In both laws the prototype used a material three-tap equalizer trained
by the ordinary startup, at 34.667–40 kbps downstream and 40 kbps PCM
upstream. Across 20 persistent gain or first-postcursor changes, the
full-scale history bound produced zero recoveries. The 32-partition
bound recovered 11 at 28–37.333 kbps downstream. Each recovery passed
B1d and at least 1,500 payload frames in both directions. Independent
checks compared 336,000 phase-specific test samples against the native
codec and FIR, including near clipping. The partitioned intervals
contained every output under the fitted channel assumptions.

## Predict B1d errors beyond the TRN2d labels

An exchange with zero wrong training labels can still provide evidence
for a change. Regenerate the known B1d symbols for the old map and
project their exact predecessor tuples using each window's own fit.
Require both windows to put the same known symbols outside their
old decision cells. The prototype requires such errors in at least two
of the ninth through forty-fourth B1d frames. The native admission
check requires forty trailing clean frames out of forty-eight. Both
windows must also contradict the retained calibration in the affected
phase, and their observations must admit a common physical model.

This trigger recovered five additional pairs for a total of 16/20.
Four of the five had zero wrong labels in both TRN2d windows. When the
same pairs kept their prior maps, all 58 independently predicted
wrong labels appeared during B1d. The six stable pairs, including two
at 56 kbps, kept their rates. Four transient changes kept their old
maps. Eight late channel steps or timing slips withheld publication.
Four persistent cases still failed to recover.

The material-memory comparison used the calibrated Rd path for each
explicitly requested exchange while its phase bounds remained valid.
It also recovered an A-law startup map that missed its first requested
Rd reversal with the ordinary detector. Three initial and four
recovered pairs passed a second exchange at the same rates, and nine
malformed marker controls failed before TRN2d. The prototype keeps
native B1d and payload checks. Live enablement still needs calibration
ownership and physical analogue-side validation.
