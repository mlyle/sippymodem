// SPDX-License-Identifier: BSD-3-Clause
#ifndef SIPPYMODEM_H
#define SIPPYMODEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sippymodem_server;
struct sippymodem_line;
struct sippymodem_endpoint;
struct sippymodem_huntgroup;

enum sippymodem_result {
	SIPPYMODEM_OK,
	SIPPYMODEM_AGAIN,
	SIPPYMODEM_CLOSED,
	SIPPYMODEM_INVALID,
	SIPPYMODEM_BUSY,
	SIPPYMODEM_STALE,
	SIPPYMODEM_OVERFLOW,
	SIPPYMODEM_ERROR,
};

enum sippymodem_dte_mode { SIPPYMODEM_COMMAND, SIPPYMODEM_DATA };
enum sippymodem_phase {
	SIPPYMODEM_IDLE, SIPPYMODEM_RINGING, SIPPYMODEM_DIALING,
	SIPPYMODEM_TRAINING, SIPPYMODEM_ONLINE, SIPPYMODEM_DISCONNECTING,
};
enum sippymodem_error_control { SIPPYMODEM_RAW, SIPPYMODEM_LAPM };
enum sippymodem_ec_policy {
	SIPPYMODEM_EC_AUTOMATIC, SIPPYMODEM_EC_RAW, SIPPYMODEM_EC_REQUIRED,
};
enum sippymodem_sip_state {
	SIPPYMODEM_SIP_IDLE, SIPPYMODEM_SIP_DIALING, SIPPYMODEM_SIP_INCOMING,
	SIPPYMODEM_SIP_EARLY, SIPPYMODEM_SIP_CONNECTED,
};
enum sippymodem_compression {
	SIPPYMODEM_COMP_NONE, SIPPYMODEM_COMP_V42BIS, SIPPYMODEM_COMP_V44,
};
enum sippymodem_disconnect_cause {
	SIPPYMODEM_DISCONNECT_NONE, SIPPYMODEM_DISCONNECT_LOCAL,
	SIPPYMODEM_DISCONNECT_REMOTE, SIPPYMODEM_DISCONNECT_MODEM,
	SIPPYMODEM_DISCONNECT_MEDIA, SIPPYMODEM_DISCONNECT_RX_OVERFLOW,
	SIPPYMODEM_DISCONNECT_SHUTDOWN, SIPPYMODEM_DISCONNECT_EVENTS,
	SIPPYMODEM_DISCONNECT_TIMEOUT,
};
enum sippymodem_event_type {
	SIPPYMODEM_EVENT_INCOMING, SIPPYMODEM_EVENT_SIP_ESTABLISHED,
	SIPPYMODEM_EVENT_CONNECTED, SIPPYMODEM_EVENT_STATE,
	SIPPYMODEM_EVENT_DISCONNECTED,
};

/* Strings are copied during create; settings longer than 255 bytes are rejected.
 * NULL means an empty optional setting.
 * Exactly one PJSUA server may be started per process. All operations for that
 * server belong to one native owner thread; the library never calls Python. */
struct sippymodem_server_config {
	const char *bound_address;
	const char *public_address;
	const char *identity_uri;
	const char *registrar_uri;
	const char *proxy_uri;
	const char *username;
	const char *password;
	const char *domain;
	const char *default_target;
	unsigned int listen_port;
	unsigned int max_lines;
	bool register_account;
	/* Explicit endpoint configurations can omit the legacy default endpoint. */
	bool omit_default_endpoint;
};

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

/* SIP accounts and transports are separate from modem settings. Strings are
 * copied, at most 255 bytes. Up to eight endpoints, including the default,
 * belong to one server.
 * request_user/host match the initial INVITE Request-URI on this endpoint's
 * owned UDP transport. "*" explicitly accepts any value. NULL user derives
 * the canonical identity user; NULL host accepts any host on the owned transport (needed
 * for registered Contact/NAT aliases). Overlapping policies are rejected. */
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

struct sippymodem_line_config {
	/* Hayes +MS modulation number; zero selects factory automatic negotiation. */
	unsigned int modulation;
	bool automode;
	unsigned int min_bps;
	unsigned int max_bps;
	unsigned int digital_role; /* +V90D: 0 analog, 1 digital, 2 automatic. */
	bool pcm_upstream;
	bool ds0_lapm; /* +MS=64: false PPP conversion; true LAPM/HDLC. */
	bool v91_enabled;
	/* AUTOMATIC offers LAPM with raw fallback; REQUIRED never falls back. */
	enum sippymodem_ec_policy ec_policy;
	unsigned int compression_types; /* bit 0 V.42bis; bit 1 V.44 */
	size_t tx_capacity;
	size_t rx_capacity;
	unsigned int rtp_port;
	unsigned int frame_ms;
	/* Remote clock: maximum hold for a real RTP gap; local clock: prefill. */
	unsigned int jitter_ms;
	/* Whole call attempt including early SIP media; 1000..600000, default 60000. */
	unsigned int call_timeout_ms;
	bool remote_clock;
	bool alaw;
	const char *recording_prefix; /* Optional private recording; copied at create. */
};

