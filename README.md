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
deterministic modem audio SDP policy, table-free G.711 conversion, strict RTP
packets, bounded receive playout, transmit timeline continuity, UDP peer
handling, negotiated SIP-to-RTP binding, and lifecycle-ordered recording hooks
without linking the legacy DSP object or requiring external vectors.

The SIP adapter owns one validated incoming or outgoing call, supports ringing,
answer, and hangup, and reports bounded call-progress events. Offers and answers
advertise one configured modem-safe audio format and RTP port, and negotiated
remote RTP endpoints have an explicit configured/inactive lifecycle. The
tested RTP runtime owns bounded jitter, playout, and packet storage, enforces a
single configured frame size, and carries the recording hook through every RTP
stage. Its endpoint can consume negotiated media through a validated,
transactional binding. A session owner opens RTP first so signaling advertises
the actual bound port, advances signaling and media together, and releases both
transports safely. It exposes typed call control and one exact-frame playout and
transmit boundary. The daemon does not use that owner yet, and no native V.21
core is attached, so this transitional tree does not yet place a V.21 modem
call. A bounded audio path now applies saturating directional gain, records the
PCM immediately around a typed modem-core callback, and preserves failures at
each playout, modem, and transmit boundary. Exact sample-derived frame pacing
supports bounded catch-up and treats an event-loop overrun as an explicit
failure instead of silently shifting the media clock. A scheduler arms that
clock only while negotiated media is active and deactivates the audio path on
any timing, modem-core, playout, or transmit failure.

Run `make install-hooks` once per checkout to enable the repository's pre-commit
gate. It rejects staged whitespace errors and commits that fail the native test
suite.

When `libpjproject` is available through `pkg-config`, the native suite also
opens, polls, and closes a real local SIP UDP transport, normalizes account
credentials, and verifies a real loopback registration request. Registration
progress and results are exposed as typed events, and confirmed registrations
receive a bounded graceful unregistration on shutdown. The test reports a
clear skip when that optional development dependency is unavailable.

Historical Smart Link and distribution documentation is retained under
`docs/legacy/`. It describes the original package and is not current build or
installation guidance. The original license is preserved at `src/COPYING`.
