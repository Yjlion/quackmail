# Shared plumbing for quackcit.sh and quackcitadm.sh. Sourced, not executed.
#
# There is no Python here and none is required: the server *is* the bundled
# DuckDB CLI, holding the database open with every listener extension loaded.
# That single fact shapes everything below.
#
# Two install layouts are supported and detected automatically:
#
#   repo    a git checkout — deploy/ next to build/release/extension/<n>/<n>.duckdb_extension
#   bundle  an unpacked release — everything flat in one directory (/opt/quackmail)
#
# In a checkout the database, logs and runtime files default to repo-relative
# paths, so development is unchanged. In a bundle they default to FHS locations
# (/var/lib/quackcit, /var/log/quackcit, /run/quackcit); set QUACKCIT_STATE_DIR,
# QUACKCIT_LOG_DIR and QUACKCIT_RUN_DIR to run without root.

# Reply files and the control FIFO carry credentials and arbitrary SQL.
umask 077

QUACKCIT_HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
QUACKCIT_PARENT=$(dirname "$QUACKCIT_HERE")

if [ -f "$QUACKCIT_HERE/quackmail.duckdb_extension" ]; then
    QUACKCIT_LAYOUT=bundle
    QUACKCIT_HOME=$QUACKCIT_HERE
elif [ -f "$QUACKCIT_PARENT/extension_config.cmake" ]; then
    QUACKCIT_LAYOUT=repo
    QUACKCIT_HOME=$QUACKCIT_PARENT
else
    # Nothing recognisable; assume a bundle so the error messages point at the
    # directory the operator actually installed.
    QUACKCIT_LAYOUT=bundle
    QUACKCIT_HOME=$QUACKCIT_HERE
fi

# The config assigns with `: "${VAR:=value}"`, so anything already in the
# environment wins and a one-off override on the command line just works.
if [ -n "${QUACKCIT_CONF:-}" ]; then
    QUACKCIT_CONF_FILE=$QUACKCIT_CONF
elif [ -f "$QUACKCIT_HERE/quackcit.conf" ]; then
    QUACKCIT_CONF_FILE=$QUACKCIT_HERE/quackcit.conf
elif [ -f "$QUACKCIT_HOME/etc/quackcit.conf" ]; then
    QUACKCIT_CONF_FILE=$QUACKCIT_HOME/etc/quackcit.conf
else
    QUACKCIT_CONF_FILE=/etc/quackcit.conf
fi
if [ -f "$QUACKCIT_CONF_FILE" ]; then
    set -a
    # shellcheck disable=SC1090
    . "$QUACKCIT_CONF_FILE"
    set +a
fi

if [ "$QUACKCIT_LAYOUT" = repo ]; then
    : "${QUACKCIT_EXT_DIR:=$QUACKCIT_HOME/build/release/extension}"
    : "${QUACKCIT_DUCKDB:=$QUACKCIT_HOME/build/release/duckdb}"
    : "${QUACKCIT_STATE_DIR:=$QUACKCIT_HOME}"
    : "${QUACKCIT_LOG_DIR:=$QUACKCIT_HOME}"
    : "${QUACKCIT_RUN_DIR:=$QUACKCIT_HOME}"
else
    : "${QUACKCIT_EXT_DIR:=$QUACKCIT_HOME}"
    : "${QUACKCIT_DUCKDB:=$QUACKCIT_HOME/duckdb}"
    : "${QUACKCIT_STATE_DIR:=/var/lib/quackcit}"
    : "${QUACKCIT_LOG_DIR:=/var/log/quackcit}"
    : "${QUACKCIT_RUN_DIR:=/run/quackcit}"
fi

: "${QUACKCIT_DB:=$QUACKCIT_STATE_DIR/quackcit.duckdb}"
: "${QUACKCIT_LOG:=$QUACKCIT_LOG_DIR/quackcit.log}"
: "${QUACKCIT_ERRLOG:=$QUACKCIT_LOG_DIR/quackcit.err}"
: "${QUACKCIT_PIDFILE:=$QUACKCIT_RUN_DIR/quackcit.pid}"
: "${QUACKCIT_ADMIN_FIFO:=$QUACKCIT_RUN_DIR/control.fifo}"
: "${QUACKCIT_HOST:=127.0.0.1}"
: "${QUACKCIT_TLS_CERT:=}"
: "${QUACKCIT_TLS_KEY:=}"

