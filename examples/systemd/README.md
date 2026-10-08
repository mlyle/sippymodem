# Run the PTY modem with systemd

This template supervises one foreground `slmodemd` per instance. It leaves call
handling to the PTY client. For a native server with huntgroups, supervise the
embedding application instead; the library does not create a daemon or a PTY.

Install sippymodem first. The template assumes `PREFIX=/usr/local`; adjust
`ExecStart` if the executable lives elsewhere. Run these setup commands from
this example directory, either in the source checkout or under the
installation's `share/sippymodem/examples/systemd`:

```sh
sudo useradd --system --create-home --home-dir /var/lib/sippymodem \
  --shell /usr/sbin/nologin sippymodem
sudo install -d -m 700 /etc/sippymodem
sudo install -m 600 default.env.example /etc/sippymodem/default.env
sudo install -m 644 sippymodem@.service /etc/systemd/system/
```

Reuse an existing service account if appropriate. Edit the private `.env` file
with the actual identity/account and ports. It is a systemd environment file,
not a shell script: use `NAME=value` assignments, no `export`, command
substitution or variable expansion. Keep credentials out of the unit and source
tree. If using a registered provider, set `SIP_REGISTER=1` and its account
settings; the supplied direct-SIP example performs no registration.

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now sippymodem@default.service
sudo journalctl -u sippymodem@default.service -f
```

The service writes its actual PTY path to `/run/sippymodem-default/pty` after
opening the modem. `Type=simple` reports the process started before this file
is necessarily populated; a PTY client should wait for a nonempty file and a
usable device. The directory is private to the service user and disappears
when the service stops. Each restart can assign a different PTY; read the path
again rather than retaining it across restarts.

For a manual console under the same user:

```sh
sudo -u sippymodem kermit -Y \
  -l "$(sudo cat /run/sippymodem-default/pty)" -b 115200
```

In Kermit, use `set carrier-watch off`, `set flow-control none`, then `connect`.
Issue `ATS0=1` to answer incoming calls, or let the attached BBS initialize the
modem and answer `RING` with `ATA`. `ATZ` resets `S0`, so initialize it again
after a reset. The slave PTY follows `grantpt`/devpts ownership and permissions;
run the attached BBS as the same Unix user. Access from another account requires
site-managed PTY permissions in addition to any shared group membership.
Set `SIP_AUTO_ANSWER=1` only when the application is ready for
immediate incoming-call acceptance. This service does not start a BBS or
`getty`; attach exactly one owner to the PTY.

Add another private instance file, such as `/etc/sippymodem/second.env`, and
start `sippymodem@second` for another independent modem. Give each instance
distinct SIP/RTP ports and a suitable SIP account/routing policy. To admit
several simultaneous calls through one registration, use the library's
endpoint and huntgroup API, documented in
`docs/terminal-server-integration.md` in the source checkout.

Normal diagnostics go to the journal; `SIP_TRACE=1` adds connection/training
information that may include numbers and network addresses. POSIX syslog is
also available through `MODEM_LOG_TARGET=syslog`. The published PTY path stays
separate from diagnostic logs. ExecStart uses a shell only to open that file as
the service user, then replaces itself with `slmodemd` through `exec`; the daemon
remains the supervised process. SIGTERM triggers the normal modem shutdown;
`Restart=on-failure` restarts failures without restarting an intentional stop.

`make install` copies these examples under `share/sippymodem/examples/systemd`.
It does not create the account, install an active unit or enable a service.
Validate a customized unit with `systemd-analyze verify` before enabling it.
