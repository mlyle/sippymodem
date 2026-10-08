# sippymodem

sippymodem is a software dial-up modem that makes and answers calls over SIP
and RTP. Connect to a remote modem, put a BBS on a phone number, or embed
several modem lines in a terminal server. It runs in user space on Linux,
without local modem hardware or a proprietary DSP binary.

It is derived from **Smart Link’s `slmodemd`**, whose
[original source and documentation](https://github.com/leggewie/pkg-sl-modem)
remain available. The Smart Link copyright notices and [BSD license](src/COPYING)
are preserved.

## Modulations and modes

The default negotiates V.92/V.90 or a supported older modulation, including
peers without V.8. It offers LAPM error control and V.44/V.42bis compression.
PCM roles are automatic; V.92 PCM upstream is opt-in.

| Mode | Nominal line rates / upper limits |
| --- | --- |
| Bell 103 / V.21 | 300 bit/s |
| Bell 212A / V.22 / V.22bis | 1,200 / 1,200 / 2,400 bit/s |
| V.23 | 1,200 bit/s one way, 75 bit/s the other |
| V.32 / V.32bis / V.34 | 9,600 / 14,400 / 33,600 bit/s each way |
| V.90 / V.92 with V.34 upload | 56,000 bit/s PCM downstream, 33,600 upstream |
| V.92 with PCM upstream enabled | 56,000 downstream, 48,000 upstream |
| V.91 between compatible digital peers | Up to 64,000 bit/s each way |
| Raw DS0 / DOVBS, PPP or LAPM framing | 56,000 or 64,000 bit/s each way |
| Baudot TTY/TDD, explicitly selected | 45.45 or 50 baud, half duplex |

Rates precede framing overhead and compression. PCM needs a suitable digital
bearer; DS0 needs its selected 56/64-kbit/s payload preserved. V.91 has software
peer coverage; external interoperability has not been established.
See [digital DS0 calls](docs/ds0.md) for bearer and framing requirements.

## Interfaces and AT highlights

- **`slmodemd`** runs one modem in the foreground and exposes a pseudoterminal
  with Hayes AT commands for terminal programs and BBS software.
- **`libsippymodem`** provides a C API with literal byte I/O, explicit call
  control, per-line logs, and SIP endpoints/huntgroups. Each started line has
  its own DSP thread; no PTY is required.

The [AT command reference](docs/at-commands.md#quick-reference) covers commands
and defaults. Highlights:

| Command | Purpose |
| --- | --- |
| `ATDT`, `ATDT<number>`, `ATDTsip:user@host` | Dial the default target, a phone number, or a SIP URI |
| `ATA` / `ATS0=1` | Answer once / enable automatic answering |
| `AT+MS?` / `AT+MS=?` | Query the selected mode / available modulations |
| `AT+V92U=1` | Offer V.92 PCM upstream on subsequent calls |
| `ATI11` | Inspect the current or last call, including disconnect details |

## Build and install

You need a C compiler, GNU Make, Python 3, `pkg-config`, and PJSIP. To build
with the pinned PJSIP source:

```sh
make pjproject-init
make -j2
```

This builds `src/slmodemd`, `src/libsippymodem.so`, and `src/libsippymodem.a`.
`make libsippymodem` builds only the library; `make test` runs the checks.
`sudo make install` installs under `/usr/local`. See
[building and installation](docs/building.md) for other prefixes and SDK linking.

## Launch and dial with Kermit

In one terminal, replace the example SIP URI with your peer and start the modem:

```sh
SIP_REGISTER=0 SIP_TARGET=sip:modem@peer.example.invalid ./src/slmodemd
```

After installation, use `slmodemd` in place of `./src/slmodemd`.
For telephone calls or registered incoming service, first configure your SIP
provider account as described in the [running guide](docs/running.md).

The modem prints its PTY path, for example `/dev/pts/7`, and stays running.
In another terminal, start [C-Kermit](https://www.kermitproject.org/ckermit.html):

```sh
kermit -Y
```

At its prompt, substitute the printed PTY path:

```text
set carrier-watch off
set flow-control none
set line /dev/pts/7
set speed 115200
connect
```

Type `AT` and press Enter to get `OK`, then `ATDT` to call the configured peer.
With a provider account, `ATDT<number>` dials a telephone number. Wait for
`CONNECT` before typing data. The PTY speed is independent of the line bitrate.

For dial-in, send `ATS0=1`, or wait for `RING` and send `ATA`. A BBS can do this
initialization itself. `Ctrl-\` followed by `C` returns to Kermit’s prompt;
`exit` quits Kermit. Stop the modem with Ctrl-C in its own terminal.
The [running guide](docs/running.md) also covers the split-pane console,
traces, and unattended operation with the [systemd examples](examples/systemd/README.md).

## Embed modem lines

Create a server and its lines, then service `sippymodem_process()` on one
owner thread. Dial explicitly or answer incoming-call events. Byte queues
report partial counts; AT text and `+++` are literal payload. Multiple SIP
accounts can share line pools. A native fd adapter supports `pppd`.
See the [library API](docs/library-api.md) and
[terminal-server integration guide](docs/terminal-server-integration.md).

## Measured performance

A 2026-10-04 `-O2` baseline on identical core source measured the following
steady-data processing cost per modem endpoint. Percentages are **estimated
one-core load**: average callback time divided by its 20-ms packet budget.
They include scheduling delays and exclude SIP/RTP and the BBS application.

| Negotiated line rates | Ryzen 7 5825U | Raspberry Pi 5 |
| --- | ---: | ---: |
| V.34, 33.6k / 33.6k | 14.3% (2.861 ms) | 37.5% (7.493 ms) |
| V.92, 56k / 33.6k | 9.0% (1.794 ms) | 21.4% (4.271 ms) |
| V.92, 56k / 48k PCM | 14.6% (2.925 ms) | 31.9% (6.371 ms) |

A later Pi run passed three waves of **five 56k/48k PCM calls** at **56% peak
whole-host CPU**. An eight-line 56k/33.6k run passed at 57%. The Pi had no active
fan and encountered thermal throttling. These runs predate the shared-server
library and exclude a complete BBS. See the [performance guide](docs/performance.md)
for revisions, methods, training spikes, and capacity limits.

## Development

Most of the new code is **vibe-coded (AI-assisted)**. I have paid special
attention to core DSP features, correctness, and performance, using focused
tests, interoperability checks, memory checks, and packet timing measurements.

Run `make install-hooks` to enable commit checks. See [testing](docs/testing.md)
and [memory checks](docs/memory-check.md). Live calls require private
configuration. PJSIP has its own licensing terms in the pinned submodule;
historical distribution documents are in [docs/legacy](docs/legacy).