# A DuckDB on PATH is the last resort; it must be the version the extensions
# were built against or LOAD refuses them.
if [ ! -x "$QUACKCIT_DUCKDB" ] && command -v duckdb >/dev/null 2>&1; then
    QUACKCIT_DUCKDB=$(command -v duckdb)
fi

PROG=$(basename "$0")

die() { echo "$PROG: $*" >&2; exit 1; }

# Sub-second polling where the shell has it; some minimal `sleep`s are integer
# only, in which case requests just settle a little more slowly.
if sleep 0.05 2>/dev/null; then
    QUACKCIT_TICK=0.05
    QUACKCIT_TICKS_PER_SEC=20
else
    QUACKCIT_TICK=1
    QUACKCIT_TICKS_PER_SEC=1
fi
tick() { sleep "$QUACKCIT_TICK"; }

# ---- paths -----------------------------------------------------------------

# ext_path <name> — the two layouts differ only here. The release tarball is
# flat; `make` emits <name>/<name>.duckdb_extension.
ext_path() {
    if [ -f "$QUACKCIT_EXT_DIR/$1.duckdb_extension" ]; then
        printf '%s' "$QUACKCIT_EXT_DIR/$1.duckdb_extension"
    elif [ -f "$QUACKCIT_EXT_DIR/$1/$1.duckdb_extension" ]; then
        printf '%s' "$QUACKCIT_EXT_DIR/$1/$1.duckdb_extension"
    else
        return 1
    fi
}

require_duckdb() {
    [ -x "$QUACKCIT_DUCKDB" ] ||
        die "no DuckDB CLI at $QUACKCIT_DUCKDB (set QUACKCIT_DUCKDB, or run make)"
}

# The DuckDB CLI's dot-commands take a bare word for a filename, so a runtime
# path containing whitespace would silently write somewhere else.
require_sane_run_dir() {
    case "$QUACKCIT_RUN_DIR" in
        *[[:space:]]*) die "QUACKCIT_RUN_DIR must not contain whitespace: $QUACKCIT_RUN_DIR" ;;
    esac
}

ensure_dirs() {
    for d in "$(dirname "$QUACKCIT_DB")" "$(dirname "$QUACKCIT_LOG")" "$QUACKCIT_RUN_DIR"; do
        mkdir -p "$d" 2>/dev/null ||
            die "cannot create $d — set QUACKCIT_STATE_DIR, QUACKCIT_LOG_DIR and QUACKCIT_RUN_DIR to a writable location (see quackcit.conf), or run as root"
    done
}

running_pid() {
    [ -f "$QUACKCIT_PIDFILE" ] || return 1
    _pid=$(cat "$QUACKCIT_PIDFILE" 2>/dev/null) || return 1
    [ -n "$_pid" ] || return 1
    # kill -0 tests for existence without signalling.
    kill -0 "$_pid" 2>/dev/null || return 1
    printf '%s' "$_pid"
}

# ---- SQL literals ----------------------------------------------------------
#
# Every value that reaches SQL comes from here. Doubling the quote is the whole
# of SQL string escaping — DuckDB does not process backslash escapes inside a
# single-quoted literal — and it is newline-agnostic, so Sieve scripts and whole
# .eml files pass through unharmed.

sql_str() {
    printf "'%s'" "$(printf '%s' "$1" | sed "s/'/''/g")"
}

# Numbers are interpolated bare, so they are the one thing that has to be
# checked. Always call this in a plain assignment — `n=$(sql_int "$1")` — never
# nested inside a larger string: `die` runs in the command substitution's
# subshell, and only an assignment propagates its failure to `set -e`.
sql_int() {
    case "${1#-}" in
        ''|*[!0-9]*) die "expected an integer, got '$1'" ;;
    esac
    printf '%s' "$1"
}

# The CLI needs a terminated statement; callers write SQL without one.
sql_terminate() {
    case "$(printf '%s' "$1" | sed -e 's/[[:space:]]*$//')" in
        *\;) printf '%s\n' "$1" ;;
        *)   printf '%s;\n' "$1" ;;
    esac
}

