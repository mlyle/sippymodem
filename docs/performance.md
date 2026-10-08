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
