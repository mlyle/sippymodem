# sippymodem

sippymodem makes and answers dial-up modem calls over SIP and RTP. Call a
BBS, put a BBS or legacy device service on a phone number, or embed modem
lines in a terminal server. It runs in user space without local modem hardware.

`slmodemd` is a single-line modem with Hayes AT commands on a PTY.
`libsippymodem` provides a native byte API, explicit call control, and SIP
huntgroups for applications that handle several lines. It needs no PTY.

Derived from [Smart Link's slmodemd](https://github.com/leggewie/pkg-sl-modem).
The original copyright and [BSD license](src/COPYING) remain with the code.
Most new code is vibe-coded. I paid particular attention to the DSP,
interoperability, and performance.

## Modes

Factory settings negotiate V.92/V.90 with automatic fallback, including peers
without V.8. The modem offers LAPM, V.42bis, and V.44.

| Modes | Nominal line rates or limits |
| --- | --- |
| Bell 103, V.21, Bell 212A, V.22/V.22bis, V.23 | 300 to 2,400 bps, including 1,200/75 bps V.23 |
| V.32, V.32bis, V.34 | 9,600, 14,400, and 33,600 bps |
| V.90/V.92 PCM | Up to 56,000 bps down and 33,600 bps up |
| V.92 with PCM upstream | Up to 48,000 bps up with `AT+V92U=1` |
| V.91 and raw DS0/DOVBS | Up to 64,000 bps both ways on suitable digital paths |
| Baudot TTY/TDD | 45.45 or 50 baud, selected explicitly |

These are line rates before framing and compression. PCM needs a suitable
digital bearer. See [DS0 requirements](docs/ds0.md) for digital calls.
V.91 interoperability has been verified with software peers.

## Install and make a call

On Linux, extract the binary release and start `./bin/slmodemd` with your SIP
settings. To build from a Git checkout with GNU Make, a C toolchain, Python,
and pkg-config:

```sh
make pjproject-init
make -j4
```

Run `./src/slmodemd` from the build, or install with `make install` and run
`slmodemd`. For a direct SIP peer:

```sh
SIP_REGISTER=0 SIP_TARGET=sip:modem@peer.example.invalid ./src/slmodemd
```

The modem prints a PTY path. Open it in C-Kermit, type `AT`, and use `ATDT`
to call the configured peer. For incoming calls, use `ATA` after RING or
`ATS0=1` to auto-answer. The [quick start](docs/quickstart.md) gives the
Kermit commands. Telephone calls need a SIP provider or gateway that passes
modem audio over G.711. See the [running guide](docs/running.md) for setup.

Use `AT&A0` if your software expects a plain CONNECT line. Use `ATI11` for
the last call report. The [AT reference](docs/at-commands.md#quick-reference)
lists the commands and factory defaults.

For multi-line applications, start with the [native SDK guide](docs/library-api.md)
and [terminal-server examples](docs/terminal-server-integration.md). See
[building and installation](docs/building.md) for SDK linking, release tarballs,
and the macOS and Windows build paths.

## Performance

The October 4 baseline measured steady V.34 at about **2.9 ms per 20 ms packet**
on a Ryzen 7 5825U and **7.5 ms** on a Pi 5. A later throttled Pi 5 passed a
mixed seven-call SSH workload at **52% mean whole-machine CPU**. These are
measurements of specific builds and workloads. See the
[performance guide](docs/performance.md) for rates, test setup, and timing limits.

Run `make test` for local checks and `make install-hooks` for the commit gate.
[Live interoperability tests](docs/testing.md) require private configuration.
