#!/bin/sh
# Run the QuackCit server: start, stop, restart, status, foreground, logs.
#
# The server is the bundled DuckDB CLI holding the database open with every
# listener extension loaded; no Python, and no DuckDB install beyond the one in
# this directory. Its standard input is a FIFO, which is both what keeps the
# process alive and the channel quackcitadm.sh administers it through.
#
# All settings come from quackcit.conf (override with QUACKCIT_CONF). Anything
# already in the environment wins over the config file, so one-off runs work:
#
#     QUACKCIT_PORT_SMTP_IN=25 deploy/quackcit.sh start
#
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=quackcit_common.sh
. "$HERE/quackcit_common.sh"

STARTUP_SQL=$QUACKCIT_RUN_DIR/startup.sql
READY=$QUACKCIT_RUN_DIR/ready

# Users seeded on first run. Mirrors the reference Citadel box (admin/admin at
# aide level, leo/leo as a regular user) so protocol output can be diffed 1:1.
SEED_USERS='admin admin 6
leo leo 4'

# ---------------------------------------------------------------------------

# One CALL that brings a listener up, with TLS wired if it is configured.
# Returns 1 for an implicit-TLS listener with no certificate — it cannot work,
# so it is skipped rather than started in the clear.
start_call() {
    _prefix=$1; _host=$2; _port=$3; _tls=$4
    _checked_port=$(sql_int "$_port")
    _args="$(sql_str "$_host"), $_checked_port"
    if [ -n "$QUACKCIT_TLS_CERT" ] && [ -n "$QUACKCIT_TLS_KEY" ]; then
        _args="$_args, tls_cert => $(sql_str "$QUACKCIT_TLS_CERT")"
        _args="$_args, tls_key => $(sql_str "$QUACKCIT_TLS_KEY")"
        case "$_tls" in
            starttls) _args="$_args, starttls => true" ;;
            implicit) _args="$_args, implicit_tls => true" ;;
        esac
    else
        # Without a certificate STARTTLS cannot be offered; the listener still
        # runs in the clear, which is what a dev box wants.
        case "$_tls" in
            implicit) return 1 ;;
        esac
    fi
    printf 'CALL %s_start(%s);\n' "$_prefix" "$_args"
}

gen_startup_sql() {
    printf '.bail off\n.mode duckbox\n'

    # The umbrella owns schema init and the qm_* administration functions, so it
    # loads first; the rest are deduplicated because several listeners share one
    # extension (POP3/POP3S, HTTP/HTTPS, ...).
    _umbrella=$(ext_path quackmail) ||
        die "quackmail.duckdb_extension not found under $QUACKCIT_EXT_DIR (run make first)"
    printf 'LOAD %s;\n' "$(sql_str "$_umbrella")"
    _loaded=' quackmail '
    while read -r _key _extension _prefix _host _port _tls; do
        case "$_loaded" in
            *" $_extension "*) continue ;;
        esac
        _path=$(ext_path "$_extension") ||
            die "$_extension.duckdb_extension not found under $QUACKCIT_EXT_DIR (run make first)"
        printf 'LOAD %s;\n' "$(sql_str "$_path")"
        _loaded="$_loaded$_extension "
    done <<EOF
$(quackcit_enabled_services)
EOF

    # Each _start returns a row with host, port and a note, so the log records
    # what came up and what failed to bind.
    while read -r _key _extension _prefix _host _port _tls; do
        if ! start_call "$_prefix" "$_host" "$_port" "$_tls"; then
            echo "quackcit: skipped $_key — implicit TLS needs QUACKCIT_TLS_CERT and QUACKCIT_TLS_KEY" >&2
        fi
    done <<EOF
$(quackcit_enabled_services)
EOF

    printf 'SELECT * FROM qm_status();\n'

    # Exercise every dot-command a request will use. A CLI that does not know
    # one of them writes to stderr, and `start` reports that once here — rather
    # than each request afterwards mistaking the same complaint for its own
    # error, since stderr is how request errors are recovered.
    printf '.mode tabs\n.headers off\n.mode json\n.mode duckbox\n.headers on\n'

    # Written last: its appearance is what tells `start` the server is up.
    printf '.output %s\nSELECT 1;\n.output\n' "$READY"
}

