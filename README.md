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
current daemon is entirely source-built and uses the host's native ABI. Its
modem core owns source-built SIP call-progress and 300-bit/s FSK data pumps. SIP
call state selects the originating or answering tone pair; a bounded mark
exchange then reports `CONNECT 300` and hands asynchronous terminal bits to the
native signal path. V.21 and Bell 103 share a reusable binary-FSK channel with
exact rational symbol timing and generated tone-pair detectors rather than
checked-in coefficient tables. Bell 103 is selectable with `AT+MS=103`.

Focused tests and compact fixtures belong in this repository. Large audio,
packet, and regression-vector collections will live in a parallel repository,
which may be attached as an optional submodule. Tests and harnesses for that
corpus may remain here, but they will skip cleanly when their vectors are not
present.

The recording file-set owner pre-creates a fixed set of eight evidence files
with private permissions and refuses to overwrite an existing capture. Tests
write only tiny temporary captures and remove them. RTP payload evidence is
preserved as exact octets, while native PCM is normalized to canonical
little-endian samples; a metadata or write error latches instead of silently
creating gaps or mixing formats.

Run the native tests with `make test`. The suite exercises asynchronous and
HDLC framing, validated SIP/media configuration, safe SIP URI construction,
deterministic modem audio SDP policy, table-free G.711 conversion, strict RTP
packets, bounded receive playout, transmit timeline continuity, UDP peer
handling, negotiated SIP-to-RTP binding, and lifecycle-ordered recording hooks
without linking the legacy DSP object or requiring external vectors. A compact
integration check trains two real modem cores through the PCMU and PCMA media
and recording boundaries, then verifies exact binary terminal data in both
directions and bounded carrier-loss handling.

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
transmit boundary. A host owner composes that session with a bounded audio path
and exact sample-derived scheduler. Its nonblocking step records every network,
line, and modem boundary, performs bounded catch-up, and reports rejected media
updates without interrupting an established stream. Timing, modem-core,
playout, or transmit failure suspends audio and requests hangup until a new
media lifecycle begins. A guarded adapter presents the transitional Smart Link
sample processor to that host at the exact 8 kHz SIP media clock. The foreground
daemon now owns that runtime directly, publishes its pseudoterminal path on
standard output, and shuts down cleanly on SIGINT or SIGTERM. V.21 and Bell 103
are currently registered; faster modes remain deliberately unavailable until
their source-built implementations arrive as reviewable slices.

Build with `make`, configure the daemon through the validated `SIP_*`
environment variables, and run `./src/slmodemd` without a hardware-device
argument or helper process. Set `SIP_RECORD_PREFIX` to enable the bounded
recording interface; the prefix must identify files that do not already exist.
If the SIP development package is unavailable, the daemon build reports that
dependency explicitly while `make test` continues to run the dependency-free
native suite.

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
