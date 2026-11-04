# RTP receive clock and playout

The default processes modem frames on a paced clock with a 60 ms reorder reserve.
Accepted RTP source timestamps discipline its average frequency. Packet arrival
jitter therefore does not directly become a pause or burst in the outgoing
carrier. RTP timestamps still advance by the actual payload sample count;
this changes scheduling, not the negotiated 8 kHz sample rate or modem bit rate.

Timestamps describe the media timeline, while arrival time is a separate clock
([RFC 3550, sections 1 and 5.1](https://www.rfc-editor.org/rfc/rfc3550.html#section-5.1)).
A receive reserve allows a late predecessor to arrive before its samples are
needed. A lost packet is concealed at its playout deadline and retains a false
received mask. Neither ordering nor missing receive media stops transmission.
The remote receiver's jitter buffer protects its own direction; it cannot
recover a missing packet on our receive path.

## Settings

- `SIP_REMOTE_CLOCK=1`, `SIP_JB_MS>0`: paced source-clock tracking with fixed
  receive prefill. Add one configured local frame to the requested reorder
  reserve: each DSP invocation consumes a whole frame at once, including
  its tail. The default total is 70 ms for the executable's 10 ms frames,
  or 80 ms for the library's 20 ms frames. Each direction configured this
  way incurs that receive delay. This costs turnaround latency in exchange
  for stable carrier timing through network jitter and reordering.
- `SIP_REMOTE_CLOCK=1`, `SIP_JB_MS=0`: direct receive-driven operation without
  fixed prefill. Contiguous received sample time releases DSP/transmit credit.
  Reordering within one socket drain is recovered, but a predecessor arriving
  later may already be lost. An incomplete frame waits at most one nominal
  frame before its missing tail is concealed. This mode retains the existing
  low-latency software/digital behavior.
- `SIP_REMOTE_CLOCK=0`: independent nominal local clock with the configured
  fixed prefill. It does not follow the peer's oscillator; sustained clock
  mismatch can eventually exhaust its reserve or receive window.

All modes retain bounded timestamp storage and received-sample provenance.
The remote modes retain a separate two-second source timestamp window for
processing backlogs. That storage is not two seconds of prefill.

## Clock discipline and work bounds

Paced operation observes source-clock lead relative to playout, including
known holes but excluding rejected future timestamps. Only newly accepted
source time updates the frequency loop. Duplicate or reordered old payload,
concealment and malformed packets do not create oscillator observations.
Missing input holds the last frequency; it cannot slowly freeze transmission.

A four-observation average reduces packetization noise. The loop accounts
for both local and received block sizes; packet batching is not oscillator
drift. Slow proportional/integral correction follows oscillator drift; bounded faster correction handles larger
source-rate differences. Clock pull is limited to 3/4..4/3 of nominal. A source
outside the sustainable range can underrun the receive reserve or exceed the
future window; discarded future samples remain visible as `early_samples`.

Each owner pass processes at most four frames. A shared token-bucket primitive
also limits repeated same-time passes: at most four immediate frames, followed
by one token per 3/4 nominal frame. Thus a 10 ms frame has a 7.5 ms ceiling interval,
and a 20 ms frame has 15 ms. Idle time cannot accumulate a larger burst. A host
scheduling backlog keeps its outstanding deadline and drains under that ceiling;
it is not also mistaken for oscillator drift. No waveform samples are inserted,
dropped or resampled to correct clock frequency.

A new SSRC resets frequency acquisition and receive state, while preserving
pacing credit. A new media lifecycle resets both. Backward owner timestamps
cannot renew credit. Deadline overflow ends media processing rather than
spinning or wrapping time.

Before the first source stream, outgoing media runs at the nominal rate.
This lets symmetric RTP gateways qualify the return route from several
sequential packets. Five seconds without accepted complete RTP clock progress
ends an established call or startup. Startup sends, malformed input and
rejected future packets do not refresh that watchdog.

## Regression coverage

Native tests drive the actual timestamp receiver and paced clock through
one-hour streams with positive/negative 50, 100, 500, 1000 ppm offsets, delayed
and reordered packets, clock-rate steps, timestamp wrap and wide pull through
4/3 nominal. Distinct sample bytes and received masks verify ordering and
provenance. Separate cases check one unrecoverable packet and a 100 ms owner
pause. Further cases use different local and remote packet sizes, from 1 ms
local frames to the largest supported executable frame. Host tests cover a receive hole without a
transmit hold, watchdog expiry and independent media lifecycles.

The explicit zero-buffer feedback test still advances 84 twenty-millisecond
frames in 1.2 seconds: four burst frames plus 80 paced frames. Raw digital framing
and throughput are described under [raw digital calls](ds0.md).
