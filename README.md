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
`dsp/v32/`, and `dsp/pcm/`; the PCM family contains V.34 and V.90 and is the
home for V.92 work as it arrives. Existing Smart Link copyright notices are
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
without overlapping the half-duplex turns and preserves exact G.711 codewords
for PCM datapumps.

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
103, Bell 212A, V.23, V.22bis, V.32/V.32bis, Baudot TTY/TDD, V.34, V.90, and
V.92 are registered. V.90 can select every standard PCM downstream rate from
28000 through 56000 bit/s and a conventional V.34 upstream from 4800 through
33600 bit/s at 3000, 3200, or 3429 symbol/s. V.90 and V.92 share one PCM call
state machine. The implemented V.92 profile uses PCM upstream with quick
connect disabled; V.92 conventional upstream and quick connect remain
unavailable and fail closed during profile selection.

Initialize the pinned pjproject submodule once, then build with `make`:

```
make pjproject-init
make
```

The ordinary build performs no network access. With its submodule initialized,
it automatically configures a minimal static PJSIP build and installs the
private build outputs under `.build/`. Without the submodule, it falls back to
a system `libpjproject` discovered through `pkg-config`. Set
`PJSIP_PROVIDER=submodule`, `system`, or `none` to make that choice explicit;
the default is `auto`. `make test` continues to run the dependency-free native
suite and reports a clear SIP signaling skip when neither provider is present.
When `ccache` is installed, all normal, test, and bundled-library compiles use
it automatically with a repository-local cache under `.build/ccache`. Set
`CCACHE=` to disable it or `CCACHE_DIR` to select another cache location.

Configure the daemon through the validated `SIP_*` environment variables, and
run `./src/slmodemd` without a hardware-device argument or helper process. Set
`SIP_RECORD_PREFIX` to enable the bounded recording interface; the prefix must
identify files that do not already exist.

Live interoperability campaigns are deliberately opt-in. Copy
`tools/live-campaign.example.json` to the ignored `.live-campaign.json`, make
it owner-only, fill in the three private SIP environments, and enable it. The
ordinary command needs no further arguments:

```
cp tools/live-campaign.example.json .live-campaign.json
chmod 600 .live-campaign.json
python3 tools/live_campaign.py
```

The runner resolves `HEAD` (or `--revision`) to an exact commit, exports that
tree and its pinned submodules into a private temporary directory, builds it
there with a bounded parallel build, and caches only the resulting executable,
build log, checksum, and revision manifest. Active development therefore cannot
replace a binary while a campaign is using it. Preparation has its own 0.5 Hz
elapsed-time display because the first build includes the pinned SIP stack;
`--prepare` performs only this frozen build.

The default standard campaign has 100 curated call configurations rather than
a Cartesian matrix: 53 raw datapump baselines plus 47 LAPM variants. LAPM is
omitted only for the 300-bit/s modes, V.23 1200/75, and Baudot. Three
seeded attempts produce 300 base calls. Each next call is chosen randomly from
the modulation/role cells with the lowest completed-call percentage, then from
the least-run cases in that cell. A case with a non-green modem result gets one
additional attempt; deterministic setup and local infrastructure errors do not.
The 22-case smoke and 146-case extended suites are
available with `--suite smoke` and `--all`. Selection options intersect: for
example,

```
python3 tools/live_campaign.py --modulation v34 --attempts 3
python3 tools/live_campaign.py --modulation v92 --feature pcm-upstream --callbacks
python3 tools/live_campaign.py --rate 9600 --role callback
python3 tools/live_campaign.py --list --suite smoke
```

The matrix varies modem family, receive/transmit rate, V.34 symbol rate,
ordinary and digital-head-end operation, conventional and PCM upstream, and
both outbound and callback-answer roles. Callback cases start the frozen
answerer first, place a bounded V.22bis request call, verify its random callback
nonce, wait for RING, issue ATA, and require the same nonce on the returned
data connection. Direct calls wait for the peer's fixed compatibility payload;
callback calls use the nonce-matched return marker as their initial data proof.
Before either data check, CONNECT must report the requested modulation, local
receive/transmit rates, and raw or LAPM protocol; a fallback cannot make a
forced-rate matrix cell green.
Both then send
`RETRAIN` and require matching START/END nonce markers, issue local `ATO1` and
require a fresh CONNECT, and finally send PING and require a structured PONG
response. The START nonce is reported as soon as it arrives; the matching END
must arrive within the same total 60-second retrain deadline. The dashboard
keeps raw and LAPM results on separate rows. Each callback-request leg is
attributed to the V.22bis/LAPM originator row as an auxiliary attempt: `Q`
means the greeting, request, and START acknowledgement all completed, while an
incomplete leg remains visible as connect, no-connect, or error. A request-only
failure does not consume or lower the requested callback modulation's maturity;
that case is counted only after the returning call reaches RING and the runner
issues ATA. Once the request leg is online, the runner verifies the peer's
initial data before sending the callback request; both that greeting and the
callback START marker have seven-second default deadlines.

