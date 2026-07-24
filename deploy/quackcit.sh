#!/bin/sh
# Run the QuackCit server: start, stop, restart, status, foreground, logs.
#
# All settings come from quackcit.conf (override with QUACKCIT_CONF). Anything
# already in the environment wins over the config file, so one-off runs work:
#
#     QUACKCIT_PORT_SMTP_IN=25 deploy/quackcit.sh start
#
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(dirname "$HERE")

CONF=${QUACKCIT_CONF:-$HERE/quackcit.conf}
if [ -f "$CONF" ]; then
    # The config assigns with `: "${VAR:=value}"`, so anything already in the
    # environment wins and a one-off override on the command line just works.
    set -a
    # shellcheck disable=SC1090
    . "$CONF"
    set +a
fi

: "${QUACKCIT_DB:=$REPO/quackcit.duckdb}"
: "${QUACKCIT_EXT_DIR:=$REPO/build/release/extension}"
: "${QUACKCIT_HOST:=127.0.0.1}"
: "${QUACKCIT_LOG:=$REPO/quackcit.log}"
: "${QUACKCIT_PIDFILE:=$REPO/quackcit.pid}"
: "${QUACKCIT_ADMIN_SOCK:=$QUACKCIT_DB.admin.sock}"
: "${PYTHON:=python3}"

export QUACKCIT_DB QUACKCIT_EXT_DIR QUACKCIT_HOST QUACKCIT_ADMIN_SOCK

LAUNCHER=$HERE/run_quackcit.py

die() { echo "quackcit: $*" >&2; exit 1; }

running_pid() {
    [ -f "$QUACKCIT_PIDFILE" ] || return 1
    pid=$(cat "$QUACKCIT_PIDFILE" 2>/dev/null) || return 1
    [ -n "$pid" ] || return 1
    # kill -0 tests for existence without signalling.
    kill -0 "$pid" 2>/dev/null || return 1
    echo "$pid"
}

cmd_start() {
    if pid=$(running_pid); then
        die "already running (pid $pid)"
    fi
    [ -d "$QUACKCIT_EXT_DIR" ] || die "extension directory not found: $QUACKCIT_EXT_DIR (run make first)"
    mkdir -p "$(dirname "$QUACKCIT_DB")" "$(dirname "$QUACKCIT_LOG")"

    # The launcher traps SIGTERM and shuts each listener down cleanly.
    nohup "$PYTHON" "$LAUNCHER" >>"$QUACKCIT_LOG" 2>&1 &
    pid=$!
    echo "$pid" > "$QUACKCIT_PIDFILE"

    # Give it a moment to bind, then confirm it survived.
    sleep 2
    if ! kill -0 "$pid" 2>/dev/null; then
        rm -f "$QUACKCIT_PIDFILE"
        echo "quackcit: failed to start; last log lines:" >&2
        tail -n 20 "$QUACKCIT_LOG" >&2 || true
        exit 1
    fi
    echo "quackcit: started (pid $pid), db=$QUACKCIT_DB, log=$QUACKCIT_LOG"
}

cmd_stop() {
    pid=$(running_pid) || { echo "quackcit: not running"; return 0; }
    kill -TERM "$pid"
    # Listeners close their sockets on the way out; wait for the process itself.
    i=0
    while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 30 ]; do
        sleep 1
        i=$((i + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
        echo "quackcit: did not stop after 30s, sending SIGKILL" >&2
        kill -KILL "$pid" 2>/dev/null || true
    fi
    rm -f "$QUACKCIT_PIDFILE"
    echo "quackcit: stopped"
}

cmd_status() {
    if pid=$(running_pid); then
        echo "quackcit: running (pid $pid)"
        echo "  db:      $QUACKCIT_DB"
        echo "  log:     $QUACKCIT_LOG"
        echo "  admin:   $QUACKCIT_ADMIN_SOCK"
        # The server owns the DB file, so ask it through the admin channel.
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
    exec "$PYTHON" "$LAUNCHER"
}

cmd_logs() {
    [ -f "$QUACKCIT_LOG" ] || die "no log file at $QUACKCIT_LOG"
    tail -n "${1:-50}" -f "$QUACKCIT_LOG"
}

usage() {
    cat <<EOF
usage: quackcit.sh <command>

  start        run the server in the background (PID file: $QUACKCIT_PIDFILE)
  stop         signal it to shut down and wait for it
  restart      stop, then start
  status       report whether it is up, and what it is serving
  foreground   run in this terminal (Ctrl-C to stop)
  logs [N]     follow the last N lines of $QUACKCIT_LOG

Configuration is read from $CONF; environment variables win over it.
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
