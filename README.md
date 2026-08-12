# sippymodem

This repository contains an implementation of a complete dial-up
modem suite built from the user-space Smart Link software modem lineage. The
resulting modem is implemented in auditable source without proprietary binary
blobs, and its scope exceeds the original modem with features including
V.90/V.92 digital head-end operation, V.44 compression, V.92 PCM upstream,
Baudot RTTY, and raw ISDN data over voice bearer service (DOVBS).

The active implementation is split between the application under `src/` and
the modem signal processing and datapumps under `dsp/`. The top-level `make`
builds only that implementation and no longer builds or installs Linux kernel
drivers. Reusable filters, modulation primitives, framing support, datapump
registration, and multi-mode V.8 negotiation live directly under `dsp/`.
Mode-family implementations are grouped under `dsp/v21_202/`, `dsp/v22/`,
`dsp/v32/`, and `dsp/pcm/`; the PCM family contains V.34 now and is the home
for V.90/V.92 work as it arrives. Existing Smart Link copyright notices are
preserved in the source.

This branch is reconstructing that suite in small, reviewable steps. The
current daemon is entirely source-built and uses the host's native ABI. Its
modem core owns source-built SIP call-progress and low-speed FSK data pumps. SIP
call state selects the originating or answering tone pair, completes a bounded
training exchange, and hands asynchronous terminal bits to the native signal
path. V.8 now negotiates V.21, V.22/V.22bis, V.23, and V.32/V.32bis before an
exact handoff to the requested datapump; Bell and Baudot modes retain their
direct startup paths. V.21 and Bell 103 share a reusable 300-bit/s binary-FSK
channel. V.23 adds
an asymmetric 1200-bit/s forward channel and 75-bit/s backward channel. These
FSK modes use exact rational symbol timing and generated tone-pair detectors
rather than checked-in coefficient tables. V.22 adds 1200-bit/s differential
QPSK with role-correct 1200 Hz and 2400 Hz channels, a generated 75%-rolloff
root-raised-cosine pulse, and the constant-carrier call/answer exchange. It is
selectable with `AT+MS=22`; its first source-built slice intentionally uses raw
asynchronous transport while LAPM negotiation is rebuilt separately. The same
wire-compatible 1200-bit/s implementation provides Bell 212A through
`AT+MS=212`. V.22bis adds its generated 16-point constellation at 2400 bit/s.
V.32 and V.32bis provide negotiated 4800- and 9600-bit/s uncoded paths plus
7200-, 12000-, and 14400-bit/s trellis-coded paths whose compact encoder,
generated signal geometry, and bounded path decoder replace large lookup
tables. Bell 103 is selectable with `AT+MS=103` and V.23 with `AT+MS=23`.
Baudot TTY/TDD is selectable with `AT+MS=45`; a 50-only rate range selects 50
baud instead of the North American 45.45-baud mode. Its bounded text path uses
explicit half-duplex turns and preserves the complete mark-hold interval before
listening again.

When LAPM compression is requested, XID may offer both V.44 and the legacy
method but selects exactly one in the response. A negotiated V.44 session owns
bounded transmit and receive codecs at the I-frame character boundary, including
terminal backpressure and automatic compressed/transparent mode adaptation.

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
directions through V.32bis at 14400 bit/s, including bounded carrier-loss
handling. The same boundary check exchanges Baudot text in both directions
without overlapping the half-duplex turns.

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
standard output, and shuts down cleanly on SIGINT or SIGTERM. V.21, V.22, Bell
103, Bell 212A, V.23, V.22bis, V.32/V.32bis, and Baudot TTY/TDD are currently
registered. Higher-speed modes remain deliberately unavailable until their
source-built implementations arrive as reviewable slices.

Build with `make`, configure the daemon through the validated `SIP_*`
environment variables, and run `./src/slmodemd` without a hardware-device
argument or helper process. Set `SIP_RECORD_PREFIX` to enable the bounded
recording interface; the prefix must identify files that do not already exist.
If the SIP development package is unavailable, the daemon build reports that
dependency explicitly while `make test` continues to run the dependency-free
native suite.

Live interoperability calls are deliberately opt-in. Copy
`tools/live-call.example.json` to the ignored `.live-call.json`, make it
owner-readable only, replace the reserved example values locally, and enable
only the plans that are safe to dial. `make live-call` runs every enabled plan
in a fresh daemon process. Use repeated `--plan NAME` options with
`python3 tools/live_calls.py` to select a subset. `--list` emits
only a redacted manifest. The inert example covers an empty dial using the
configured direct SIP target and selects V.21 at exactly 300 bit/s for the
first physical-modem interoperability milestone. Add separately named plans
when comparing modem profiles or AT setups against that same direct target.
The example also includes a short placeholder online-data marker in
`online_expect_hex`. Replace it with the bytes emitted by the private peer.
An optional `online_send_hex` can drive an echo or request/response check;
outbound data is rejected unless an expected response is also configured.
Both patterns are limited to 512 bytes, and `online_timeout_seconds` bounds the
wait. When an expectation is present, a plan reports `connected` only after
the terminal receives that exact byte sequence. Alternatively,
`"round_trip_probe": "ping"` waits for that initial sequence, sends one fixed
PING command, and requires a complete nonce-and-timestamp PONG response. This
probe is mutually exclusive with custom transmitted bytes and exposes only its
name, byte count, and pass/fail result outside the private artifacts.

The current physical interoperability baseline has transferred an exact,
bounded 54-byte pattern over direct SIP at V.21 300 bit/s and at every
implemented V.32/V.32bis rate from 4800 through 14400 bit/s. These milestone
observations establish functional interoperation, not a repeatability or
impaired-line qualification matrix; their recordings remain private and ignored.
Fresh diagnostic calls reported symmetric raw V.21 at 300 bit/s without LAPM
or compression, V.32 at 9600 bit/s with LAPM and V.42bis, and V.32bis at 14400
bit/s with LAPM and V.42bis; each again transferred all 54 expected bytes. The
structured PING/PONG probe also proved terminal data in both directions on the
V.21 and 9600-bit/s V.32 calls.

The runner clears inherited SIP settings before applying the private file,
uses ephemeral local ports unless the file explicitly selects ports, and never
prints endpoints, credentials, terminal paths, or artifact paths. Each attempt
gets a private ignored directory under `live-call-artifacts/` containing its
redacted result, daemon diagnostics, and bounded media recordings. Enabled
plans and credentials require the private configuration file to have no group
or other permissions. Data-verifying plans also retain bounded
`terminal-tx.bin` and `terminal-rx.bin` evidence there with owner-only access;
the public result represents terminal payload only as expected and verified
byte counts. The runner also requests CONNECT diagnostics and records the
negotiated modulation, protocol, compression method, and transmit and receive
rates; it accepts only known labels and bounded numeric rates, discarding
arbitrary modem text.

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
