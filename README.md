# sippymodem

This repository contains sippymodem, an implementation of a complete dial-up
modem suite built from the user-space Smart Link software modem lineage. The
resulting modem is implemented in auditable source without proprietary binary
blobs, and its scope exceeds the original modem with features including
V.90/V.92 digital head-end operation, V.44 compression, V.92 PCM upstream,
Baudot RTTY, and raw ISDN data over voice bearer service (DOVBS).

The active implementation lives under `src/`; the top-level `make` builds only
that implementation and no longer builds or installs Linux kernel drivers.
Existing Smart Link copyright notices are preserved in the source.

This branch is reconstructing that suite in small, reviewable steps. The
current transitional baseline still requires a 32-bit C toolchain and links the
legacy `src/dsplibs.o`; an early step will replace that object with a minimal
native skeleton before capabilities are restored incrementally.

Focused tests and compact fixtures belong in this repository. Large audio,
packet, and regression-vector collections will live in a parallel repository,
which may be attached as an optional submodule. Tests and harnesses for that
corpus may remain here, but they will skip cleanly when their vectors are not
present.

Run the native tests with `make test`. The suite exercises asynchronous and
HDLC framing, validated SIP/media configuration, safe SIP URI construction,
table-free G.711 conversion, strict RTP packets, bounded receive playout,
transmit timeline continuity, UDP peer handling, and lifecycle-ordered
recording hooks without linking the legacy DSP object or requiring external
vectors.

The current RTP work is a tested, allocation-free media stream connected to a
bounded, nonblocking UDP endpoint. It is not yet connected to SIP call control
or a native V.21 core, so this transitional tree does not yet place a V.21
call.

Run `make install-hooks` once per checkout to enable the repository's pre-commit
gate. It rejects staged whitespace errors and commits that fail the native test
suite.

Historical Smart Link and distribution documentation is retained under
`docs/legacy/`. It describes the original package and is not current build or
installation guidance. The original license is preserved at `src/COPYING`.
