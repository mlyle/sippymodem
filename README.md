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

The recording file-set owner pre-creates a fixed set of nine evidence files
with private permissions and refuses to overwrite an existing capture. Tests
write only tiny temporary captures and remove them. RTP payload evidence is
preserved as exact octets, while native PCM is normalized to canonical
little-endian samples; a metadata or write error latches instead of silently
creating gaps or mixing formats.

`<prefix>.rtp.packets.tsv` adds one headerless row per valid RTP packet sent
or routed from the allowed peer. Columns are event (`sent`, `send-failed`,
`accepted`, `late`, `early`, `rejected`), monotonic nanoseconds, sequence,
RTP timestamp, SSRC, payload type, marker (0/1), payload octets, local receive
playout samples (including idle/prefill/concealment), and local samples
concealed. Sequence/timestamp wrap is kept
intact; time 0 means the clock was unavailable. Times mark application socket
observations before evidence writes, not kernel arrival or remote playout.
Together with the exact media, these rows expose local pauses, bursts, late
media and concealment. They contain no payload, addresses or credentials.
The journal is enabled by the existing recording option, not in normal calls.

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
state machine. V.92 supports PCM or conventional upstream and optional
QC1/QCA1 Quick Connect startup (`AT+V92Q=1`). These experimental profiles
can carry native duplex host data and perform full retrains. Sustained excessive
PCM-upstream residuals or ambiguous payload trigger up to two automatic full
retrains per call; ambiguous bits are never delivered. Generated-channel tests
are not evidence of robust operation on a physical line.

The PCM call owner supports V.92 shortened Phase 2/3 using caller-retained
training settings. Its read-only profile query uses the actual training
selection and live receive gain, rejecting retrain, rate exchange, and
cleardown. Tests reuse successful full-call settings through two successive
shortened calls on both codec laws, including 52000/40000 bit/s PCM data.
The digital Quick Connect signal exchange sends timed QTS/ANSpcm, qualifies
TONEq, and preserves Phase 2 handoff silence. Its answer-side timeout requests
normal V.8 fallback instead of reporting a successful shortened handshake.
The analogue exchange receives QTS and its reversal before qualifying ANSpcm,
sends at least 50 ms of TONEq, waits for ANSpcm to end, and retains the final
75 ms silence. Paired tests exercise the complete QC1 Phase 1 in both calling
directions; receive conditioning and retained-history admission remain owned
by the caller, not inferred from the signal exchange alone.
The V.8 calling and answering owners exchange explicitly admitted QC1/QCA1
messages using their existing FSK streams, while retaining ordinary menu
negotiation for unadmitted or unsuitable requests. A digital caller recognizes
continued ANSam as refusal without treating silence as refusal. Peer-keyed
history retention is not yet integrated into the modem, so its Quick Connect
opening uses full Phase 2 rather than claiming saved channel training. The
modem copies QC1/QCA1 and the received fragment tail into the PCM call owner,
preserves the training deadline across ordinary-V.8 fallback, and reports
`Startup: QC`, `V8`, or `V8-FALLBACK` from the observed exchange. Live Quick
Connect cases require that evidence; fallback calls still exercise data and
retrains but are reported as profile mismatches.

For broad incoming-call selection, issue `AT+MS=92,1,300,56000` and `ATS0=1`.
The factory selection remains fixed V.21. Automatic answering accepts V.8
callers offering lower families within the configured rate bounds; paired
media tests cover V.21, V.23, V.22bis, V.32, and V.34. It also recognizes a
legacy V.32 caller's sustained 1800-Hz response during ANSam and transfers its
acquired carrier into the existing 75-ms silence and AC turnaround exchange,
without repeating the answer tone or claiming V.8 negotiation. This early
branch follows recorded peer behavior, but the complete no-menu tone ladder
and V.22-only downgrade within the shared V.22/V.22bis family remain incomplete.

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

For an interactive modem console, install `splitvt` and C-Kermit, copy
`tools/sip-terminal.example.json` to the ignored `.sip-terminal.json`, make it
owner-only (`chmod 600 .sip-terminal.json`), and fill in the private provider
settings. Then run either:

```
python3 tools/modem_terminal.py 2600
python3 tools/modem_terminal.py pstn
```

