# Native modem integration

`make` builds `src/libsippymodem.a`, `src/libsippymodem.so`, and the existing
`src/slmodemd` executable. Include `src/sippymodem.h` for the byte API and
`src/sippymodem_fd.h` for the optional native terminal/PPP adapter. The shared
library exports only the public `sippymodem_*` namespace. Static consumers also
link the libraries returned by `pkg-config --static --libs libpjproject` and
`-lm`.

## Ownership and scheduling

A server owns the SIP account, transport, native PJSUA endpoint, and its lines.
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

The ordinary native tests exercise two real concurrent non-PTY calls, literal
bytes, raw overflow, LAPM/V.44 backpressure in COMMAND, partial counts, DATA
resume, retained disconnect status, stale controls, startup rollback, bounded
attempt deadlines, event pressure, adapter EOF, and compatibility PTY/AT paths.
