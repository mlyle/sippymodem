# Terminal-server integration boundary

Use the native library for terminal-server lines. Include
[sippymodem.h](../src/sippymodem.h) and, for native stream/PPP pumping,
[sippymodem_fd.h](../src/sippymodem_fd.h). `make` builds
`src/libsippymodem.so`, `src/libsippymodem.a`, and the compatibility
`src/slmodemd` executable. Shared-library exports use `sippymodem_*`.
Static consumers also link `pkg-config --static --libs libpjproject` and `-lm`.
The headers are the source of truth for types, layouts and declarations;
generate bindings from them rather than duplicating structure offsets.

The library exposes server/line byte I/O, a native fd adapter, and explicit
SIP endpoint/huntgroup ownership. Use the current headers when compiling
against the library.

## Application and library responsibilities

| Owner | Responsibilities |
| --- | --- |
| Terminal server | Service selection, BBS/PPP process lifecycle, authentication, application fds, line-handle map and control requests |
| Native server | PJSUA lifecycle, SIP accounts/transports, callback dispatch, events and owner-loop scheduling |
| Native line | Modem state, RTP socket, DSP, framing, error control, compression, recording and bounded DTE queues |
| Compatibility executable | CLI/environment parsing, signals, PTY/termios, AT parser, echo, guarded escape and result text |

The byte API requires no PTY and always treats NUL, XON/XOFF, `+++` and AT text
as data. Use explicit controls for answer, hangup and COMMAND/DATA. It installs
no process signal handlers, reads no environment configuration, and does not
call Python. Configuration strings are copied during create; limits and empty
optional fields are defined in the public header.

One started SDK server is supported per process. All operations on it and its
children, including status reads and byte I/O, belong to one owner thread. If Python handles application policy,
marshal its commands to that owner and return copied status/events. Keep SIP,
RTP, DSP, modem timers, framing and compression in native code. Keep the native
PPP pump in that owner too; Python must not relay steady-state PPP bytes.

## Current lifecycle and owner loop

1. Initialize server and line configs with their `*_config_init()` functions.
   Set SIP account/transport settings in the server config and modem/media/queue
   settings in each line config. A zeroed structure does not replace the initializer.
2. Create/start the server. Create/start each line, keeping a map from its
   `status.line_id` to its opaque handle. Line start reserves RTP before SDP.
3. Call `sippymodem_process(server, monotonic_now_ns, &deadline)` once before dialing.
4. Keep processing, draining events, servicing application commands and pumping
   active native fd adapters. Use the returned absolute monotonic deadline.
5. On shutdown, destroy adapters, close caller-owned peer fds, stop lines/server,
   then destroy handles. Server destroy invalidates every remaining child.

```c
/* Owner-loop outline; dispatch/poll helpers belong to the terminal server.
 * Check each returned result, including the final poll_event result. */
for_each_adapter_before_modem(); /* sippymodem_fd_process(adapter) */
sippymodem_process(server, monotonic_now_ns, &next_deadline_ns);
for_each_adapter_after_modem();
while (sippymodem_poll_event(server, &event) == SIPPYMODEM_OK)
    dispatch_copied_event(&event);
/* Poll the borrowed server event fd, adapter poll fds and application fds
 * until next_deadline_ns. Do not close borrowed fds or consume their bytes. */
```

Only `process()` advances protocols. It is nonblocking, polls SIP once, drains
RTP in batches of at most 64 packets, and runs at most four audio frames per line.
An immediate deadline means native work remains. Continue with another bounded
pass; otherwise wait for readiness/deadline. Backward timestamps are rejected.
An event fd is a wakeup interface; it does not replace deadline-driven processing.

Avoid unconditional `POLLOUT` polling on adapter sockets: they are usually
writable and can make the owner spin. Deadline-driven adapter passes retry
output. Enable peer-input `POLLIN` only for ready DATA with `tx_free > 0`;
continue observing hangup/EOF while input is paused. This also avoids spinning
on queued peer bytes during COMMAND or transmit backpressure.
See [media clock](media-clock.md) for remote pacing and gap recovery.

