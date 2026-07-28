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
LOG_FIFO=$QUACKCIT_RUN_DIR/log.fifo

# The accounts to create on first run are QUACKCIT_SEED_USERS; see quackcit.conf.

# ---------------------------------------------------------------------------

# This machine's own name, as good as we can get it. Used both for the subject
# of a generated certificate and, on a fresh database, for the site's c_fqdn —
# so the two agree instead of the certificate naming the real host while c_fqdn
# still says quackmail.test.
host_fqdn() {
    # `hostname -f` needs a resolver that answers, and fails outright on some
    # minimal images; set -e would take the script down with it.
    _fq=$(hostname -f 2>/dev/null || true)
    [ -n "$_fq" ] || _fq=$(hostname 2>/dev/null || true)
    [ -n "$_fq" ] || _fq=localhost
    printf '%s' "$_fq"
}

# The subject of a generated certificate. Nothing will trust it either way, but
# the name still has to be *something* a client can be told to expect.
tls_cn() {
    if [ -n "$QUACKCIT_TLS_CN" ]; then
        printf '%s' "$QUACKCIT_TLS_CN"
        return 0
    fi
    host_fqdn
}

# Write a self-signed certificate/key pair. Both are built under .tmp names and
# moved into place, so an interrupted run cannot leave a half-written file or a
# cert that does not match the key beside it.
tls_generate() {
    _cert=$1; _key=$2; _cn=$3
    _sans="DNS:$_cn"
    case "$_cn" in
        localhost) ;;
        *) _sans="$_sans,DNS:localhost" ;;
    esac
    _sans="$_sans,IP:127.0.0.1,IP:::1"

    mkdir -p "$QUACKCIT_TLS_DIR" 2>/dev/null ||
        die "cannot create $QUACKCIT_TLS_DIR — set QUACKCIT_TLS_DIR to a writable location, or QUACKCIT_TLS_AUTOGEN=0 to run without TLS"
    chmod 700 "$QUACKCIT_TLS_DIR" 2>/dev/null || true
    rm -f "$_cert.tmp" "$_key.tmp"

    # Ten years: a certificate nobody trusts gains nothing from expiring, and a
    # server that quietly stops accepting TLS after a year is the worse failure.
    # -addext arrived in OpenSSL 1.1.1; without it the cert is CN-only, which
    # modern clients dislike but older toolchains are stuck with.
    if ! openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 \
             -subj "/CN=$_cn" -addext "subjectAltName=$_sans" \
             -keyout "$_key.tmp" -out "$_cert.tmp" >/dev/null 2>&1; then
        rm -f "$_cert.tmp" "$_key.tmp"
        if ! openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 \
                 -subj "/CN=$_cn" \
                 -keyout "$_key.tmp" -out "$_cert.tmp" >/dev/null 2>&1; then
            rm -f "$_cert.tmp" "$_key.tmp"
            return 1
        fi
    fi

    chmod 600 "$_key.tmp"
    chmod 644 "$_cert.tmp"
    mv "$_key.tmp" "$_key"
    mv "$_cert.tmp" "$_cert"
}

