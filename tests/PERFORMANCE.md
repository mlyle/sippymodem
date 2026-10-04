# Packet processing benchmark

Build with `make -C tests build/bench_packets`. This opt-in executable uses the
ordinary native modem media fixture and processes 160-sample G.711 packets
(20 ms at 8 kHz). It does not dial an external destination.

List profiles with `tests/build/bench_packets --list`, then run, for example:

```
taskset -c 2 tests/build/bench_packets v92-pcm48 2 0 0 1000 0
```

Arguments after the profile are:

1. Link: `0` raw, `1` LAPM without compression, `2` LAPM with V.44.
2. Digital originator: `0` for an analog caller and digital answerer, `1` for
   the opposite PCM roles. V.91 uses two digital endpoints.
3. G.711 law: `0` PCMU, `1` PCMA.
4. Number of data packets per endpoint.
5. Payload: `0` deterministic random bytes, `1` compressible text-like runs.

Use raw mode for Bell 103, V.21, V.23 and Baudot. Baudot transmits in one
direction because its channel is half duplex. Other profiles exercise both
directions. The benchmark verifies every received payload byte and rejects
changes in modulation or negotiated rates during the data interval. The
interval can end with data still queued; `verified_bytes` counts received
bytes, not all bytes accepted at the transmitting terminal.

Each JSON line describes one endpoint and one phase. Training includes
negotiation and data-link establishment. A one-second settling interval is
excluded before data measurement. The timed boundary is `modem_process_g711`,
including DSP, framing, error correction, compression and modem terminal
output. Media preparation, traffic generation and receiving-terminal reads
are outside that boundary. It therefore measures core packet processing;
measure SIP/RTP, scheduling and application costs separately with live calls.

On x86-64, fenced RDTSCP reads the invariant TSC, calibrated against
CLOCK_MONOTONIC_RAW. These are reference cycles, not dynamic CPU core cycles.
On AArch64, the generic timer gives elapsed ticks at CNTFRQ_EL0. Where Linux
permits a pinned user-mode CPU cycle event, `perf_event_open` supplies actual
CPU cycles as an additional measurement. The benchmark leaves host security
and frequency settings unchanged and reports when the PMU is unavailable.
Hardware timer durations include interrupts or descheduling inside a callback;
user-mode PMU counts exclude that time. Both include measurement overhead.

For each endpoint and phase:

- Average time is `sum_ticks / packets / counter_hz`.
- Worst packet time is `max_ticks / counter_hz`.
- Worst consecutive three-packet processing time is `max3_ticks / counter_hz`.
  This sums three measured callback durations; idle gaps are excluded.
- `max_index` and `max3_index` identify the observed worst positions.
- PMU fields provide the corresponding cycle totals and maxima when available.
- Bit rates, symbol rates and the worst packet's protocol step aid comparison.

Repeat profiles, codecs and roles on a quiet host. Aggregate averages by total
packets and take maxima across runs. Never join three-packet windows across
runs, endpoints or phases. The real-time budgets are 20 ms for one packet and
60 ms for three packets. An observed maximum is not a guaranteed upper bound.
Record compiler options, CPU affinity, temperature, frequency, throttling and
source revision with the results.

The V.90 digital-originator policy omits optional 3429-baud probing; that
benchmark permits its highest supported upstream rate of 31,200 bit/s.