These commands start the local `src/slmodemd` and register the selected account;
they do not dial or set auto-answer. The top half shows live SIP trace and modem
errors, and the bottom half starts C-Kermit connected to the modem's PTY. Enter
AT commands directly. `Ctrl-W` switches panes; `Ctrl-O` then `q` quits both and
stops the modem. Kermit's `Ctrl-\` then `Q` also quits the console and stops the
modem; `Ctrl-\` then `C` returns to its command prompt, where `exit` does the
same cleanup. Kermit startup files are skipped. Trace is private
and temporary, but can display addresses and dialed numbers; do not publish it
without redaction. No audio capture is enabled by this launcher.
The launcher enables `SIP_TRACE=1`, which can also be used with the daemon alone.
While media is configured, `RTP` trace lines report the codec, local port, actual
send destination (SDP or learned symmetric RTP), and interval packet/sample
counts once per second. `RX=0` means no UDP datagrams arrived in that interval;
`accepted=0` with nonzero RX indicates rejected packets. Buffered/played samples
are received media (possibly silence), not proof of audible voice or decoded
modem data; concealed/idle samples are locally supplied silence. These reports
continue even when incoming RTP stops. The rebuilt daemon does not accept the
old `-d3` option; use `SIP_TRACE=1` instead.
`CALL` status lines also appear at most once per second, including before media
arrives: they show the SIP state, host modem phase (training, error-control
negotiation, online, local retrain, or cleardown), and active datapump.
`TX-peak` and `RX-peak` are absolute signed-16-bit audio peaks for the interval,
with 32768 representing full scale. RX is before receive gain and excludes
generated playout silence; TX is successfully sent audio after transmit gain,
before G.711 encoding (or decoded exact PCM codewords). Zero means silence or
no samples; use the RTP counts to distinguish those cases. Peaks reset after
each report and between calls. Metering is disabled when tracing is off.
Credential-bearing and authentication records are omitted entirely; this is
not general redaction of SIP metadata.

Each profile contains native `SIP_*` settings, not shell commands. Ambient SIP
settings are ignored. `--config` selects another private JSON file and `--modem`
selects another executable. `SIP_REGISTER=0` is useful for explicitly configured
local-only tests. For provider dialing, `ATD<number>` sends that user part to the
configured SIP domain; `ATDsip:<user>@<host>` selects an explicit SIP destination.
PSTN routing and permitted number formats depend on the selected account.

This console does not configure a router or discover a public address. Existing
NAT settings belong in the private profile: `SIP_PUBLIC_ADDR` advertises the
public address, while `SIP_PORT` and `SIP_RTP_PORT` select local UDP ports. Static
port forwarding must preserve those ports. Symmetric RTP learns the media peer
from received packets, but is not STUN/TURN and does not guarantee traversal of
arbitrary NAT or CGNAT. Leave public-address overrides unset on a direct route.

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

Profile mismatches do not stop online testing: a connected call still attempts
data, remote and local retrain, and post-retrain data (unless retrain is disabled).
The dashboard keeps `done/exp+x` against the requested cases, but credits F/R/D/C
to the modulation and raw/LAPM protocol reported at CONNECT. Thus a V.90 attempt
that falls back to V.34 adds a V.34 result and a mismatch beneath V.90, without
counting twice in TOTAL. A rate-only mismatch stays in the same modulation/link
row. Mismatches remain eligible for failure extras; explicitly permitted analogue
fallbacks are not mismatches. Callback request legs appear separately as `q` in
history, and mismatches show the attempted profile on an indented `tried:` line.
PCM-session CONNECT details also report `Upstream: PCM` or `Upstream: V34`;
history displays this as `up=PCM` or `up=V.34`. The value describes the negotiated
upstream, not the enabled capability. A PCM-upstream case requires explicit PCM
evidence at the initial CONNECT: the V.92 label and bit rate alone are insufficient.
Missing or conventional-upstream reports count as mismatches, without stopping
the data and retrain exercises.
Results save the fresh local-ATO1 CONNECT separately in `retrain_*` profile
fields, even if the subsequent PONG fails. History adds an indented `after ATO1:`
line when this profile changes. Initial scoring is unchanged; missing retrain
details remain unknown rather than inheriting the initial rates or PCM label.
This final profile does not describe the intervening remote-retrain interval.

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
both directions under shared deadlines. Accepted RTP timestamps clock this local
media loop. Missing packets still advance sample time, so loss does not also
slow the transmit clock; reordered packets do not clock the modem twice.
Catch-up remains bounded by the receive window. It lets the RTP path run for 100 ms
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

Pass `--jobs 4` to run four calls at once. Each wave reserves a disjoint set of
loopback SIP and RTP ports before starting any endpoint, even when the private
configuration contains fixed ports. Results are buffered for the wave and
printed in matrix order, so concurrent endpoint activity cannot interleave the
bounded JSON records. `--stop-on-failure` completes the current wave and does
not schedule another one. Use `--media-codec PCMU` or `--media-codec PCMA` to
limit a comparison to one tested G.711 path.

Pass `--group v90 --media-codec PCMU` to sweep all 22 V.90 downstream rates
and all 36 conventional-upstream rate/cadence profiles against a controlled
digital peer. V.92 and analogue-peer fallback profiles remain unavailable
until their respective interoperability paths are established.

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
Set `V90_REPLAY_RX_GAIN` to apply a bounded linear gain to decoded receive
samples while preserving the exact recorded G.711 codewords; it defaults to
1.0.
By default, call direction selects the conventional analogue-originator/
digital-answerer topology; set `V90_REPLAY_PCM_ROLE` to `analog` or `digital`
when a recording exercises the opposite line role. An answered call or an
explicitly digital originating call must sustain PCM carrier unless it selects
V.34 fallback. Fallback selected directly in V.8 or later in V.90 is reported
separately as a selection check, not a PCM data pass. Any call can set
`V90_REPLAY_EXPECT_TEXT` to require an exact decoded terminal substring after
CONNECT, including after fallback. Online output is consumed with bounded
storage while preserving matches across reads. This distinguishes a sustained
carrier from a usable receive data path while keeping the recording itself in
the external evidence store.

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