An unattended peer service must treat a failed retrain or callback dial as a
call-local result, clean up, and rearm for the next call. If that service exits
on one failed call, later request legs can reach SIP while receiving no modem
answer; those request-only failures remain auxiliary evidence rather than
failures of each requested callback modulation. Remote retrain is an in-band
modem operation; peer host escape sequencing and `ATO1` are outside this
protocol. Any literal `+++` received remains ordinary online payload while the
runner waits for the matching END within the single retrain deadline.

The TTY summary clears and repaints every two seconds, names the active call
stage and seconds spent there, and places originator and callback results next
to each other on each modulation/transport row. Both roles use mutually
exclusive full, retrain, data, connect, and no-connect maturity buckets; the
separate request count covers successful auxiliary request legs. Setup and
local infrastructure failures remain separate. BUSY calls are recorded but do not
consume a logical attempt: the runner waits the configured busy cooldown and
retries up to its bound. `NO CARRIER` is always reported as a failed modem
attempt; the runner does not infer a SIP status that the modem interface did
not provide. A seven-second inter-call cooldown applies before the first
attempt and after every non-BUSY attempt. BUSY retries use a separate ten-second
cooldown and make up to nine retries, allowing the line about 90 seconds to
clear. A failure before any SIP call is placed does not consume a line
cooldown. The live dashboard uses otherwise unused terminal rows for a compact
newest-first call ledger; its flags show callback-request, connect, initial
data, remote retrain, local retrain, and post-retrain data milestones.
The seed, complete schedule, detailed results, daemon logs, terminal traffic,
and bounded media recordings remain in an owner-only ignored campaign
directory. No endpoint, credential, bind address, or recording path is printed
in the dashboard.

For a faster cross-implementation check without an external SIP endpoint,
copy `tools/local-interop.example.json` to an owner-only file beneath the
ignored `live-call-artifacts/` directory. Configure the two executable and
argument lists, select the desired AT profiles, set `enabled` to true, and run:

```
python3 tools/local_interop.py \
    --config live-call-artifacts/local-interop.json
```

The runner starts both modems, constructs loopback-only SIP and RTP routing,
waits for RING before issuing ATA, and verifies distinct terminal payloads in
both directions under shared deadlines. Accepted RTP packets clock this local
media loop so host scheduling jitter cannot insert silence into a modem
waveform. It lets the RTP path run for 100 ms
after CONNECT before injecting terminal data, because some peers announce the
connection before their datapump can safely accept its first character. Raw
daemon logs, terminal traffic, and media recordings remain in owner-only
artifacts; stdout contains only bounded, redacted status fields. The checked-in
example is an inert raw V.21 baseline,
but its AT command lists are intentionally generic so the same runner can test
LAPM and compression. A cross-implementation PCMU run has also connected at
symmetric V.34 9600 bit/s and verified all 15 payload bytes in each direction.

Use the same private configuration with `tools/fast_interop.py` to run every
implemented rate, both call orientations, and both G.711 laws. These are
ordinary SIP calls; accepted RTP packets clock the media loop immediately, so
the local matrix runs faster than real time. Add `--ato1` to escape the
candidate endpoint, issue ATO1, and verify a fresh byte exchange after it
returns online. The runner allows one second after the retrain CONNECT before
that fresh exchange because the peer's decoder and error-control path may
become data-ready slightly after its terminal result.

The public profile catalog covers Bell 103/212A, V.21, V.22/V.22bis, V.23,
Baudot at both supported rates, V.32/V.32bis, and every valid fixed V.34
bit-rate/symbol-rate pair selectable through `+MS` and `+V34B`. The fast
matrix runs all 55 V.34 pairs through both G.711 laws and both station roles:
through 21600 bit/s at 2400 symbol/s, through 26400 bit/s at 2800 symbol/s,
through 28800 bit/s at 3000 symbol/s, through 31200 bit/s at 3200 symbol/s,
and through 33600 bit/s at 3429 symbol/s. The optional 2743-symbol/s cadence
is excluded because the control does not accept it. The fast profile set also
includes V.90 calls at 28000/19200, 42667/26400, and 56000/31200 bit/s,
covering the 3000, 3200, and 3429-symbol/s conventional upstream cadences in
both implementation roles and both G.711 laws. The native
boundary also trains V.90 through V.8 over both G.711 laws, confirms exact
codeword output at
both endpoints, exercises selectable conventional upstream rates, and passes
distinct binary payloads in both directions. This is generated in-process
coverage, not external modem
interoperability. A faster focused datapump matrix separately completes all 22
standard V.90 downstream rates over both PCMU and PCMA, negotiates the highest
mutually enabled rate, and passes independent bit streams in both directions.
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

