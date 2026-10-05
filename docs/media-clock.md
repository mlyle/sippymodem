# RTP receive clock and playout

The default receive clock processes contiguous RTP audio when it arrives.
There is no fixed playout prefill. This keeps modem turnaround short on an
ordered digital path while retaining bounded storage for delayed processing.

RTP can arrive out of order or be lost. Timestamps describe the media timeline;
packet arrival time is a different clock. These are the roles defined in
[RFC 3550, sections 1 and 5.1](https://www.rfc-editor.org/rfc/rfc3550.html#section-5.1).
A modem benefits from recovering a reordered packet because concealment
introduces missing waveform samples. Waiting on every packet would instead
delay every training response, including calls with no reordering.

## Jitter policy

- `SIP_REMOTE_CLOCK=1`: `SIP_JB_MS`, default 60 ms, is the maximum wait for a
  missing interval before already buffered future audio. Contiguous audio
  plays immediately. One continuous hole receives one deadline, even if it
  spans several frames. After expiry, missing samples are concealed and their
  received mask stays false.
- `SIP_JB_MS=0` removes that recovery wait. Reordering in one socket drain is
  still recovered. A predecessor arriving in a later drain may be too late.
- An incomplete frame retains its fragments until completion. If the peer
  stops sending and waits for a response, a deadline of at least one nominal
  frame, or the larger jitter setting, completes the clock interval once.
  Only the missing tail is concealed; a late tail cannot supply another tick.
- `SIP_REMOTE_CLOCK=0`: the same jitter setting remains fixed prefill before
  wall-clock playout. The scheduler then advances at the configured rate.

The separate two-second timestamp storage bounds a processing backlog. It is
not two seconds of playout delay. A new SSRC retires queued clock credit and
wait deadlines. A new call also resets its rate allowance.

Each direction needs its own receive storage. The peer's jitter buffer protects
its receiver from jitter in our transmissions; it cannot repair a missing or
reordered packet on our receive path. Our bounded slots also support ordering,
duplicate rejection and batches delivered together. None of these requires a
fixed prefill when the next remote-clocked samples are already contiguous.

## Receive-driven pacing

Accepted RTP sample time supplies transmit and DSP credit, including lost
intervals. A seed frame starts media before the first inbound RTP; an already
received stream supplies its own startup credit.

The ceiling is exactly four thirds of the configured sample rate. A token
interval is three quarters of a nominal frame: 20 ms becomes 15 ms; 10 ms
becomes 7.5 ms. At most four frames can run immediately, including across
repeated event-loop passes. Idle time restores only that burst. Pending RTP
credit remains queued; the event loop waits for the later of the recovery and
pacing deadlines and still polls signaling. Backward or saturated timestamps
cannot create unlimited credit. Each pass also processes at most four frames.

This ceiling is an application policy, not a new negotiated RTP sample rate.
The RTP timestamp still advances by the number of payload samples; the status
bit rate describes the nominal modem sample timeline. Normal 8 kHz peers
supply normal 8 kHz progress. A software feedback path can advance faster in
wall time, bounded by this ceiling. RFC 3550 does not prescribe a 60 ms buffer
or this four-thirds ceiling.

An independent sender faster than the ceiling cannot be slowed by our scheduler.
Its future timestamps accumulate in the bounded receive window. Samples beyond
that window are rejected and counted in the receiver's `early_samples` statistic;
continued excess input therefore loses packets. A software feedback peer follows
the rate of our replies, while an independent peer must keep its own clock within
the sustainable receive rate.

Five seconds without accepted complete RTP clock progress ends the call.