# Decide what TLS material the listeners will be started with, generating a
# self-signed pair the first time if nothing is configured. Must run before
# gen_startup_sql, which reads QUACKCIT_TLS_CERT/KEY.
ensure_tls_material() {
    # An operator's own certificate always wins, and a path that does not
    # resolve is an error — papering over a typo with a self-signed certificate
    # would hand out the wrong identity for as long as nobody noticed.
    if [ -n "$QUACKCIT_TLS_CERT" ] || [ -n "$QUACKCIT_TLS_KEY" ]; then
        [ -n "$QUACKCIT_TLS_CERT" ] && [ -n "$QUACKCIT_TLS_KEY" ] ||
            die "set both QUACKCIT_TLS_CERT and QUACKCIT_TLS_KEY, or neither"
        [ -r "$QUACKCIT_TLS_CERT" ] || die "no readable certificate at $QUACKCIT_TLS_CERT"
        [ -r "$QUACKCIT_TLS_KEY" ] || die "no readable private key at $QUACKCIT_TLS_KEY"
        return 0
    fi

    env_true "$QUACKCIT_TLS_AUTOGEN" || return 0

    # The "if no certs exist" test: generation happens once, and every restart
    # after it reuses the same certificate, so a client that accepted the
    # fingerprint keeps working.
    if [ -r "$QUACKCIT_TLS_SELF_CERT" ] && [ -r "$QUACKCIT_TLS_SELF_KEY" ]; then
        QUACKCIT_TLS_CERT=$QUACKCIT_TLS_SELF_CERT
        QUACKCIT_TLS_KEY=$QUACKCIT_TLS_SELF_KEY
        return 0
    fi

    if ! command -v openssl >/dev/null 2>&1; then
        say warning tls "no openssl on PATH — cannot generate a certificate; implicit-TLS listeners will be skipped"
        return 0
    fi

    _cn=$(tls_cn)
    if ! tls_generate "$QUACKCIT_TLS_SELF_CERT" "$QUACKCIT_TLS_SELF_KEY" "$_cn"; then
        say warning tls "openssl failed to generate a certificate; implicit-TLS listeners will be skipped"
        return 0
    fi
    QUACKCIT_TLS_CERT=$QUACKCIT_TLS_SELF_CERT
    QUACKCIT_TLS_KEY=$QUACKCIT_TLS_SELF_KEY
    say info tls "generated a self-signed certificate for CN=$_cn at $QUACKCIT_TLS_CERT (valid 10 years)"
    say info tls "no client will trust it — point QUACKCIT_TLS_CERT/QUACKCIT_TLS_KEY at real material for production"
}

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
    # Tab-separated and headerless, not duckbox: everything the CLI prints here
    # ends up in the log, and one row per line is what lets each one become a
    # syslog record. A duckbox table is several lines of box-drawing characters
    # wrapped around one row, which no line-oriented log format can carry.
    printf '.bail off\n.mode tabs\n.headers off\n'

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
            # Stderr only, never stdout: this function's stdout *is* the startup
            # SQL file. `say` routes a warning to stderr for exactly this reason.
            say warning tls "skipped $_key — implicit TLS needs QUACKCIT_TLS_CERT and QUACKCIT_TLS_KEY"
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
    log_line notice startup "quackcit starting"
    seed_site
    ensure_tls_material
    [ -n "$(quackcit_enabled_services)" ] || die "no services enabled; nothing to do"
    gen_startup_sql > "$STARTUP_SQL"
    rm -f "$READY" "$QUACKCIT_ADMIN_FIFO" "$LOG_FIFO"
    mkfifo -m 600 "$QUACKCIT_ADMIN_FIFO"
    mkfifo -m 600 "$LOG_FIFO"
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

# Everything a fresh database needs before the first listener comes up: the
# site's own name, the web settings that make the interface reachable, and the
# accounts in QUACKCIT_SEED_USERS.
#
# This runs *before* the server starts, so adm_exec routes through adm_offline
# and only the umbrella extension is loaded — which is enough, because loading
# it runs EnsureSchema and qm_user_add and qm_config_set both live there. Doing
# it here rather than after startup is also what makes `foreground` seed at all:
# it execs into the CLI and never comes back, so there is no "after" for it.
#
# One probe and at most one write, because adm_offline spawns a DuckDB process
# per call and this runs on every start, not only the first one.
seed_site() {
    # qm_status() runs first and its row is thrown away. The umbrella creates the
    # schema from a table function's *init*, not from LOAD, so on a brand-new
    # database the tables the probe below reads do not exist yet — reading them
    # first would just be a catalog error. Any qm_* function would do; this one
    # touches every table and writes nothing.
    _probe=$(adm_exec tsv "SELECT count(*) FROM qm_status();
SELECT coalesce((SELECT value FROM citadel_config WHERE name = 'c_fqdn'), ''),
       (SELECT count(*) FROM citadel_config WHERE name = 'qm_web_force_https'),
       (SELECT count(*) FROM quackmail_users)" 2>/dev/null) || return 0
    _probe=$(printf '%s\n' "$_probe" | tail -n 1)
    # Three fields or nothing happened: never guess at a half-read probe.
    [ "$(printf '%s' "$_probe" | awk -F'\t' '{print NF}')" = 3 ] || return 0
    _cur_fqdn=$(printf '%s' "$_probe" | cut -f1 | tr -d ' \r')
    _has_force_https=$(printf '%s' "$_probe" | cut -f2 | tr -d ' \r')
    _user_count=$(printf '%s' "$_probe" | cut -f3 | tr -d ' \r')

    # Initialized here, not in the functions below: each one may return before
    # setting its own, and seed_report reads all of them under `set -u`.
    _sql=
    _names=
    _secrets=
    _generated=0
    _seeded_web=0
    _want_fqdn=
    seed_fqdn
    seed_web_defaults
    seed_users
    [ -n "$_sql" ] || return 0
    adm_exec tsv "$_sql" >/dev/null || die "could not seed the database"
    seed_report
}

# The extension seeds c_fqdn itself, as the placeholder `quackmail.test`, so
# "not configured yet" has to mean "empty, or still that placeholder" —
# otherwise the default would never be replaced by anything. A name the operator
# actually chose is never touched; an explicit QUACKCIT_FQDN always wins.
seed_fqdn() {
    if [ -n "$QUACKCIT_FQDN" ]; then
        _want_fqdn=$QUACKCIT_FQDN
    else
        case "$_cur_fqdn" in
            ''|quackmail.test) _want_fqdn=$(host_fqdn) ;;
            *) return 0 ;;
        esac
    fi
    # Already what we would write: clear it again so seed_report does not
    # announce a change that did not happen.
    if [ "$_want_fqdn" = "$_cur_fqdn" ]; then
        _want_fqdn=
        return 0
    fi
    _sql="$_sql
