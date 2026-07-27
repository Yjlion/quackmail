#!/bin/sh
# Administer a QuackCit server: users, domains, aliases, access rules, DKIM
# keys, rate limits, Sieve scripts, the outbound queue and server config.
#
# Works whether or not the server is running. DuckDB permits a single
# read-write process per database file, so while the server is up this talks to
# it over its admin socket; when it is down, the database file is opened
# directly. Either way the commands are identical.
#
#     deploy/quackcitadm.sh user add alice s3cret
#     deploy/quackcitadm.sh domain add example.com
#     deploy/quackcitadm.sh dkim keygen example.com mail
#
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(dirname "$HERE")

CONF=${QUACKCIT_CONF:-$HERE/quackcit.conf}
if [ -f "$CONF" ]; then
    set -a
    # shellcheck disable=SC1090
    . "$CONF"
    set +a
fi

: "${QUACKCIT_DB:=$REPO/quackcit.duckdb}"
: "${QUACKCIT_EXT_DIR:=$REPO/build/release/extension}"
: "${QUACKCIT_ADMIN_SOCK:=$QUACKCIT_DB.admin.sock}"
: "${PYTHON:=python3}"
export QUACKCIT_DB QUACKCIT_EXT_DIR QUACKCIT_ADMIN_SOCK

RUNNER=$HERE/quackcit_admin.py
FORMAT=${QUACKCIT_FORMAT:-table}

die() { echo "quackcitadm: $*" >&2; exit 1; }

need() {
    # need <count> <given...> — the first argument is how many are required.
    want=$1; shift
    [ "$#" -ge "$want" ] || die "missing arguments; try 'quackcitadm.sh help'"
}

# qN <sql> <param>... — parameters are bound by the runner, never interpolated
# into the SQL, so an address or script containing a quote cannot break the
# statement. POSIX sh has no arrays, hence one helper per arity.
q0() { "$PYTHON" "$RUNNER" "--format=$FORMAT" "$1"; }
q1() { "$PYTHON" "$RUNNER" "--format=$FORMAT" --param "$2" "$1"; }
q2() { "$PYTHON" "$RUNNER" "--format=$FORMAT" --param "$2" --param "$3" "$1"; }
q3() { "$PYTHON" "$RUNNER" "--format=$FORMAT" --param "$2" --param "$3" --param "$4" "$1"; }
q4() { "$PYTHON" "$RUNNER" "--format=$FORMAT" --param "$2" --param "$3" --param "$4" --param "$5" "$1"; }

# Run one query with a temporary output format. A `VAR=val func` prefix is not
# portable for shell functions, so save and restore explicitly.
as_tsv() {
    _saved=$FORMAT
    FORMAT=tsv
    "$@"
    _rc=$?
    FORMAT=$_saved
    return $_rc
}

# ---------------------------------------------------------------------------

cmd_user() {
    action=${1:-list}; shift 2>/dev/null || true
    case "$action" in
        add|passwd) need 2 "$@"; q2 "SELECT ok, note FROM qm_user_add(?, ?)" "$1" "$2" ;;
        remove)     need 1 "$@"; q1 "SELECT ok, note FROM qm_user_remove(?)" "$1" ;;
        list)       q0 "SELECT u.username, u.enabled, c.usernum, c.axlevel
                        FROM quackmail_users u
                        LEFT JOIN citadel_users c ON c.username = u.username
                        ORDER BY u.username" ;;
        *) die "unknown user command '$action' (add|remove|passwd|list)" ;;
    esac
}

cmd_domain() {
    action=${1:-list}; shift 2>/dev/null || true
    case "$action" in
        add)    need 1 "$@"; q2 "SELECT ok, note FROM qm_domain_add(?, ?)" "$1" "${2:-local}" ;;
        remove) need 1 "$@"; q1 "SELECT ok, note FROM qm_domain_remove(?)" "$1" ;;
        list)   q0 "SELECT * FROM qm_domains()" ;;
        *) die "unknown domain command '$action' (add|remove|list)" ;;
    esac
}