gen_shutdown_sql() {
    while read -r _key _extension _prefix _host _port _tls; do
        printf 'CALL %s_stop();\n' "$_prefix"
    done <<EOF
$(quackcit_enabled_services)
EOF
    # Closes the database cleanly, which a signal would not do.
    printf '.quit\n'
}

prepare_runtime() {
    require_duckdb
    require_sane_run_dir
    ensure_dirs
    [ -n "$(quackcit_enabled_services)" ] || die "no services enabled; nothing to do"
    gen_startup_sql > "$STARTUP_SQL"
    rm -f "$READY" "$QUACKCIT_ADMIN_FIFO"
    mkfifo -m 600 "$QUACKCIT_ADMIN_FIFO"
    printf '=== quackcit starting %s ===\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" >> "$QUACKCIT_LOG"
}

wait_ready() {
    _pid=$1
    _waited=0
    while [ ! -e "$READY" ]; do
        if ! kill -0 "$_pid" 2>/dev/null; then
            return 1
        fi
        _waited=$((_waited + 1))
        if [ "$_waited" -gt $((60 * QUACKCIT_TICKS_PER_SEC)) ]; then
            return 1
        fi
        tick
    done
    return 0
}

# The listener extensions provide no idempotent seed, and a table function's
# side effect happens when it is bound, so a guarding WHERE would not stop
# qm_user_add from resetting the password on every restart. Ask first instead.
seed_users() {
    _count=$(adm_exec tsv "SELECT count(*) FROM quackmail_users" 2>/dev/null | tr -d ' \t\r') || return 0
    [ "$_count" = 0 ] || return 0
    printf '%s\n' "$SEED_USERS" | while read -r _name _pw _axlevel; do
        [ -n "$_name" ] || continue
        _ax=$(sql_int "$_axlevel")
        adm_exec tsv "SELECT ok, note FROM qm_user_add($(sql_str "$_name"), $(sql_str "$_pw"))" >/dev/null
        # Provision the Citadel side too, and set the access level.
        adm_exec tsv "INSERT INTO citadel_users (username, usernum, axlevel)
                      VALUES ($(sql_str "$_name"), nextval('citadel_user_seq'), $_ax)
                      ON CONFLICT (username) DO UPDATE SET axlevel = excluded.axlevel" >/dev/null
    done
    echo "quackcit: seeded users: $(printf '%s\n' "$SEED_USERS" | while read -r n _rest; do printf '%s ' "$n"; done)"
}

cmd_start() {
    if pid=$(running_pid); then
        die "already running (pid $pid)"
    fi
    prepare_runtime
    err_before=$(errlog_size)

    # Opening the FIFO read-write (0<>) makes the CLI a writer on its own input,
    # so the read never sees end-of-file and the process idles there for its
    # whole life. That is the entire supervisor loop the Python launcher was.
    nohup "$QUACKCIT_DUCKDB" -unsigned -init "$STARTUP_SQL" "$QUACKCIT_DB" \
        >>"$QUACKCIT_LOG" 2>>"$QUACKCIT_ERRLOG" 0<>"$QUACKCIT_ADMIN_FIFO" &
    pid=$!
    echo "$pid" > "$QUACKCIT_PIDFILE"

    if ! wait_ready "$pid"; then
        kill -KILL "$pid" 2>/dev/null || true
        rm -f "$QUACKCIT_PIDFILE"
        echo "quackcit: failed to start; last log lines:" >&2
        tail -n 20 "$QUACKCIT_LOG" >&2 2>/dev/null || true
        tail -n 20 "$QUACKCIT_ERRLOG" >&2 2>/dev/null || true
        exit 1
    fi

    # `.bail off` keeps the CLI alive through a bad statement, so startup can
    # reach the ready marker with a LOAD or a dot-command having failed. Those
    # go to stderr and would otherwise pass unnoticed.
    err_after=$(errlog_size)
    if [ "$err_after" -gt "$err_before" ]; then
        echo "quackcit: errors during startup:" >&2
        errlog_delta "$err_before" "$err_after" >&2
    fi

    seed_users
    echo "quackcit: started (pid $pid), db=$QUACKCIT_DB, log=$QUACKCIT_LOG"
    # Site policy starts empty, which means: only c_fqdn is accepted as a local
    # domain, no DNSBL is queried, and no outbound mail is DKIM-signed.
    echo "quackcit: policy tables are empty by default — see quackcitadm.sh"
}