SELECT ok FROM qm_config_set('c_fqdn', $(sql_str "$_want_fqdn"));"
}

# The web interface has to be reachable on a fresh install. qm_web_force_https
# defaults to on, and the redirect it sends is built from c_fqdn with no port —
# deliberately, since using the client's Host header there would be an open
# redirect — so on the default dev ports http://host:8080/ redirects to
# https://host/ and lands on nothing. Written once, and only when the row is
# absent, so an operator's choice and an existing install are both left alone.
seed_web_defaults() {
    [ "$_has_force_https" = 0 ] || return 0
    _seeded_web=1
    _sql="$_sql
SELECT ok FROM qm_config_set('qm_web_force_https', '0');"
}

# The listener extensions provide no idempotent seed, and a table function's
# side effect happens when it is bound, so a guarding WHERE would not stop
# qm_user_add from resetting the password on every restart. Ask first instead:
# seeding happens only while the database has no accounts at all.
seed_users() {
    [ "$_user_count" = 0 ] || return 0

    # Built by hand: `$(...)` strips trailing newlines, so accumulating the
    # credential lines through a command substitution would run them together.
    _tab=$(printf '\t')
    _nl='
'
    # A here-document, not a pipe: `while read` on the right of a pipe runs in a
    # subshell in most shells, and everything accumulated below would be lost
    # when that subshell exited.
    while read -r _name _pw _axlevel; do
        [ -n "$_name" ] || continue
        # Plain assignment, so `die` inside sql_int reaches `set -e`.
        _ax=$(sql_int "${_axlevel:-4}")
        if [ -z "$_pw" ] || [ "$_pw" = - ]; then
            _pw=$(gen_password) ||
                die "no openssl and no readable /dev/urandom to generate a password with — give $_name an explicit one in QUACKCIT_SEED_USERS"
            _generated=1
            _secrets="$_secrets$_name$_tab$_pw$_nl"
        fi
        # One batch for every account: adm_offline spawns a DuckDB process per
        # call, and a per-user call would spawn two of them each.
        _sql="$_sql
SELECT ok, note FROM qm_user_add($(sql_str "$_name"), $(sql_str "$_pw"));
INSERT INTO citadel_users (username, usernum, axlevel)
VALUES ($(sql_str "$_name"), nextval('citadel_user_seq'), $_ax)
ON CONFLICT (username) DO UPDATE SET axlevel = excluded.axlevel;"
        _names="$_names$_name "
    done <<EOF
$QUACKCIT_SEED_USERS
EOF
}