cmd_alias() {
    action=${1:-list}; shift 2>/dev/null || true
    case "$action" in
        add)    need 2 "$@"; q2 "SELECT ok, note FROM qm_alias_add(?, ?)" "$1" "$2" ;;
        remove) need 1 "$@"; q2 "SELECT ok, note FROM qm_alias_remove(?, ?)" "$1" "${2:-}" ;;
        list)   q0 "SELECT * FROM qm_aliases()" ;;
        *) die "unknown alias command '$action' (add|remove|list)" ;;
    esac
}

cmd_acl() {
    action=${1:-list}; shift 2>/dev/null || true
    case "$action" in
        allow|block)
            need 2 "$@"
            q4 "SELECT ok, note FROM qm_acl_add(?, ?, ?, ?)" "$1" "$2" "$action" "${3:-}" ;;
        remove) need 1 "$@"; q1 "SELECT ok, note FROM qm_acl_remove(CAST(? AS BIGINT))" "$1" ;;
        list)   q0 "SELECT * FROM qm_acl()" ;;
        *) die "unknown acl command '$action' (allow|block|remove|list)" ;;
    esac
}

cmd_rbl() {
    action=${1:-list}; shift 2>/dev/null || true
    case "$action" in
        add)    need 1 "$@"; q1 "SELECT ok, note FROM qm_rbl_add(?)" "$1" ;;
        remove) need 1 "$@"; q1 "SELECT ok, note FROM qm_rbl_remove(?)" "$1" ;;
        list)   q0 "SELECT * FROM qm_rbl_zones()" ;;
        check)  need 1 "$@"; q1 "SELECT * FROM qm_rbl_check(?)" "$1" ;;
        *) die "unknown rbl command '$action' (add|remove|list|check)" ;;
    esac
}

cmd_dkim() {
    action=${1:-list}; shift 2>/dev/null || true
    case "$action" in
        keygen)
            need 2 "$@"
            echo "Generating a ${3:-2048}-bit key for $1 (selector $2)..."
            q3 "SELECT ok, dns_name, dns_record FROM qm_dkim_keygen(?, ?, CAST(? AS BIGINT))" "$1" "$2" "${3:-2048}"
            echo
            echo "Publish that record as a TXT record at the name shown above,"
            echo "then verify with:  dig +short TXT $2._domainkey.$1"
            ;;
        list)   q0 "SELECT * FROM qm_dkim_keys()" ;;
        remove) need 2 "$@"; q2 "SELECT ok, note FROM qm_dkim_key_remove(?, ?)" "$1" "$2" ;;
        verify)
            need 1 "$@"
            [ -f "$1" ] || die "no such file: $1"
            q1 "SELECT * FROM qm_dkim_verify_detail(?)" "$(cat "$1")" ;;
        *) die "unknown dkim command '$action' (keygen|list|remove|verify)" ;;
    esac
}

cmd_ratelimit() {
    action=${1:-list}; shift 2>/dev/null || true
    case "$action" in
        set)
            need 4 "$@"
            q4 "SELECT ok, note FROM qm_ratelimit_set(?, CAST(? AS BIGINT), CAST(? AS BIGINT), CAST(? AS BIGINT))" "$1" "$2" "$3" "$4" ;;
        list)   q0 "SELECT * FROM qm_ratelimits()" ;;
        status) need 1 "$@"; q1 "SELECT * FROM qm_rate_status(?)" "$1" ;;
        *) die "unknown ratelimit command '$action' (set|list|status)" ;;
    esac
}

cmd_config() {
    action=${1:-list}; shift 2>/dev/null || true
    case "$action" in
        get)  need 1 "$@"; q1 "SELECT * FROM qm_config_get(?)" "$1" ;;
        set)  need 2 "$@"; q2 "SELECT ok, note FROM qm_config_set(?, ?)" "$1" "$2" ;;
        list) q0 "SELECT * FROM qm_config()" ;;
        *) die "unknown config command '$action' (get|set|list)" ;;
    esac
}

