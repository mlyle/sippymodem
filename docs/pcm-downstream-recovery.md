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