# ---- the listeners ---------------------------------------------------------
#
# key  extension  control-prefix  default-port  tls  bind-override
#
# Ports are the non-privileged dev ones, chosen so QuackCit can run beside a
# real Citadel server; the module, not the port, defines the service. LMTP
# performs no sender authentication and no spam filtering by design — anything
# that can reach it can inject mail — so it is pinned to loopback.
quackcit_services() {
    cat <<'EOF'
CITADEL      quackmail_citadel     cit                 5040   starttls  -
SMTP_IN      quackmail_smtp_in     qm_smtp_in          2525   starttls  -
LMTP         quackmail_smtp_in     qm_lmtp             2033   none      127.0.0.1
SUBMISSION   quackmail_smtp_out    qm_smtp_submission  2587   starttls  -
SMTPS        quackmail_smtp_out    qm_smtp_smtps       2465   implicit  -
POP3         quackmail_pop3        qm_pop3             1110   starttls  -
POP3S        quackmail_pop3        qm_pop3s            1995   implicit  -
IMAP         quackmail_imap        qm_imap             1143   starttls  -
MANAGESIEVE  quackmail_managesieve qm_managesieve      4190   starttls  -
TELNET       quackmail_telnet      qm_telnet           2300   none      -
TELNETS      quackmail_telnet      qm_telnets          2992   implicit  -
NNTP         quackmail_nntp        qm_nntp             1119   starttls  -
NNTPS        quackmail_nntp        qm_nntps            1563   implicit  -
XMPP         quackmail_xmpp        qm_xmpp             15222  starttls  -
XMPPS        quackmail_xmpp        qm_xmpps            15223  implicit  -
HTTP         quackmail_http        qm_http             8080   none      -
HTTPS        quackmail_http        qm_https            8443   implicit  -
EOF
}

env_true() {
    case "$(printf '%s' "$1" | tr 'ABCDEFGHIJKLMNOPQRSTUVWXYZ' 'abcdefghijklmnopqrstuvwxyz')" in
        1|true|yes|on) return 0 ;;
        *) return 1 ;;
    esac
}

# Emits "KEY EXTENSION PREFIX HOST PORT TLS" for each listener that should run.
# Bind address precedence: QUACKCIT_HOST_<KEY>, the per-service override,
# QUACKCIT_HOST.
quackcit_enabled_services() {
    quackcit_services | while read -r key extension prefix port tls hostov; do
        [ -n "$key" ] || continue
        eval "_enable=\${QUACKCIT_ENABLE_$key:-}"
        if [ -n "$_enable" ] && ! env_true "$_enable"; then
            continue
        fi
        eval "_port=\${QUACKCIT_PORT_$key:-}"
        if [ -n "$_port" ]; then port=$_port; fi
        eval "_host=\${QUACKCIT_HOST_$key:-}"
        if [ -n "$_host" ]; then
            host=$_host
        elif [ "$hostov" != "-" ]; then
            host=$hostov
        else
            host=$QUACKCIT_HOST
        fi
        printf '%s %s %s %s %s %s\n' "$key" "$extension" "$prefix" "$host" "$port" "$tls"
    done
}

# ---- the control channel ---------------------------------------------------
#
# DuckDB permits exactly one read-write process per database file, so while the
# server holds the database nothing else can open it. The server therefore reads
# its standard input from a FIFO for the life of the process, and that FIFO is
# the admin channel: a request is SQL wrapped in `.output` redirections, and the
# reply is the file it writes.
#
# The FIFO is mode 0600 and accepts arbitrary SQL — it is a root-equivalent
# control channel and must not live on a shared filesystem.
#
# Errors are the one thing `.output` cannot capture: the CLI prints them to
# standard error. The server's stderr therefore has its own file, and since no
# part of the C++ tree ever writes to stderr, the bytes appended across one
# request are exactly that request's error text.

QUACKCIT_ADM_SEQ=0
QUACKCIT_ADM_LOCK=

adm_unlock() {
    if [ -n "$QUACKCIT_ADM_LOCK" ]; then
        rm -rf "$QUACKCIT_ADM_LOCK"
        QUACKCIT_ADM_LOCK=
    fi
}
trap adm_unlock EXIT
trap 'adm_unlock; exit 130' INT
trap 'adm_unlock; exit 143' TERM