The native media integration test can replay optional V.22bis receive
recordings through the ordinary modem and datapump boundary. Set
`V22_REPLAY_RX_PCM` for an originating call or `V22_ANSWER_REPLAY_RX_PCM` for
an answered call. Each input is 8 kHz signed 16-bit little-endian PCM; the test
loads at most three minutes and requires carrier acquisition within 15 seconds.
The ordinary test run reports a clear skip for each missing recording.

The same integration test can replay optional V.34 receive recordings. Set
`V34_REPLAY_RX_PCM` for an originating call or `V34_ANSWER_REPLAY_RX_PCM` for
an answered call. Set the corresponding `V34_REPLAY_SYMBOL_RATE` or
`V34_ANSWER_REPLAY_SYMBOL_RATE` to 2400, 2800, 3000, 3200, or 3429 when the
recorded call forced that symbol rate; omitting it preserves automatic
selection. Set `V34_REPLAY_BIT_RATE` or `V34_ANSWER_REPLAY_BIT_RATE` to the
recorded primary rate; omitting it preserves the 9600-bit/s replay default.
Set `V34_REPLAY_LINK_MODE` or `V34_ANSWER_REPLAY_LINK_MODE` to `lapm` for a
LAPM recording; omitting it preserves raw framing.
Set the corresponding `V34_REPLAY_RETRAIN_SAMPLE` or
`V34_ANSWER_REPLAY_RETRAIN_SAMPLE` to inject ATO1 at an exact receive-sample
boundary and require a second CONNECT. `V34_REPLAY_EXPECT_TEXT` and
`V34_ANSWER_REPLAY_EXPECT_TEXT` can additionally require an exact terminal
substring after the initial CONNECT, or after the second CONNECT when a
retrain boundary is configured. Each input is 8 kHz signed 16-bit
little-endian PCM; the test loads at most three minutes, reproduces the raw
V.34 setup and call role, and requires carrier acquisition. The ordinary test
run reports a skip when no recording is supplied, so large and private call
evidence stays outside this repository.

V.90 receive recordings can likewise be replayed from the exact G.711 RTP
payload stream with `V90_REPLAY_RX_G711`. Select `V90_REPLAY_LAW` as `PCMU` or
`PCMA`, `V90_REPLAY_ROLE` as `answerer` or `originator`, and describe a forced
call with `V90_REPLAY_DOWNSTREAM_RATE`, `V90_REPLAY_UPSTREAM_RATE`, and
`V90_REPLAY_SYMBOL_RATE`; select `V90_REPLAY_LINK_MODE` as `raw` or `lapm`.
By default, call direction selects the conventional analogue-originator/
digital-answerer topology; set `V90_REPLAY_PCM_ROLE` to `analog` or `digital`
when a recording exercises the opposite line role. An answered call or an
explicitly digital originating call must sustain carrier. Either can set
`V90_REPLAY_EXPECT_TEXT` to require an exact decoded terminal substring after
CONNECT. This distinguishes a sustained carrier from a usable receive data path
while keeping the recording itself in the external evidence store.

For short local iterations, `MODEM_INTEGRATION_FILTER` selects ordinary cases
by name and `V34_MATRIX_FILTER` selects V.34 rate-matrix cases by name. Both
filters fail when they match nothing, so a stale case name cannot produce a
false pass.

Run `make install-hooks` once per checkout to enable the repository's pre-commit
gate. It rejects staged whitespace errors and commits that fail the native test
suite.

When the submodule build or a system `libpjproject` is available, the native
suite also opens, polls, and closes a real local SIP UDP transport, normalizes
account credentials, and verifies a real loopback registration request.
Registration progress and results are exposed as typed events, and confirmed
registrations receive a bounded graceful unregistration on shutdown. The test
reports a clear skip when that optional dependency is unavailable.

Historical Smart Link and distribution documentation is retained under
`docs/legacy/`. It describes the original package and is not current build or
installation guidance. The original license is preserved at `src/COPYING`.
