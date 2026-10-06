# Native modem integration

`make` builds `src/libsippymodem.a`, `src/libsippymodem.so`, and the existing
`src/slmodemd` executable. Include `src/sippymodem.h` for the byte API and
`src/sippymodem_fd.h` for the optional native terminal/PPP adapter. The shared
library exports only the public `sippymodem_*` namespace. Static consumers also
link the libraries returned by `pkg-config --static --libs libpjproject` and
`-lm`.

See the [terminal-server integration guide](terminal-server-integration.md) for
pool topology, owner-loop and native PPP examples, and [DS0 operation](ds0.md)
for explicit 56/64-kbit bearer policies.

## Ownership and scheduling

A server owns SIP account endpoints, UDP transports, huntgroups, the native
PJSUA endpoint, and its modem lines. SIP endpoint settings and modem line
settings have separate ownership.
Each line owns its modem, DSP, scheduler, RTP socket, recording state, and DTE
buffers. Creating a server or line copies configuration strings and reads no
environment variables. Explicitly start the server, create/start lines, then
call `sippymodem_process(server, monotonic_now_ns, &deadline)` on one owner
thread. Establish that clock with one process call before dialing. A line's
RTP port is reserved when it starts, before either outgoing or incoming SDP.

There can be one started server per process because PJSUA has one endpoint and
global callback registration. Callback dispatch validates the server, account,
call slot, and owning line. The bundled SDK is configured for 32 SIP call slots;
public servers accept up to 32 lines, with eight by default. A system SDK must
have enough compiled `PJSUA_MAX_CALLS` slots for the requested line count.
Retired SIP transactions can temporarily consume slots after cancellation.
The bundled SDK uses 256 I/O keys for RTP/RTCP and signaling. Closed SDK
keys have a bounded 500 ms safe-reuse delay; temporary exhaustion returns an
ordinary resource error, while existing calls keep running. Its release
assertion policy applies only to the SDK; native test assertions remain enabled.

The first implementation runs every line's SIP, RTP, DSP, framing, and
compression on the same native owner thread. Each process pass polls SIP once,
uses RTP batches of at most 64 packets (including call-boundary cleanup), and
processes at most four audio frames per line. Excess received clock credit is
retained for later passes. It returns an absolute monotonic deadline; an immediate deadline means
more work is due. Backwards timestamps are rejected, and deadline addition
saturates at `UINT64_MAX`. The call timeout covers SIP progress, early media,
training, and error-control establishment until the line is ready.

These bounds limit work per pass; they are not a guarantee that every workload
fits its realtime budget. Capacity measurements with separate modem processes
across CPU cores do not establish capacity for this single owner. Future
parallelism belongs at the server/line scheduling boundary: give whole line
state to a worker, with explicit ownership transfer, rather than locking DSP
internals. No Python callback or payload copy occurs in protocol processing.

Line admission and dial return BUSY while bounded cleanup of old queued RTP is
pending. Processing drains those datagrams before a replacement call can start,
so cleanup cannot discard its training packets.

The retained process globals are the SDK dispatch bridge and the compatibility
modem debug level, which defaults to zero and is never configured by the native
API. Datapump registration tables are constant. Modem, DSP, codec, media, and
recording state are owned instances. SDK logging is scoped to native SDK entry
and restores the embedding process's logger and level before returning. The
native API installs no signal handlers and emits no diagnostics by default.

## SIP endpoints and huntgroups

The default endpoint uses the SIP settings in `server_config`; its borrowed
huntgroup automatically contains every modem line. Existing `dial()` calls use
that endpoint. For explicit routing, set `omit_default_endpoint = true`, create
huntgroups, and create endpoints attached to those groups. The server supports
up to eight endpoints and 32 groups, including any default handles. Destroying
the server invalidates all these child handles.

Group membership is configured before attached endpoints start. Add/remove
returns `BUSY` while any attached endpoint is started or the member has an active
call. Creating or destroying a line updates the automatic default group. Line
destruction also removes it from every explicit group. A line can belong to
several groups; one call reservation excludes it everywhere. Admission chooses
the first eligible member in server line-slot order, which can change when a
line is destroyed and its slot reused. It does not rotate between idle members.