# Said once, after the batch above has actually been executed — a password must
# not be written down, nor an account announced, until the account exists.
seed_report() {
    [ "$_seeded_web" = 0 ] ||
        say notice web "web interface serves plaintext — set qm_web_force_https to 1 once you have a certificate clients trust"
    [ -z "$_want_fqdn" ] ||
        say notice fqdn "site name (c_fqdn) set to $_want_fqdn"
    [ -n "$_names" ] || return 0

    if [ "$_generated" = 1 ]; then
        mkdir -p "$(dirname "$QUACKCIT_SEED_SECRET_FILE")" 2>/dev/null || true
        # Appended, not truncated: seeding again against a database that was
        # emptied must not silently drop the earlier passwords.
        {
            printf '# Passwords QuackCit generated on %s.\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
            printf '# Change them, then delete this file.\n'
            printf '%s' "$_secrets"
        } >> "$QUACKCIT_SEED_SECRET_FILE"
        chmod 600 "$QUACKCIT_SEED_SECRET_FILE" 2>/dev/null || true
        # The path, never the password: this goes to the terminal and the log.
        say info seed "seeded users: ${_names}— generated passwords are in $QUACKCIT_SEED_SECRET_FILE (mode 0600)"
    else
        say info seed "seeded users: $_names"
    fi
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
    #
    # Standard output goes to the log FIFO rather than the log file: the reader
    # started just below turns each line into a syslog record. Opening a FIFO
    # for writing blocks until a reader arrives, and that open happens in the
    # child, so `pid` is available immediately — which is what lets the reader
    # carry the server's real PID as the record's PROCID. Keeping the reader out
    # of a pipeline is also what leaves `$!` pointing at the CLI itself, which
    # the PID file, wait_ready and the control channel all depend on.
    nohup "$QUACKCIT_DUCKDB" -unsigned -init "$STARTUP_SQL" "$QUACKCIT_DB" \
        >"$LOG_FIFO" 2>>"$QUACKCIT_ERRLOG" 0<>"$QUACKCIT_ADMIN_FIFO" &
    pid=$!
    echo "$pid" > "$QUACKCIT_PIDFILE"
    # Exits by itself on end-of-file when the server closes the pipe, so there
    # is nothing extra to stop or reap. Its own output goes to the log and its
    # errors to /dev/null — not to the error log, which is the control channel's
    # byte-exact return path, and above all not to the caller's descriptors: it
    # outlives this script, and inheriting them would leave `quackcit.sh start`
    # holding a pipe open for the life of the server.
    syslog_stream "$pid" server < "$LOG_FIFO" >> "$QUACKCIT_LOG" 2>/dev/null &

    if ! wait_ready "$pid"; then
        kill -KILL "$pid" 2>/dev/null || true
        rm -f "$QUACKCIT_PIDFILE"
        log_line err startup "failed to start; see $QUACKCIT_ERRLOG"
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
        # The CLI announces its -init file on stderr, in colour, on every start.
        # That is not an error, and reporting it as one would put a false `err`
        # record in the log every single time. Filtered here, in the display
        # path only — errlog_delta itself stays byte-exact, because that is how
        # quackcitadm.sh recovers the error text of a failed request.
        _esc=$(printf '\033')
        _startup_err=$(errlog_delta "$err_before" "$err_after" |
                       sed -e "s/${_esc}\[[0-9;]*m//g" \
                           -e '/^-- Loading resources from /d' \
                           -e '/^[[:space:]]*$/d')
        if [ -n "$_startup_err" ]; then
            # The text itself stays in the error log; the record says it
            # happened and where to read it.
            log_line err startup "errors during startup — see $QUACKCIT_ERRLOG"
            echo "quackcit: errors during startup:" >&2
            printf '%s\n' "$_startup_err" >&2
        fi
    fi

    say info startup "started (pid $pid), db=$QUACKCIT_DB, log=$QUACKCIT_LOG"
    # Site policy starts empty, which means: only c_fqdn is accepted as a local
    # domain, no DNSBL is queried, and no outbound mail is DKIM-signed.
    say info policy "policy tables are empty by default — see quackcitadm.sh"
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
        say warning stop "did not stop after 30s, sending SIGKILL"
        kill -KILL "$pid" 2>/dev/null || true
    fi
    rm -f "$QUACKCIT_PIDFILE" "$QUACKCIT_ADMIN_FIFO" "$LOG_FIFO" "$READY" \
          "$QUACKCIT_RUN_DIR/shutdown.sql"
    say info stop "stopped"
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
        # ensure_tls_material only runs on start, so work out what that would
        # have chosen rather than reporting the raw (usually empty) variable.
        if [ -n "$QUACKCIT_TLS_CERT" ]; then
            echo "  tls:     $QUACKCIT_TLS_CERT"
        elif [ -r "$QUACKCIT_TLS_SELF_CERT" ]; then
            echo "  tls:     $QUACKCIT_TLS_SELF_CERT (self-signed)"
        else
            echo "  tls:     none — implicit-TLS listeners are skipped"
        fi
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
    say info startup "running in the foreground; SQL errors go to $QUACKCIT_ERRLOG"
    # Same formatter as the background path, with tee so the records still reach
    # this terminal — or the journal, under systemd. It inherits this shell's
    # stdout now, before the exec below replaces the process, and keeps it.
    # PROCID is $$ because after that exec the CLI *is* this PID.
    syslog_stream "$$" server < "$LOG_FIFO" | tee -a "$QUACKCIT_LOG" &
    exec "$QUACKCIT_DUCKDB" -unsigned -init "$STARTUP_SQL" "$QUACKCIT_DB" \
        >"$LOG_FIFO" 2>>"$QUACKCIT_ERRLOG" 0<>"$QUACKCIT_ADMIN_FIFO"
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