struct sippymodem_status {
	unsigned int line_id; /* Unique for this server lifetime, including destroyed slots. */
	uint64_t generation;
	uint64_t state_sequence;
	uint64_t disconnect_sequence;
	uint64_t disconnect_generation;
	enum sippymodem_disconnect_cause disconnect_cause;
	bool disconnect_local;
	char disconnect_reason[128]; /* Fixed native cause text; retained across calls. */
	enum sippymodem_phase phase;
	enum sippymodem_dte_mode dte_mode;
	enum sippymodem_sip_state sip_state;
	unsigned int rtp_port; /* Reserved before this line can accept or originate SDP. */
	bool started;
	bool off_hook;
	bool carrier;
	bool ready;
	bool rx_overflow;
	/* Negotiated Hayes +MS number; zero means not yet known. */
	unsigned int modulation;
	unsigned int tx_bps;
	unsigned int rx_bps;
	double tx_baud;
	double rx_baud;
	enum sippymodem_error_control error_control;
	enum sippymodem_compression compression;
	size_t tx_pending;
	size_t tx_free;
	size_t rx_ready;
};

/* Private call facts. Request-URI user is the called-number policy;
 * absent, invalid, or oversized users remain empty rather than truncating.
 * Keep these strings out of public logs. The AT compatibility frontend may
 * display the caller number alongside its incoming result text. */
struct sippymodem_call_facts {
	uint64_t generation;
	unsigned int endpoint_id; /* Stable native owner ID, never a recycled SIP account ID. */
	char called_number[128];
	/* Incoming telephone From user normalized to digits and optional '+'. Withheld, invalid,
	 * or oversized identities and every outgoing attempt remain empty. */
	char caller_number[128];
};

enum sippymodem_log_level {
	SIPPYMODEM_LOG_DEBUG, SIPPYMODEM_LOG_INFO,
	SIPPYMODEM_LOG_WARNING, SIPPYMODEM_LOG_ERROR,
};
enum { SIPPYMODEM_LOG_CAPACITY = 64, SIPPYMODEM_LOG_TEXT_MAX = 256 };

/* Copied diagnostic records; no payload, phone numbers or SIP credentials.
 * Sequence belongs to the line lifetime, generation identifies the call.
 * time_ns uses process()'s monotonic clock; zero means no clock supplied yet. */
struct sippymodem_log_record {
	uint64_t sequence, time_ns, generation;
	unsigned int line_id;
	enum sippymodem_log_level level;
	bool truncated;
	char text[SIPPYMODEM_LOG_TEXT_MAX];
};

struct sippymodem_event {
	enum sippymodem_event_type type;
	unsigned int line_id;
	struct sippymodem_status status;
};

void sippymodem_server_config_init(struct sippymodem_server_config *config);
/* Queue defaults are 4096 TX / 16384 RX; accepted capacities are 16..1048576.
 * Default is automatic V.92, V.34 upstream, V.91 enabled, LAPM with raw fallback,
 * both compression types, PCMU, 20 ms packets and a 60 ms gap hold. Remote
 * receive clock is enabled without fixed playout prefill. */
void sippymodem_line_config_init(struct sippymodem_line_config *config);
struct sippymodem_server *sippymodem_server_create(
	const struct sippymodem_server_config *config);
enum sippymodem_result sippymodem_server_start(struct sippymodem_server *server);
enum sippymodem_result sippymodem_server_stop(struct sippymodem_server *server);
/* Destroy stops the server and frees remaining lines, endpoints and groups;
 * all child handles become invalid. Destroy a line first when its server must
 * remain usable. */
void sippymodem_server_destroy(struct sippymodem_server *server);
/* Default handles are borrowed and owned by the server; they cannot be
 * destroyed separately. Its default huntgroup automatically contains all lines.
 * Explicit groups are configured before their endpoints start. A line may
 * belong to several groups; one active reservation always excludes it from all. */
struct sippymodem_endpoint *sippymodem_default_endpoint(struct sippymodem_server *server);
struct sippymodem_huntgroup *sippymodem_default_huntgroup(struct sippymodem_server *server);
void sippymodem_endpoint_config_init(struct sippymodem_endpoint_config *config);
struct sippymodem_huntgroup *sippymodem_huntgroup_create(struct sippymodem_server *server);
enum sippymodem_result sippymodem_huntgroup_add(struct sippymodem_huntgroup *group,
	struct sippymodem_line *line);
enum sippymodem_result sippymodem_huntgroup_remove(struct sippymodem_huntgroup *group,
	struct sippymodem_line *line);
enum sippymodem_result sippymodem_huntgroup_destroy(struct sippymodem_huntgroup *group);
struct sippymodem_endpoint *sippymodem_endpoint_create(struct sippymodem_server *server,
	struct sippymodem_huntgroup *group, const struct sippymodem_endpoint_config *config);
/* Start requires a started server; server_start starts only the default endpoint.
 * Stop terminates only calls selected through this endpoint, including when
 * another endpoint shares its group. All operations use the owner thread. */