# mkdir is the portable atomic lock; flock is util-linux and not guaranteed.
adm_lock() {
    _lock=$QUACKCIT_RUN_DIR/adm.lock
    _waited=0
    while ! mkdir "$_lock" 2>/dev/null; do
        _holder=$(cat "$_lock/pid" 2>/dev/null || true)
        if [ -n "$_holder" ] && ! kill -0 "$_holder" 2>/dev/null; then
            # Left behind by a client that was killed mid-request.
            rm -rf "$_lock"
            continue
        fi
        _waited=$((_waited + 1))
        if [ "$_waited" -gt $((30 * QUACKCIT_TICKS_PER_SEC)) ]; then
            die "timed out waiting for the admin lock at $_lock"
        fi
        tick
    done
    echo $$ > "$_lock/pid"
    QUACKCIT_ADM_LOCK=$_lock
}

adm_mode_cmds() {
    case "$1" in
        tsv)  printf '.mode tabs\n.headers off\n' ;;
        json) printf '.mode json\n' ;;
        *)    printf '.mode duckbox\n.headers on\n' ;;
    esac
}

# adm_online <mode> <sql>
adm_online() {
    _mode=$1
    _sql=$2
    QUACKCIT_ADM_SEQ=$((QUACKCIT_ADM_SEQ + 1))
    _id=$$.$QUACKCIT_ADM_SEQ
    _rep=$QUACKCIT_RUN_DIR/rep.$_id
    _done=$QUACKCIT_RUN_DIR/done.$_id

    adm_lock
    rm -f "$_rep" "$_done"
    _before=$(errlog_size)
    {
        adm_mode_cmds "$_mode"
        printf '.output %s\n' "$_rep"
        sql_terminate "$_sql"
        # Switching output closes the reply file; the marker's arrival is what
        # tells the client the statement above has finished.
        printf '.output %s\nSELECT 1;\n.output\n' "$_done"
    } > "$QUACKCIT_ADMIN_FIFO"

    _waited=0
    while [ ! -e "$_done" ]; do
        if ! kill -0 "$QUACKCIT_SERVER_PID" 2>/dev/null; then
            adm_unlock
            die "the server exited while handling the request; see $QUACKCIT_ERRLOG"
        fi
        _waited=$((_waited + 1))
        if [ "$_waited" -gt $((30 * QUACKCIT_TICKS_PER_SEC)) ]; then
            adm_unlock
            die "timed out waiting for a reply on $QUACKCIT_ADMIN_FIFO"
        fi
        tick
    done

    _after=$(errlog_size)
    adm_unlock

    if [ "$_after" -gt "$_before" ]; then
        errlog_delta "$_before" "$_after" >&2
        rm -f "$_rep" "$_done"
        return 1
    fi
    cat "$_rep"
    rm -f "$_rep" "$_done"
}

errlog_size() {
    if [ -f "$QUACKCIT_ERRLOG" ]; then
        wc -c < "$QUACKCIT_ERRLOG" | tr -d ' '
    else
        echo 0
    fi
}

# errlog_delta <before> <after> — what the server wrote to stderr in between.
errlog_delta() {
    tail -c "+$(($1 + 1))" "$QUACKCIT_ERRLOG" 2>/dev/null | head -c "$(($2 - $1))"
}

# adm_offline <mode> <sql> — only possible while the server is stopped. Just the
# umbrella extension is loaded, so the qm_* functions resolve; protocol-specific
# ones (cit_room_add, qm_sieve_check) need a running server.
adm_offline() {
    require_duckdb
    _umbrella=$(ext_path quackmail) ||
        die "quackmail.duckdb_extension not found under $QUACKCIT_EXT_DIR (run make first)"
    if ! {
        adm_mode_cmds "$1"
        printf 'LOAD %s;\n' "$(sql_str "$_umbrella")"
        sql_terminate "$2"
    } | "$QUACKCIT_DUCKDB" -unsigned "$QUACKCIT_DB"; then
        echo "$PROG: the server holds a write lock on $QUACKCIT_DB while it runs;" >&2
        echo "        if it is up, remove a stale $QUACKCIT_PIDFILE so this reaches the control channel." >&2
        return 1
    fi
}

# adm_exec <mode> <sql> — the server when it is up, the file when it is not.
adm_exec() {
    require_sane_run_dir
    if QUACKCIT_SERVER_PID=$(running_pid); then
        adm_online "$1" "$2"
    else
        adm_offline "$1" "$2"
    fi
}
