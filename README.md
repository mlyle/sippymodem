# sippymodem

sippymodem is a software dial-up modem that makes and answers calls over SIP
and RTP. Use it to connect a terminal program to a modem, put a BBS on a phone
number, or build a terminal server with several modem lines. It runs in user
space on Linux and builds from source, without a proprietary DSP binary or a
hardware modem.

There are two interfaces:

- **`slmodemd`** provides a pseudoterminal with Hayes AT commands for existing
  terminal programs and BBS software. It runs one modem in the foreground.
- **`libsippymodem`** provides a C API with literal byte I/O, explicit call
  control, per-line diagnostics, and SIP endpoints and huntgroups. Each started
  line has its own DSP thread. The library needs no PTY.

## Build

You need a C compiler, GNU Make, Python 3, `pkg-config`, and PJSIP. To use the
pinned PJSIP source:

```sh
make pjproject-init
make -j2
```

This produces `src/slmodemd`, `src/libsippymodem.so`, and
`src/libsippymodem.a`. `make libsippymodem` builds only the library.
`make test` runs the checks; `make install` installs under `/usr/local`.
See [building and installation](docs/building.md) for system PJSIP, staging,
SDK linking, and build options.

## Make or answer a call

Configure your SIP account or direct SIP peer, then run `./src/slmodemd`.
It prints its PTY path to stdout. Open that device with your terminal program
and send AT commands:

```text
ATDT<number>
ATDTsip:modem@peer.example.invalid
```

An empty `ATDT` calls the configured default target. For incoming calls, use
`ATS0=1` to answer automatically, or wait for `RING` and send `ATA`.
The default negotiates the best available supported mode, including older
modems without V.8. It offers LAPM error control and V.44/V.42bis compression;
V.92 PCM upstream requires `AT+V92U=1`. `ATI11` reports the current or last call.

The [running guide](docs/running.md) covers SIP configuration, the interactive
console, traces, and dial-in setup. The [AT reference](docs/at-commands.md)
lists commands and defaults.

## Embed modem lines

Include `sippymodem.h`, create a server and its lines, then service
`sippymodem_process()` on one owner thread. Dial out explicitly or answer
incoming-call events. Byte queues copy exact partial counts; AT text and `+++`
remain ordinary data. Multiple SIP accounts can share a pool of lines, and the
native fd adapter can connect a line to `pppd` without relaying bytes in Python.

Start with the [library API](docs/library-api.md) and
[terminal-server integration guide](docs/terminal-server-integration.md).

## Rates and performance

These are nominal line rates, before framing overhead and compression. The
peer, digital/analog topology, and channel quality determine the negotiated
rates in each direction.

| Mode | Supported rate or upper limit |
| --- | --- |
| Bell 103, V.21 | 300 bit/s |
| Bell 212A, V.22 | 1,200 bit/s |
| V.22bis | 2,400 bit/s |
| V.23 | 1,200 bit/s one way, 75 bit/s the other |
| V.32 / V.32bis | 9,600 / 14,400 bit/s |
| V.34 | 33,600 bit/s each way |
| V.90 / V.92 with V.34 upload | 56,000 bit/s PCM downstream, 33,600 upstream |
| V.92 with PCM upstream enabled | 56,000 downstream, 48,000 upstream |
| V.91 between compatible digital peers | Up to 64,000 bit/s each way |
| Raw DS0 / DOVBS | 56,000 or 64,000 bit/s each way |
| Baudot TTY/TDD | 45.45 or 50 baud, half duplex |

PCM needs a suitable digital bearer; raw DS0 needs its selected 56/64-kbit/s
payload preserved.
V.91 has been tested between software peers; external V.91 interoperability
has not been established. See [digital DS0 calls](docs/ds0.md) for framing and
bearer requirements.

Processing cost depends on modulation, role, training, and host cooling.
The [performance guide](docs/performance.md) records measured desktop/Pi 5
results and their limits. The packet benchmark reports averages, the worst
packet, and the worst consecutive three packets; use it on the intended host
before sizing a terminal server.

## Development and license

Run `make install-hooks` to enable the commit checks. See
[testing](docs/testing.md) and [memory checks](docs/memory-check.md) for focused
and interoperability work. Live calls require explicit private configuration.

sippymodem descends from the Smart Link user-space modem. Its copyright
notices and [BSD license](src/COPYING) are preserved. PJSIP has its own licensing
terms in the pinned submodule. Historical distribution documents are kept in
[docs/legacy](docs/legacy).