enum sippymodem_result sippymodem_endpoint_start(struct sippymodem_endpoint *endpoint);
enum sippymodem_result sippymodem_endpoint_stop(struct sippymodem_endpoint *endpoint);
enum sippymodem_result sippymodem_endpoint_destroy(struct sippymodem_endpoint *endpoint);
unsigned int sippymodem_endpoint_id(const struct sippymodem_endpoint *endpoint);
unsigned int sippymodem_endpoint_listen_port(const struct sippymodem_endpoint *endpoint);
enum sippymodem_result sippymodem_endpoint_get_status(const struct sippymodem_endpoint *endpoint,
	struct sippymodem_endpoint_status *status);

struct sippymodem_line *sippymodem_line_create(struct sippymodem_server *server,
	const struct sippymodem_line_config *config);
enum sippymodem_result sippymodem_line_start(struct sippymodem_line *line);
enum sippymodem_result sippymodem_line_stop(struct sippymodem_line *line);
void sippymodem_line_destroy(struct sippymodem_line *line);

/* Nonblocking copies with exact prefix counts; caller pointers are never
 * retained. tx_pending/free count unframed local DTE bytes. receive drains
 * retained RX after disconnect before returning CLOSED. DATA is always literal,
 * including NUL, +++ and AT text. COMMAND rejects sends and preserves RX. */
enum sippymodem_result sippymodem_send(struct sippymodem_line *line,
	const void *bytes, size_t length, size_t *accepted);
enum sippymodem_result sippymodem_receive(struct sippymodem_line *line,
	void *bytes, size_t capacity, size_t *received);
/* Finish all accepted TX for this generation without hanging up. After READY,
 * the first call seals further nonempty sends (AGAIN, accepted=0) until a new
 * call starts. AGAIN requires continued process(); OK means LAPM payload has
 * been acknowledged, or a raw encoder/filter tail has been emitted. Raw cannot
 * promise remote application receipt. RX, carrier and COMMAND remain active.
 * Before READY, AGAIN has no effect. CLOSED/STALE leave other calls untouched.
 * ERROR means completion cannot be established; use hangup for cancellation.
 * Completion is idempotent. The application owns its bounded timeout. */
enum sippymodem_result sippymodem_finish_tx(struct sippymodem_line *line,
	uint64_t generation);
enum sippymodem_result sippymodem_get_status(const struct sippymodem_line *line,
	struct sippymodem_status *status);
/* Non-destructive, nonblocking owner-thread read of the line's latest 64 logs.
 * Start cursor at zero; it advances to the last copied sequence. Independent
 * readers use independent cursors. missed reports overwritten records since
 * that cursor. OK may include missed > 0; AGAIN means no newer records.
 * Future cursors return STALE. Capacity zero is a no-op; logs survive stop and
 * hangup until line destruction. Log overwrite never affects a call or DTE I/O. */
enum sippymodem_result sippymodem_read_log(const struct sippymodem_line *line,
	uint64_t *cursor, struct sippymodem_log_record *records, size_t capacity,
	size_t *count, uint64_t *missed);
enum sippymodem_result sippymodem_get_call_facts(const struct sippymodem_line *line,
	struct sippymodem_call_facts *facts);

/* Controls carry the generation read from status; STALE has no effect. Mode
 * changes require tx_pending == 0; DATA resume never requests retraining. */
enum sippymodem_result sippymodem_set_dte_mode(struct sippymodem_line *line,
	uint64_t generation, enum sippymodem_dte_mode mode);
/* Call process() once to establish the monotonic clock before dialing.
 * BUSY also covers retained RX and pending bounded cleanup of old RTP. */
enum sippymodem_result sippymodem_dial(struct sippymodem_line *line,
	const char *target, uint64_t *generation);
enum sippymodem_result sippymodem_dial_from(struct sippymodem_line *line,
	struct sippymodem_endpoint *endpoint, const char *target, uint64_t *generation);
enum sippymodem_result sippymodem_answer(struct sippymodem_line *line,
	uint64_t generation);
enum sippymodem_result sippymodem_hangup(struct sippymodem_line *line,
	uint64_t generation);

/* Only process advances protocols; now_ns is monotonic and must not move
 * backwards. Each pass polls SIP once, drains RTP in batches of at most 64
 * packets, and processes at most four audio frames per line; it never waits for
 * the network. The returned absolute deadline may equal now_ns when work remains. */
enum sippymodem_result sippymodem_process(struct sippymodem_server *server,
	uint64_t now_ns, uint64_t *next_deadline_ns);
/* Event overflow is fatal and sticky: active calls close, terminal events have
 * reserved delivery, and disconnect status remains readable. Drain evidence
 * and recreate the server to recover. */
enum sippymodem_result sippymodem_poll_event(struct sippymodem_server *server,
	struct sippymodem_event *event);
unsigned int sippymodem_listen_port(const struct sippymodem_server *server);
int sippymodem_event_fd(const struct sippymodem_server *server);

#ifdef __cplusplus
}
#endif
#endif