Start the server and modem lines, then explicitly start configured endpoints.
Only the default endpoint starts with the server. Each line reserves its RTP
socket before it can accept SDP. An endpoint can accept several incoming calls
through one account and UDP socket. Endpoints with identical bound/public
address and listen-port settings share their owned transport, including
identical ephemeral-port settings. Different settings create separate
transports. Several endpoints can use one group or separate groups with
independent modem profiles.

The initial INVITE must arrive on the selected endpoint's UDP transport and
match its Request-URI user/host policy. A missing `request_user` derives the
canonical identity URI user (or configured username). A missing `request_host` accepts
host aliases on the owned transport, including registered Contact, NAT, and PBX
rewrites. `"*"` explicitly accepts all users or hosts; an explicit host enables
strict matching. Overlapping endpoint policies on one transport return `BUSY`
at start. Registered accounts with an explicit public address use their user
routing key in Contact, so the registrar routes back to the same policy.
Unknown requests receive 404 before a call slot is assigned. A matched endpoint
with no eligible member receives 486; it never spills into another group.
Retained RX and pending bounded RTP cleanup both block membership admission.
ACK, CANCEL, BYE, and in-dialog INVITEs keep their transaction/dialog handling.

Use `dial_from(line, endpoint, target, &generation)` for explicit outgoing
accounts. `endpoint_get_status` reports start state, listen port, and coarse
registration state/result with a sequence number. Private `call_facts` retains
the stable native endpoint ID for the call generation, distinct from recycled
SDK account IDs. It also contains the numeric called number and caller number;
withheld, invalid, oversized, and outgoing caller identities stay empty. Keep
these facts out of public diagnostics.

Stopping an endpoint ends only its calls, including when another endpoint
shares its group or transport. Its stop does not wait for network replies.
Destroy explicit endpoints before destroying their group; a referenced group
returns `BUSY`. Default handles remain server-owned. Destroying and recreating
an endpoint assigns a new native ID, while previous call facts remain readable.

For a direct five-line pool:

```c
struct sippymodem_server_config server_config;
sippymodem_server_config_init(&server_config);
server_config.max_lines = 5;
server_config.omit_default_endpoint = true;
struct sippymodem_server *server = sippymodem_server_create(&server_config);
struct sippymodem_huntgroup *pool = sippymodem_huntgroup_create(server);
struct sippymodem_line_config line_config;
sippymodem_line_config_init(&line_config);
struct sippymodem_line *lines[5];
for (unsigned int i = 0; i < 5; ++i) {
    lines[i] = sippymodem_line_create(server, &line_config);
    sippymodem_huntgroup_add(pool, lines[i]);
}
struct sippymodem_endpoint_config sip_config;
sippymodem_endpoint_config_init(&sip_config);
sip_config.identity_uri = "sip:pool@example.invalid";
sip_config.listen_port = 5060;
sip_config.request_user = "*"; /* All called numbers on this direct endpoint. */
struct sippymodem_endpoint *endpoint =
    sippymodem_endpoint_create(server, pool, &sip_config);
sippymodem_server_start(server);
for (unsigned int i = 0; i < 5; ++i)
    sippymodem_line_start(lines[i]);
sippymodem_endpoint_start(endpoint);
/* Check all return values in the application, then run the owner loop. */
```

## Bytes, modes, and readiness

`send` and `receive` are nonblocking copies. Check the exact accepted/received
prefix count and retry the remaining bytes; no pointer is retained. A positive
prefix returns `OK`, zero progress returns `AGAIN`, and `CLOSED` follows a
retained receive queue being drained. Zero-length calls succeed without touching
payload pointers. DTE data is always literal, including NUL, `+++`, and AT text.
The native API has no AT parser, PTY, termios, escape detector, or result text.
Those remain in the executable compatibility frontend.

