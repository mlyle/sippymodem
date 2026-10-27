# Rates and processing performance

The [README rate table](../README.md#modulations-and-modes) lists nominal
physical bitrates. Framing consumes some of those bits; compression can make
application throughput higher or lower depending on the data. Status reports
transmit and receive bitrates separately, along with their symbol clocks.
The remote-clock pacing ceiling can also make a software feedback connection
advance faster than wall time; it does not negotiate a higher line bitrate.
See [media clocking](media-clock.md).

## Measure the intended workload

The checked-in [packet benchmark](../tests/PERFORMANCE.md) trains real modem
cores, verifies received bytes, and measures each 160-sample / 20-ms callback.
It reports the average, worst packet, and worst sum of three consecutive
callbacks without their idle gaps. Data and training are separate intervals.
The core boundary includes DSP, framing, error control, compression and modem
output; it excludes SIP/RTP socket handling and the BBS application.

```sh
make -C tests build/bench_packets
./tests/build/bench_packets --list
./tests/build/bench_packets v92-pcm48 2 0 0 1000 0
```

Repeat on the intended host with its expected load and cooling. One packet
has a 20-ms processing budget, and three have 60 ms. An observed maximum is
not a worst-case guarantee. Record the revision, compiler options, CPU
frequency, temperature, throttling, packet size, roles, codec and traffic.

Each started native library line has its own DSP thread; one owner thread
handles SIP, sockets and public API calls. A shared server is bounded to
32 lines with the bundled SDK, but that is a configuration limit rather than
a CPU-capacity claim. Size the deployment from sustained calls, training
bursts, application load and host scheduling as well as the core benchmark.

## Recorded baseline: 2026-10-04

An earlier core revision, `d9e06e7b`, was measured on a Ryzen 7 5825U and a
four-core Raspberry Pi 5 with 8 GB RAM. Both used normal `-O2` builds.
The Pi had no active fan; heatsink presence was unknown. It reached a thermal
limit during the study. These observations predate later calibration, PCM
training and threading changes; they are a baseline, not measurements of the
current shared-server implementation.

Selected steady-data results below are milliseconds per 20-ms packet.
Average is the largest weighted mean among the tested role/codec/link/data
configurations; maxima are taken across those configurations. Three-packet
windows never cross endpoints, phases or runs.

| Profile | Desktop average | Pi average | Pi worst packet | Pi worst three |
| --- | ---: | ---: | ---: | ---: |
| V.21, 300 bit/s | 0.021 | 0.042 | 0.101 | 0.207 |
| V.32bis, 14,400 bit/s | 0.894 | 5.391 | 6.365 | 18.914 |
| V.34, 33,600 bit/s | 2.861 | 7.493 | 9.436 | 28.071 |
| V.90 | 1.783 | 4.258 | 5.285 | 15.581 |
| V.92 with 48-kbit/s PCM upstream | 2.925 | 6.371 | 8.019 | 22.949 |
| V.91 | 0.292 | 0.764 | 1.023 | 2.888 |

The README expresses selected means as estimated one-core load:
`mean_ms / 20 ms * 100`. For V.34 that is 14.3% on the Ryzen and 37.5% on the
Pi; for V.92/33.6k upload, 9.0% and 21.4%; for V.92/48k PCM upload, 14.6% and
31.9%. The V.92/33.6k profile means are 1.794 and 4.271 ms. These are the largest
per-endpoint weighted means across measured configurations, including both
PCM roles. The conversion estimates steady core processing demand; it is
not a sampled whole-host CPU measurement. Timer durations include interrupts
and descheduling, and exclude SIP/RTP and application work.

The earlier V.34 33.6-kbit/s training maximum was 47.327 ms, with 66.696 ms
for three callbacks. This exceeded both processing budgets even though an
individual call could survive through buffering. Steady-data averages cannot
establish training headroom.

A separate LAN stress run on that Pi completed three staggered waves of eight
digital V.92 lines with V.34 upload: 24 calls, 90-second connected holds,
56,000 bit/s Pi transmit and 33,600 bit/s receive, LAPM and V.44. It verified
50,348,452 received bytes across both directions. Peak whole-host CPU was
57.0%, and peak modem proportional RAM was 19.8 MiB. Eight heavy V.34 calls
and eight PCM-upstream calls reached approximately 100% CPU and did not
complete their campaigns. The Pi was thermally throttled.

Those stress runs used separate modem processes. They excluded a complete
BBS application, storage traffic and PSTN impairments, and were not endurance
tests. They do not establish an eight-line capacity guarantee for the current
shared server or predict the effect of adding cooling. Raw evidence remains
in the private development corpus; the reusable measurement procedure is
checked in under `tests/PERFORMANCE.md`.

## Follow-up Pi capacity: 2026-10-05

After bounded startup calibration and associated optimizations, the study
ending at `423b2765` completed three staggered waves of five heavy calls per
profile. Each wave held all five calls for 90 seconds, with LAPM/V.44 and
checked bidirectional payloads.

| Pi profile | Completed calls | Peak whole-host CPU | Peak modem PSS | Verified received bytes |
| --- | ---: | ---: | ---: | ---: |
| V.34, 33.6k both ways | 15 | 58.0% | 11.8 MiB | 21,247,303 |
| V.92, 56k transmit / 48k PCM receive | 15 | 56.0% | 14.2 MiB | 32,396,817 |

CPU uses the full four-core host as 100%, sampled once per second. PSS apportions
shared pages. Completed-hold snapshots had zero retrains and zero late/concealed
samples. The Pi had no active fan, reached 86.5–87.0 °C and reported thermal
throttling. The eight-V.34 campaign still ended with a second-wave disconnect
at 99% peak CPU; its exact caller-side datapump failure was not resolved.

These were separate modem processes on LAN SIP. They measured neither the
current shared-server library nor a complete BBS. Five heavy calls passed
these workloads; this is a planning result with those limits, rather than a
measured maximum capacity. Private calibration and wave records retain the
exact source/build hashes, counters, and failure logs.

## PCM receive training on Pi 5: 2026-10-26

Nonlinear channel fitting and paired B1 decoding previously exceeded the
20-ms packet budget on a channel with return echo and a 0.125 postcursor.
Receive acquisition now retains physical observations while fitting advances
across native media blocks. Both channel hypotheses and all independent B1
checks remain. After successful B1 validation, a sufficiently separated
channel releases the competing decoder; uncertain channels keep both.
The temporary observation queue is bounded to 1,280 samples and drains
accepted input after remote media retirement. It adds no steady-state prefill.

The following measurements capture the initial scheduling change with
normal `-O2` builds, one Pi core pinned for
DSP, LAPM, 52,000 bit/s transmit and 40,000 bit/s receive, and 20-ms packets.
There were two 500-data-packet calls per row, with run order reversed between
repeats. Runs started at or below 65 degrees C; telemetry showed no active
throttling. Four-tap rows force a tiny valid third CPd feedback coefficient
to exercise the more expensive scoring path; they are synthetic cost cases,
not evidence from another physical line.

All figures are milliseconds of thread CPU time. Three-packet windows exclude
idle gaps and never cross phases or calls.

| Receive case | Training worst packet | Training worst three | Data average | Data worst packet |
| --- | ---: | ---: | ---: | ---: |
| Mu-law, three taps | 16.87 | 48.18 | 9.48 | 10.39 |
| A-law, three taps | 17.37 | 50.75 | 9.69 | 10.68 |
| Mu-law, four taps | 17.34 | 49.61 | 10.96 | 12.16 |
| A-law, four taps | 17.67 | 51.49 | 11.46 | 13.48 |

Elapsed-timer maxima across these runs were 17.68 ms per training packet
and 51.52 ms per three packets, within the respective 20/60-ms budgets.
A separate reversed-order comparison against `ca7fc44d` measured three-tap
training maxima of 36.63/39.83 ms for mu-law/A-law before these changes, and
steady-data averages of 18.28/18.67 ms.

A fresh build of all 356 native core modules also compared 4,902,720 decoded
payload bits against independently recorded transmitter output, with zero
bit errors or ambiguous frames. That check used a 0.375 postcursor for
mu-law and 0.125 for A-law, with return echo. Receiver tests cover bounded
input, sub-frame scheduling, restart, four-tap fitting and retained-media
drain. These measurements establish headroom for the tested core paths;
they exclude SIP/RTP, BBS load and concurrent-call scheduling. The source,
build procedure, logs and thermal telemetry remain in the private evidence
corpus.

The stronger 0.375-postcursor A-law case exposed a remaining burst in that
initial schedule: 22.29 ms for one training packet and 65.81 ms for three.
The receive budget now also accounts for dense codec cells and carries
fractional scoring credit across every block size. Paired B1 scoring charges
18 work samples per three-tap frame or 24 per four-tap frame. A bounded
1,280-sample queue covers the fitting and independent-validation debt,
including native block boundaries. After validation, one retained sample
may catch up per four new inputs, limiting decoder acceleration to 25%.
These changes preserve the fitted models, B1 checks and recovery thresholds.

The final schedule (`46965cb2`) was measured again with the same packet size,
build options, affinity and cooling rules. Each row below contains two calls
with 500 data packets each; paired cases ran in reverse order on the second
pass. Values are thread CPU milliseconds, with maxima taken across both calls.

| Receive case | Training worst packet | Training worst three | Data average | Data worst packet |
| --- | ---: | ---: | ---: | ---: |
| Mu-law, 0.125 postcursor, three taps | 13.16 | 36.77 | 9.47 | 10.38 |
| A-law, 0.125 postcursor, three taps | 13.19 | 39.16 | 9.70 | 10.72 |
| Mu-law, 0.125 postcursor, four taps | 13.41 | 39.83 | 10.97 | 12.12 |
| A-law, 0.125 postcursor, four taps | 15.58 | 42.80 | 11.46 | 12.93 |
| A-law, 0.375 postcursor, three taps | 18.62 | 51.93 | — | — |
| A-law, 0.375 postcursor, four taps | 17.26 | 48.87 | 13.58 | 17.19 |

Every final measured training maximum, including elapsed-timer measurements,
fit the 20-ms packet and 60-ms three-packet budgets. The stronger four-tap
case also shows why moving work needs a data-phase check: catch-up increases
its data peak even as training bursts shrink. Normal-channel startup takes
roughly 100–140 ms more media time; steady-state input has no added prefill.
These sampled results do not guarantee deadlines on every channel or under
concurrent host load.

A separate rebuild of the final source checked 4,911,480 decoded payload bits
against recorded transmitter words, with zero errors or ambiguous frames.
Targeted whole-core AddressSanitizer/UndefinedBehaviorSanitizer receiver and
retired-media checks passed. Receiver Memcheck was clean; broader full-call
Memcheck still reports an existing INFO1 structure-padding comparison issue,
which also reproduces before the scheduling changes.

A later 56k/48k A-law four-tap probe retained both channel hypotheses into
payload and exposed a 20.43-ms callback. The observed-frame hot path now
accepts exclusively owned, valid decoder state and a validated model frozen
for the coding epoch, avoiding repeated full metadata scans every 12 symbols.
The checked entry point retains those scans. An 8-KiB frame-local cache also
reuses exact coordinate-to-geometry lookups across symbols; it stores no
observation-dependent scores and resets when the model is rebound. These
changes retain both hypotheses, independent B1u validation, per-frame input
checks, transactional prediction failures and all recovery thresholds.