cmd_stop() {
    pid=$(running_pid) || { echo "quackcit: not running"; return 0; }

    # Ask it to close down over the control channel, in a subshell so a server
    # that has already died cannot block us on the FIFO open forever.
    if [ -p "$QUACKCIT_ADMIN_FIFO" ]; then
        gen_shutdown_sql > "$QUACKCIT_RUN_DIR/shutdown.sql" 2>/dev/null || true
        (cat "$QUACKCIT_RUN_DIR/shutdown.sql" > "$QUACKCIT_ADMIN_FIFO" 2>/dev/null) &
        writer=$!
        i=0
        while kill -0 "$writer" 2>/dev/null && [ "$i" -lt $((5 * QUACKCIT_TICKS_PER_SEC)) ]; do
            tick
            i=$((i + 1))
        done
        kill -KILL "$writer" 2>/dev/null || true
    fi

    i=0
    while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 15 ]; do sleep 1; i=$((i + 1)); done
    if kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid"
        i=0
        while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 15 ]; do sleep 1; i=$((i + 1)); done
    fi
    if kill -0 "$pid" 2>/dev/null; then
        echo "quackcit: did not stop after 30s, sending SIGKILL" >&2
        kill -KILL "$pid" 2>/dev/null || true
    fi
    rm -f "$QUACKCIT_PIDFILE" "$QUACKCIT_ADMIN_FIFO" "$READY" "$QUACKCIT_RUN_DIR/shutdown.sql"
    echo "quackcit: stopped"
}

cmd_status() {
    if pid=$(running_pid); then
        echo "quackcit: running (pid $pid)"
        echo "  layout:  $QUACKCIT_LAYOUT ($QUACKCIT_HOME)"
        echo "  duckdb:  $QUACKCIT_DUCKDB"
        echo "  db:      $QUACKCIT_DB"
        echo "  log:     $QUACKCIT_LOG"
        echo "  errors:  $QUACKCIT_ERRLOG"
        echo "  control: $QUACKCIT_ADMIN_FIFO"
        # The server owns the DB file, so ask it through the control channel.
        "$HERE/quackcitadm.sh" status 2>/dev/null || true
        return 0
    fi
    echo "quackcit: not running"
    return 1
}

cmd_foreground() {
    if pid=$(running_pid); then
        die "already running in the background (pid $pid)"
    fi
    prepare_runtime
    echo $$ > "$QUACKCIT_PIDFILE"
    # Errors still go to their own file: that is how quackcitadm.sh reports the
    # error text of a failed request back to whoever asked.
    echo "quackcit: running in the foreground; SQL errors go to $QUACKCIT_ERRLOG"
    exec "$QUACKCIT_DUCKDB" -unsigned -init "$STARTUP_SQL" "$QUACKCIT_DB" \
        2>>"$QUACKCIT_ERRLOG" 0<>"$QUACKCIT_ADMIN_FIFO"
}

cmd_logs() {
    [ -f "$QUACKCIT_LOG" ] || die "no log file at $QUACKCIT_LOG"
    if [ -f "$QUACKCIT_ERRLOG" ]; then
        tail -n "${1:-50}" -f "$QUACKCIT_LOG" "$QUACKCIT_ERRLOG"
    else
        tail -n "${1:-50}" -f "$QUACKCIT_LOG"
    fi
}

usage() {
    cat <<EOF
usage: quackcit.sh <command>

  start        run the server in the background (PID file: $QUACKCIT_PIDFILE)
  stop         close it down cleanly and wait for it
  restart      stop, then start
  status       report whether it is up, and what it is serving
  foreground   run in this terminal (Ctrl-C to stop)
  logs [N]     follow the last N lines of the log and error log

Install layout: $QUACKCIT_LAYOUT ($QUACKCIT_HOME)
Configuration is read from $QUACKCIT_CONF_FILE; environment variables win over it.
Use quackcitadm.sh to administer a running server.
EOF
}

case "${1:-}" in
    start)      cmd_start ;;
    stop)       cmd_stop ;;
    restart)    cmd_stop; cmd_start ;;
    status)     cmd_status ;;
    foreground) cmd_foreground ;;
    logs)       shift; cmd_logs "${1:-50}" ;;
    ""|-h|--help|help) usage ;;
    *)          usage; exit 2 ;;
esac