The default server limit is eight lines, with a maximum of 32 using the bundled
SDK. Defaults are automatic V.92/V.34 upstream, V.91 enabled, automatic digital
role, LAPM with raw fallback, both compression types, PCMU, 20-ms packets,
60-ms remote gap hold, 60-second whole-attempt timeout, 4096-byte TX and
16384-byte RX queues. PCM upstream requires its explicit config switch.

## Readiness, calls and generations

| Operation or event | Application rule |
| --- | --- |
| `sippymodem_dial(line, target, &generation)` | Starts an attempt; retain the returned generation |
| `EVENT_INCOMING` | Find the line by id, inspect matching-generation private facts, then answer with the event generation |
| `sippymodem_answer(line, generation)` | Accepts the current incoming attempt |
| `EVENT_SIP_ESTABLISHED` | SIP accepted; modem/data readiness is still pending |
| `EVENT_CONNECTED` / `status.ready` | Modem and configured error control are ready for payload |
| `sippymodem_finish_tx(line, generation)` | Seals input; poll until acknowledged/emitted, then hang up explicitly |
| `sippymodem_hangup(line, generation)` | Ends this call immediately; other lines continue |
| `EVENT_DISCONNECTED` | Record retained cause; drain RX before reusing the line |

Store `(line_id, generation)` with every asynchronous control request. `STALE`
has no effect. Looking up a fresh generation when an old timer fires would
apply that timer to the replacement call, so use the original generation.
Private call facts also carry a generation: compare it before using called or
caller numbers. Keep those facts out of public logs.