cmd_room() {
    action=${1:-list}; shift 2>/dev/null || true
    case "$action" in
        add)  need 1 "$@"; q1 "SELECT ok, note FROM cit_room_add(?)" "$1" ;;
        list) q0 "SELECT room_num, display_name, floor_num, qr_flags, highest_msg
                  FROM citadel_rooms ORDER BY floor_num, listorder, room_num" ;;
        *) die "unknown room command '$action' (add|list)" ;;
    esac
}

cmd_floor() {
    action=${1:-list}; shift 2>/dev/null || true
    case "$action" in
        add)  need 1 "$@"; q1 "SELECT ok, note FROM cit_floor_add(?)" "$1" ;;
        list) q0 "SELECT floor_num, name FROM citadel_floors ORDER BY floor_num" ;;
        *) die "unknown floor command '$action' (add|list)" ;;
    esac
}

cmd_queue() {
    action=${1:-list}; shift 2>/dev/null || true
    case "$action" in
        list) q0 "SELECT id, from_addr, rcpt, status, attempts, next_attempt_at, last_error
                  FROM quackmail_outbound ORDER BY id" ;;
        retry)
            need 1 "$@"
            q1 "UPDATE quackmail_outbound SET status = 'queued', next_attempt_at = now()
                WHERE id = CAST(? AS BIGINT) RETURNING id, rcpt, status" "$1" ;;
        flush)
            # Only the terminal rows; anything still queued is left alone.
            q0 "DELETE FROM quackmail_outbound WHERE status IN ('sent', 'failed')
                RETURNING id, rcpt, status" ;;
        *) die "unknown queue command '$action' (list|retry|flush)" ;;
    esac
}

cmd_sieve() {
    action=${1:-list}; shift 2>/dev/null || true
    case "$action" in
        list) need 1 "$@"
              q1 "SELECT name, active, length(script) AS bytes
                  FROM quackmail_sieve_scripts WHERE username = ? ORDER BY name" "$1" ;;
        get)  need 2 "$@"
              as_tsv q2 "SELECT script FROM quackmail_sieve_scripts
                         WHERE username = ? AND name = ?" "$1" "$2" ;;
        set)
            need 3 "$@"
            [ -f "$3" ] || die "no such file: $3"
            script=$(cat "$3")
            # Refuse to install a script the engine cannot parse, so a typo in a
            # filter is caught here rather than silently at delivery time.
            check=$(as_tsv q1 "SELECT ok FROM qm_sieve_check(?)" "$script")
            case "$check" in
                true|True|1) : ;;
                *) as_tsv q1 "SELECT note FROM qm_sieve_check(?)" "$script" >&2
                   die "script rejected; nothing was stored" ;;
            esac
            # One statement per call: the runner executes a single statement.
            q2 "DELETE FROM quackmail_sieve_scripts WHERE username = ? AND name = ?" "$1" "$2" >/dev/null
            q3 "INSERT INTO quackmail_sieve_scripts (username, name, active, script)
                VALUES (?, ?, false, ?)" "$1" "$2" "$script" >/dev/null
            echo "stored script '$2' for $1 (activate it with: quackcitadm.sh sieve activate $1 $2)"
            ;;
        activate)
            need 2 "$@"
            q1 "UPDATE quackmail_sieve_scripts SET active = false WHERE username = ?" "$1" >/dev/null
            q2 "UPDATE quackmail_sieve_scripts SET active = true
                WHERE username = ? AND name = ? RETURNING name, active" "$1" "$2" ;;
        check)
            need 1 "$@"
            [ -f "$1" ] || die "no such file: $1"
            q1 "SELECT ok, note FROM qm_sieve_check(?)" "$(cat "$1")" ;;
        *) die "unknown sieve command '$action' (list|get|set|activate|check)" ;;
    esac
}

cmd_spf() {
    need 3 "$@"
    q3 "SELECT * FROM qm_spf_check(?, ?, ?)" "$1" "$2" "$3"
}

cmd_dmarc() {
    need 1 "$@"
    q1 "SELECT * FROM qm_dmarc_check(?)" "$1"
}

cmd_status() {
    q0 "SELECT * FROM qm_status()"
}