TX counts measure local, unframed DTE bytes. They exclude compressed output,
frames already sent, and unacknowledged LAPM data. RX counts measure decoded
local payload bytes. Capacities are explicit, from 16 bytes to 1 MiB, with
4096 TX / 16384 RX defaults. LAPM and compression preserve blocked decoded
output and use receive backpressure. Raw receive overflow retains the accepted
prefix, records a cause, and deliberately terminates at the next safe owner
boundary; it never silently discards overflow bytes while keeping a call alive.

Use `set_dte_mode` with the current generation instead of `+++`. Mode changes
reject pending TX. Online COMMAND preserves carrier and receive bytes while
DSP, RTP, LAPM, and compression keep running. Payload receive remains available;
send pauses until DATA. DATA resume does not retrain. The compatibility frontend
keeps AT replies in its own bounded control queue and holds incoming payload
while the terminal is in online command mode.

`EVENT_SIP_ESTABLISHED` means SIP accepted the call. `EVENT_CONNECTED` and
`status.ready` mean modem training and configured error control are ready.
Send payload immediately at this event; no application startup delay is needed.
Status reports hook, carrier, phase, DTE mode, Hayes modulation number,
directional bit/symbol rates, negotiated compression, and local queue counts.
Default error control offers LAPM with raw fallback; REQUIRED refuses raw.
Default modulation is automatic V.92 with V.34 upstream; PCM upstream requires
its explicit config switch. V.91 and automatic digital-role negotiation are on.

## Calls and events

Dial, answer, hangup, and mode controls carry a call generation. A stale control
returns `STALE` and cannot affect a replacement call. Each line has a unique id
for the server lifetime, a state sequence, and retained disconnect sequence,
generation, cause, origin, and reason. Polling after a quick connect/disconnect
still exposes the terminal cause. Incoming called-number facts use the numeric
SIP Request-URI user; absent, invalid, and oversized users remain empty instead
of being truncated. Facts are private routing data and belong outside public
logs.

Poll the borrowed event fd along with your own application fds, and drain coarse
events with `poll_event`. Events contain snapshots rather than borrowed payload
pointers. Event overflow is explicit, sticky, and fatal: the owner closes active
calls, reserves disconnect delivery, and preserves per-line terminal status.
Drain the evidence and recreate the server to recover. Stop is idempotent and
does not wait for unregister or hangup replies. Cleanup continues after a
failure; stop/hangup return the first error while retained status remains
readable. Repeated stop succeeds after cleanup. Stop/destroy one line leaves the
other lines alive. Destroy a server frees every remaining line and invalidates
all child handles. Destroy native adapters before their borrowed line.

## PPP and terminal file descriptors

Create an adapter for a ready DATA call and pass its returned socket fd directly
to `pppd` or another native terminal peer. The returned fd is nonblocking and
close-on-exec: explicitly preserve it through spawn/pass_fds or clear CLOEXEC
before exec. Call `sippymodem_fd_process` before/after the server process call;
it pumps at most 4096 bytes each way per pass using the byte API and retains
partial writes. Python may coordinate status and process ownership, but it must
not copy steady-state PPP payload.

Online COMMAND pauses fd-to-modem input and keeps modem-to-fd output active.
Bytes still buffered in the peer socket are outside modem queue counts. Carrier
loss drains retained RX before closing the owned endpoint so the peer gets EOF.
A peer EOF deliberately hangs up that generation, including in COMMAND when
paused peer input remains queued. A stale adapter closes only
its own endpoint and leaves a replacement call untouched.

The ordinary native tests exercise five concurrent incoming calls through one
endpoint, shared/isolated huntgroups, endpoint teardown/reuse, strict route
rejection, two real concurrent non-PTY calls, literal
bytes, raw overflow, LAPM/V.44 backpressure in COMMAND, partial counts, DATA
resume, retained disconnect status, stale controls, startup rollback, bounded
attempt deadlines, event pressure, adapter EOF, and compatibility PTY/AT paths.