Use `sippymodem_read_log()` for per-line connection and training diagnostics.
Keep a separate cursor (initially zero) for each line and reader, and copy logs
on the native owner thread after processing. The 64-record ring reports missed
records when a reader falls behind; it never interrupts a call. Records carry
line ID, call generation, sequence, native time, severity and bounded text.
Forward those copies to the terminal server's logger. Use typed status/events
to drive call handling; log text is diagnostic and may change. Logs survive hangup/stop
until line destruction; training logs have no separate wake fd. See the
[complete log API and example](library-api.md#per-line-diagnostics).

Status is a copied snapshot of hook, carrier, connection/SIP phase, DTE mode,
negotiated modulation, directional bitrates and symbol rates, error control,
compression and local queue counts. `state_sequence` identifies changes.
`disconnect_sequence`, `disconnect_generation`, cause, origin and reason remain
available after the call. Poll status to recover facts even if a short call
connects and disconnects between application polls.

## Exact byte I/O and backpressure

`send(line, bytes, length, &accepted)` copies only the reported prefix.
Keep and retry the suffix. `receive(line, buffer, capacity, &received)` drains
only its reported prefix. Neither retains caller pointers. Positive progress
returns `OK`; no progress normally returns `AGAIN`. Receiving drains retained
RX after carrier loss before returning `CLOSED`. A zero-length operation succeeds.
Compare result enum names explicitly; their numeric order is not a success test.

`tx_pending` and `tx_free` measure local unframed DTE bytes, excluding encoded,
compressed and unacknowledged link work. `rx_ready` measures decoded local bytes.
A positive `tx_free` alone does not imply a call is ready or in DATA mode.
LAPM/compression retain blocked decoded output and apply receive backpressure.
Raw overflow retains the accepted prefix, reports its cause and ends the call.

### BBS EOF and final messages

TCP EOF does not mean the modem has finished transmitting the BBS's final
message. Submit all remaining application bytes using exact partial counts,
stop submitting new output, then call `sippymodem_finish_tx(line, generation)`.
This native operation tracks compression, frames, acknowledgments and the raw
modulator tail. Further nonempty sends return `AGAIN` with zero accepted bytes.

Poll completion while the owner loop continues `process`, events and receive
handling. Hang up on `OK` or an application-owned deadline. On LAPM, `OK` means
the peer acknowledged all accepted payload. Raw modes finish emitting the
signal and cannot promise that the remote application received it. `CLOSED`
means the call ended; `STALE` retires the completion request for the old call.
`ERROR` means completion cannot be established and permits immediate cancellation.
The operation preserves carrier and RX and also works in online COMMAND.
Do not use an empty `tx_pending` count or a fixed sleep as a completion test.

```c
/* Run this after all application output has been accepted, on a ready call. */
enum sippymodem_result result = sippymodem_finish_tx(line, generation);
if (result == SIPPYMODEM_OK ||
    (result == SIPPYMODEM_AGAIN && now_ns >= finish_deadline_ns) ||
    result == SIPPYMODEM_ERROR) {
    result = sippymodem_hangup(line, generation);
    /* Handle result and retire this generation's completion request. */
}
/* AGAIN: continue the owner loop and poll completion again.
 * CLOSED/STALE: retire this request without acting on a replacement call. */
```

### Command and data mode

`set_dte_mode(line, generation, COMMAND)` requires no pending local TX.
COMMAND preserves carrier, RX and protocol processing; sends return `AGAIN`.
`DATA` resumes without retraining. Use these controls directly rather than
sending an escape sequence. An fd adapter follows the same mode rule.

| Result | Meaning/action |
| --- | --- |
| `OK` | Operation or reported prefix completed |
| `AGAIN` | No progress/readiness yet; retry after native processing/readiness |
| `CLOSED` | Call/line is closed; receive may first return retained bytes |
| `BUSY` | Resource/state conflict, pending TX/RX or bounded old-media cleanup |
| `STALE` | Generation no longer matches; discard that control |
| `INVALID` | Invalid argument, bounds or unsupported reentrant call |
| `OVERFLOW` | Explicit bounded overflow; inspect retained status/evidence |
| `ERROR` | Native operation failed; inspect status and unwind/stop |

Drain coarse events regularly. Event overflow is sticky and fatal: active calls
close, terminal evidence is retained, and recovery requires recreating the server.
Stop is idempotent, continues cleanup after errors and does not wait for network
hangup/unregister replies. Destroying an adapter must precede its borrowed line.
The existing [native API reference](library-api.md) gives additional ownership
and SDK resource details.

## SIP endpoints and huntgroups

One server owns the SDK lifecycle and UDP transports. Modem settings belong to
lines. SIP identity, credentials, registration, transport, and incoming route
policy belong to endpoints. Each endpoint selects exactly one huntgroup. Several
endpoints may share a group; each line keeps its own modem profile.
There is no spill from a full or unmatched group to another group.

```c
struct sippymodem_endpoint;
struct sippymodem_huntgroup;

struct sippymodem_endpoint_config {
    const char *bound_address;
    const char *public_address;
    const char *identity_uri;
    const char *registrar_uri;
    const char *proxy_uri;
    const char *username;
    const char *password;
    const char *domain;
    const char *default_target;
    const char *request_user;
    const char *request_host;
    unsigned int listen_port;
    bool register_account;
};

void sippymodem_endpoint_config_init(struct sippymodem_endpoint_config *config);
struct sippymodem_endpoint *sippymodem_default_endpoint(struct sippymodem_server *server);
struct sippymodem_huntgroup *sippymodem_default_huntgroup(struct sippymodem_server *server);
struct sippymodem_huntgroup *sippymodem_huntgroup_create(struct sippymodem_server *server);
enum sippymodem_result sippymodem_huntgroup_add(struct sippymodem_huntgroup *group,
    struct sippymodem_line *line);
enum sippymodem_result sippymodem_huntgroup_remove(struct sippymodem_huntgroup *group,
    struct sippymodem_line *line);
enum sippymodem_result sippymodem_huntgroup_destroy(struct sippymodem_huntgroup *group);
struct sippymodem_endpoint *sippymodem_endpoint_create(struct sippymodem_server *server,
    struct sippymodem_huntgroup *group, const struct sippymodem_endpoint_config *config);
enum sippymodem_result sippymodem_endpoint_start(struct sippymodem_endpoint *endpoint);
enum sippymodem_result sippymodem_endpoint_stop(struct sippymodem_endpoint *endpoint);
enum sippymodem_result sippymodem_endpoint_destroy(struct sippymodem_endpoint *endpoint);
unsigned int sippymodem_endpoint_id(const struct sippymodem_endpoint *endpoint);
unsigned int sippymodem_endpoint_listen_port(const struct sippymodem_endpoint *endpoint);
enum sippymodem_result sippymodem_endpoint_get_status(const struct sippymodem_endpoint *endpoint,
    struct sippymodem_endpoint_status *status);
enum sippymodem_result sippymodem_dial_from(struct sippymodem_line *line,
    struct sippymodem_endpoint *endpoint, const char *target, uint64_t *generation);
```

Set server config `omit_default_endpoint` to create only explicit endpoints.
Without it, the existing server SIP config still creates the convenient default endpoint and an
automatic group containing every line. These default handles are borrowed and
cannot be destroyed separately. With it, only explicitly configured endpoints
receive calls; ordinary `sippymodem_dial()` returns `CLOSED`, so use `dial_from()`.

Configuration strings are copied during create, at most 255 bytes. NULL denotes
an empty optional setting. `request_user == NULL` derives the identity user or
username. `request_host == NULL` accepts host aliases on the owned transport,
including registered Contact/NAT/PBX rewrites. Explicit `"*"` accepts any user or
host; an explicit host makes matching strict. Policies match the initial INVITE
Request-URI on that endpoint's owned UDP transport. Overlapping policies on a
shared transport return `BUSY` at start. Unknown routes receive 404; a matched
endpoint with no eligible line receives 486.

Identical bind/public/listen-port settings share one owned transport, including
identical ephemeral-port settings (`listen_port == 0`). Different settings create
separate transports. The initial limits are eight endpoints, 32 groups, and 32
lines, including any default handles. One SDK server may be started per process;
all of its operations run on one native owner thread.

## One direct SIP endpoint and five lines

This setup fragment belongs inside the application initializer.
Check every pointer/result and use structured cleanup in production. On setup
failure, destroy any adapters, then destroy the server to unwind child handles.

```c
#include <sippymodem.h>
#include <stdlib.h>

struct sippymodem_server_config sc;
sippymodem_server_config_init(&sc);
sc.max_lines = 5;
sc.omit_default_endpoint = true;
struct sippymodem_server *server = sippymodem_server_create(&sc);
if (!(server)) abort();
struct sippymodem_huntgroup *pool = sippymodem_huntgroup_create(server);
if (!(pool)) abort();

struct sippymodem_line_config lc;
sippymodem_line_config_init(&lc);
struct sippymodem_line *lines[5];
for (unsigned int i = 0; i < 5; ++i) {
    lines[i] = sippymodem_line_create(server, &lc);
    if (!(lines[i])) abort();
    if (!(sippymodem_huntgroup_add(pool, lines[i]) == SIPPYMODEM_OK)) abort();
}
struct sippymodem_endpoint_config ec;
sippymodem_endpoint_config_init(&ec);
ec.identity_uri = "sip:pool@example.invalid";
ec.listen_port = 5060;
ec.request_user = "*"; /* Accept all called numbers on this direct endpoint. */
struct sippymodem_endpoint *endpoint = sippymodem_endpoint_create(server, pool, &ec);
if (!(endpoint)) abort();
if (!(sippymodem_server_start(server) == SIPPYMODEM_OK)) abort();
for (unsigned int i = 0; i < 5; ++i)
    if (!(sippymodem_line_start(lines[i]) == SIPPYMODEM_OK)) abort();
if (!(sippymodem_endpoint_start(endpoint) == SIPPYMODEM_OK)) abort();
/* Run the owner loop, accept INCOMING with answer(line, generation), then wait
 * for EVENT_CONNECTED/status.ready before creating the terminal fd adapter. */
```

The server selects the first eligible member in live server line-slot order,
which may change when a destroyed slot is reused. Selection does not rotate.
A reservation excludes a line from every pool it belongs to. Stopped lines,
active calls, retained RX, and pending old RTP cleanup cannot be selected.

## Two SIP endpoints sharing or separating pools

For two identities on one transport, replace the one endpoint setup above with:

```c
struct sippymodem_endpoint_config a, b;
sippymodem_endpoint_config_init(&a);
sippymodem_endpoint_config_init(&b);
a.identity_uri = "sip:first@example.invalid";
b.identity_uri = "sip:second@example.invalid";
a.listen_port = b.listen_port = 5060;
/* NULL request_user derives the distinct first/second identity keys. */
struct sippymodem_endpoint *first = sippymodem_endpoint_create(server, pool, &a);
struct sippymodem_endpoint *second = sippymodem_endpoint_create(server, pool, &b);
if (!(first && second)) abort();
```

Both share the five lines. Start both after server/line start. Stopping `first`
ends only calls admitted through `first`; calls through `second` keep running.

For isolated profiles, create `other_pool`, create its lines from a different
`struct sippymodem_line_config`, add them to `other_pool`, and create `second`
against `other_pool` instead. Increase `sc.max_lines` to cover both pools before
server creation. To give `second` a different direct UDP endpoint, use a different
`listen_port` or bind address. A full first pool never takes an other-pool line.

For registration, additionally supply endpoint `username`, `password`, `domain`,
`registrar_uri`, and `register_account = true`. Typed fields are separate; do not
put a combined username/password/domain login string into `username`.

Endpoint registration status is:

```c
enum sippymodem_registration_state {
    SIPPYMODEM_REG_DISABLED, SIPPYMODEM_REG_REGISTERING, SIPPYMODEM_REG_REGISTERED,
    SIPPYMODEM_REG_UNREGISTERING, SIPPYMODEM_REG_UNREGISTERED, SIPPYMODEM_REG_FAILED,
    SIPPYMODEM_REG_UNREGISTER_FAILED,
};
struct sippymodem_endpoint_status {
    unsigned int endpoint_id, listen_port;
    uint64_t state_sequence;
    bool started;
    enum sippymodem_registration_state registration;
    unsigned int registration_code;
    int library_status;
};
```

Each endpoint has a stable, monotonic native ID distinct from recycled SDK
account IDs. Private `struct sippymodem_call_facts` contains
`unsigned int endpoint_id`, `generation`, `called_number[128]`, and
`caller_number[128]`. Facts belong to that call generation. Keep telephone
and SIP-derived routing facts private; public status/events contain no SIP URIs
or credentials.

## DS0 56/64 per-call policy

Select raw digital operation explicitly with line config `modulation = 64` and
`automode = false`. Set rate bounds to `56000` or `64000`
bits per second; use matching `min_bps` and `max_bps` for a fixed rate. This mode
uses the digital bearer directly rather than negotiating an analog modulation.
The bearer must preserve exact G.711 octets. The 56-kbit path uses seven high
bits and tolerates robbed least-significant bits. See [DS0 operation](ds0.md).

During V.8 automode, DS0 recognition requires at least 1200 consecutive raw
G.711 codewords that parse cleanly as 56- or 64-kbit HDLC, including at least one
frame with a valid FCS. Idle alone never selects DS0. A passive peer that sends
no valid frame cannot establish an automatic DS0 connection. Proving frames are
retained across selection for the PPP or LAPM path, so recognition does not
discard the peer's startup exchange.

`min_bps = 56000, max_bps = 64000` hunts both rates. It starts at 64 kbit/s and
tries 56 kbit/s after eight nominal seconds; the first valid frame locks the
selection.

`ds0_lapm = false` is the default DS0 policy: synchronous PPP framing, with the
native byte/fd path supplying the asynchronous PPP stream expected by `pppd`.
It bypasses modem LAPM detection and modem compression. The usual required-EC
readiness check does not apply to this PPP mode. PPP readiness means the bearer
is ready; `pppd` still establishes the PPP session. A passive PPP peer may need
startup retries while the bearer hunts its rate.

`ds0_lapm = true` selects the shared LAPM/HDLC data path and requires LAPM rather
than falling back to raw asynchronous modem data. The selection belongs to each
line, so separate groups can offer different DS0 policies. Continue to use
`EVENT_CONNECTED`/`status.ready` and generation checks for both policies.
Configured compression remains available if the LAPM peer negotiates it.

## Native PPP fd integration

Use the native adapter after modem readiness. It works with lines in any group.

```c
#include <sippymodem_fd.h>
int peer_fd = -1;
struct sippymodem_fd *adapter = sippymodem_fd_create(line, generation, &peer_fd);
/* adapter borrows line. On success, the application owns peer_fd. Hand peer_fd
 * directly to pppd as its terminal fd; preserve it through spawn/pass_fds or
 * clear CLOEXEC for exec. Close the parent's copy after the child inherits it. */

/* In the one native owner loop, with monotonic now_ns: */
sippymodem_fd_process(adapter);
sippymodem_process(server, now_ns, &next_deadline_ns);
sippymodem_fd_process(adapter);
/* Poll the adapter's borrowed sippymodem_fd_poll_fd(adapter), the server event
 * fd, and application fds until the returned deadline. Handle all results. */
```

Create the adapter only for a ready DATA call. Its returned fd is nonblocking
and close-on-exec. Each pass pumps at most 4096 bytes each way using the literal
byte API, retaining partial counts. Python must not relay PPP bytes. Carrier
loss delivers retained RX before peer EOF; peer EOF hangs up only that generation.
An adapter tied to an old generation cannot terminate a replacement call.
Destroy the adapter before destroying its borrowed line. The caller separately
owns and closes the returned peer fd.

## Owner loop, generation, and shutdown rules

- Configure group membership before any attached endpoint starts. Add/remove
  returns `BUSY` while an attached endpoint runs or the member has an active call.
  Destroying a line removes it from all groups. Destroying a referenced group
  returns `BUSY`; destroy its explicit endpoints first.
- Start server, start lines (reserving RTP before SDP), then start explicit
  endpoints. Server start automatically starts only the default endpoint.
- Maintain your own event `line_id` to line-handle map. Answer/hangup/mode controls
  use the event/status generation; `STALE` has no effect. Compare private facts'
  generation before applying a route. `EVENT_SIP_ESTABLISHED` is not readiness;
  `EVENT_CONNECTED` and `status.ready` include modem and error-control readiness.
- Only `sippymodem_process(server, now_ns, &next_deadline_ns)` advances protocols.
  Establish the monotonic clock with one process pass before dialing. Backwards
  timestamps are rejected. Native processing is bounded and returns an absolute
  deadline, possibly now when work remains; it never waits for network replies.
- `send`/`receive` copy exact prefixes with no retained caller pointers. DATA bytes
  are literal, including NUL, XON/XOFF, `+++`, and AT text. Local TX counts exclude
  compressed/framed/unacknowledged bytes. RX remains available in COMMAND.
- Explicit COMMAND/DATA changes require no pending local TX. Carrier, RTP, DSP,
  error control, and retained RX continue in COMMAND. DATA resume does not retrain.
- Retained disconnect sequence/generation/cause/origin survive later polls/calls.
  Raw receive overflow retains its accepted prefix and deliberately ends the call;
  event overflow is sticky/fatal with reserved terminal delivery. Drain evidence
  and recreate the server after event overflow.
- Stop is idempotent, continues cleanup after failures, and does not wait for
  unregister/hangup network replies. Stop one line or endpoint independently.
  Destroy adapters first; destroy explicit endpoints before groups; destroy lines
  before a still-needed server. Server destroy frees remaining children and
  invalidates every child handle.

The initial shared server has one DSP owner loop. Earlier Pi capacity measurements
using separate modem processes across cores do not establish the same capacity
for heavy modes in this shared owner. A deliberate worker boundary is future work.