# Browser sessions for the web front-end. The raw token is never stored — only
# its SHA-256 — so there is nothing here that could be replayed as a login.
cmd_websession() {
    action=${1:-list}; shift 2>/dev/null || true
    case "$action" in
        list) q0 "SELECT token_hash, username, peer_ip, tls, user_agent,
                         to_timestamp(created_at) AS created,
                         to_timestamp(last_seen)  AS last_seen,
                         to_timestamp(expires_at) AS expires
                  FROM quackmail_web_sessions
                  WHERE NOT revoked AND expires_at > epoch(now())
                  ORDER BY last_seen DESC" ;;
        revoke)
            need 1 "$@"
            q1 "DELETE FROM quackmail_web_sessions WHERE token_hash = ?
                RETURNING token_hash, username" "$1" ;;
        revoke-user)
            need 1 "$@"
            q1 "DELETE FROM quackmail_web_sessions WHERE username = ?
                RETURNING token_hash, username" "$1" ;;
        prune)
            q0 "DELETE FROM quackmail_web_sessions
                WHERE revoked OR expires_at <= epoch(now())
                RETURNING token_hash, username" ;;
        *) die "unknown websession command '$action' (list|revoke|revoke-user|prune)" ;;
    esac
}

usage() {
    cat <<EOF
usage: quackcitadm.sh <object> <action> [arguments]

  user      add <name> <pw> | passwd <name> <pw> | remove <name> | list
  domain    add <domain> [local|relay] | remove <domain> | list
  alias     add <alias> <destination> | remove <alias> [destination] | list
              <alias> may be an address (sales@example.com) or a per-domain
              catch-all (@example.com)
  acl       allow <scope> <pattern> [note] | block <scope> <pattern> [note]
            remove <id> | list
              scope: ip | sender | domain | rcpt | helo | webadmin
              patterns are globs; ip and webadmin also accept CIDR (192.0.2.0/24)
              an allow rule always beats a block rule
              webadmin restricts which networks may reach the web console
  rbl       add <zone> | remove <zone> | list | check <ip>
  dkim      keygen <domain> <selector> [bits] | list | remove <domain> <selector>
            verify <file>
  ratelimit set <user|''> <burst_max> <burst_secs> <daily_max> | list | status <user>
              the '' user is the default policy (100/300s, 500/24h)
  sieve     list <user> | get <user> <name> | set <user> <name> <file>
            activate <user> <name> | check <file>
  config    get <name> | set <name> <value> | list
              qm_spf_reject, qm_dkim_reject, qm_dmarc_enforce, qm_rbl_reject,
              qm_quarantine_room, c_fqdn, ...
              web: qm_web_force_https, qm_web_trusted_proxies, qm_web_hsts,
                   qm_web_origins, qm_web_admin_enabled (off by default),
                   qm_web_admin_require_tls
  room      add <name> | list
  floor     add <name> | list
  queue     list | retry <id> | flush
  websession list | revoke <token_hash> | revoke-user <name> | prune
              signed-in browsers; only the hash of each token is stored
  spf       <client-ip> <helo> <mail-from>
  dmarc     <domain>
  status
  sql       <statement>

Output format: QUACKCIT_FORMAT=table (default) | tsv | json
Database: $QUACKCIT_DB
EOF
}

object=${1:-help}
shift 2>/dev/null || true
case "$object" in
    user)      cmd_user "$@" ;;
    domain)    cmd_domain "$@" ;;
    alias)     cmd_alias "$@" ;;
    acl)       cmd_acl "$@" ;;
    rbl)       cmd_rbl "$@" ;;
    dkim)      cmd_dkim "$@" ;;
    ratelimit) cmd_ratelimit "$@" ;;
    sieve)     cmd_sieve "$@" ;;
    config)    cmd_config "$@" ;;
    room)      cmd_room "$@" ;;
    floor)     cmd_floor "$@" ;;
    queue)     cmd_queue "$@" ;;
    websession) cmd_websession "$@" ;;
    spf)       cmd_spf "$@" ;;
    dmarc)     cmd_dmarc "$@" ;;
    status)    cmd_status ;;
    sql)       need 1 "$@"; q0 "$1" ;;
    help|-h|--help) usage ;;
    *)         usage; exit 2 ;;
esac
