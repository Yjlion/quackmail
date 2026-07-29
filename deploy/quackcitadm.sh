#!/bin/sh
# Administer a QuackCit server: users, domains, aliases, access rules, DKIM
# keys, rate limits, Sieve scripts, the outbound queue and server config.
#
# Works whether or not the server is running. DuckDB permits a single
# read-write process per database file, so while the server is up this talks to
# it over its control FIFO; when it is down, the database file is opened
# directly with the bundled DuckDB CLI. Either way the commands are identical.
#
#     deploy/quackcitadm.sh user add alice s3cret
#     deploy/quackcitadm.sh domain add example.com
#     deploy/quackcitadm.sh dkim keygen example.com mail
#
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=quackcit_common.sh
. "$HERE/quackcit_common.sh"

FORMAT=${QUACKCIT_FORMAT:-table}

need() {
    # need <count> <given...> — the first argument is how many are required.
    want=$1; shift
    [ "$#" -ge "$want" ] || die "missing arguments; try 'quackcitadm.sh help'"
}

# q <sql> — every user-supplied value reaches the statement through sql_str or
# sql_int, never raw, so an address or a script containing a quote cannot break
# out of its literal.
q() { adm_exec "$FORMAT" "$1"; }

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
        add|passwd) need 2 "$@"
                    q "SELECT ok, note FROM qm_user_add($(sql_str "$1"), $(sql_str "$2"))" ;;
        remove)     need 1 "$@"
                    q "SELECT ok, note FROM qm_user_remove($(sql_str "$1"))" ;;
        list)       q "SELECT u.username, u.enabled, c.usernum, c.axlevel
                       FROM quackmail_users u
                       LEFT JOIN citadel_users c ON c.username = u.username
                       ORDER BY u.username" ;;
        *) die "unknown user command '$action' (add|remove|passwd|list)" ;;
    esac
}

cmd_domain() {
    action=${1:-list}; shift 2>/dev/null || true
    case "$action" in
        add)    need 1 "$@"
                q "SELECT ok, note FROM qm_domain_add($(sql_str "$1"), $(sql_str "${2:-local}"))" ;;
        remove) need 1 "$@"
                q "SELECT ok, note FROM qm_domain_remove($(sql_str "$1"))" ;;
        list)   q "SELECT * FROM qm_domains()" ;;
        *) die "unknown domain command '$action' (add|remove|list)" ;;
    esac
}

cmd_alias() {
    action=${1:-list}; shift 2>/dev/null || true
    case "$action" in
        add)    need 2 "$@"
                q "SELECT ok, note FROM qm_alias_add($(sql_str "$1"), $(sql_str "$2"))" ;;
        remove) need 1 "$@"
                q "SELECT ok, note FROM qm_alias_remove($(sql_str "$1"), $(sql_str "${2:-}"))" ;;
        list)   q "SELECT * FROM qm_aliases()" ;;
        *) die "unknown alias command '$action' (add|remove|list)" ;;
    esac
}

cmd_acl() {
    action=${1:-list}; shift 2>/dev/null || true
    case "$action" in
        allow|block)
            need 2 "$@"
            q "SELECT ok, note FROM qm_acl_add($(sql_str "$1"), $(sql_str "$2"), $(sql_str "$action"), $(sql_str "${3:-}"))" ;;
        remove) need 1 "$@"
                id=$(sql_int "$1")
                q "SELECT ok, note FROM qm_acl_remove($id)" ;;
        list)   q "SELECT * FROM qm_acl()" ;;
        *) die "unknown acl command '$action' (allow|block|remove|list)" ;;
    esac
}

cmd_rbl() {
    action=${1:-list}; shift 2>/dev/null || true
    case "$action" in
        add)    need 1 "$@"; q "SELECT ok, note FROM qm_rbl_add($(sql_str "$1"))" ;;
        remove) need 1 "$@"; q "SELECT ok, note FROM qm_rbl_remove($(sql_str "$1"))" ;;
        list)   q "SELECT * FROM qm_rbl_zones()" ;;
        check)  need 1 "$@"; q "SELECT * FROM qm_rbl_check($(sql_str "$1"))" ;;
        *) die "unknown rbl command '$action' (add|remove|list|check)" ;;
    esac
}

cmd_dkim() {
    action=${1:-list}; shift 2>/dev/null || true
    case "$action" in
        keygen)
            need 2 "$@"
            bits=$(sql_int "${3:-2048}")
            echo "Generating a ${bits}-bit key for $1 (selector $2)..."
            q "SELECT ok, dns_name, dns_record FROM qm_dkim_keygen($(sql_str "$1"), $(sql_str "$2"), $bits)"
            echo
            echo "Publish that record as a TXT record at the name shown above,"
            echo "then verify with:  dig +short TXT $2._domainkey.$1"
            ;;
        list)   q "SELECT * FROM qm_dkim_keys()" ;;
        remove) need 2 "$@"
                q "SELECT ok, note FROM qm_dkim_key_remove($(sql_str "$1"), $(sql_str "$2"))" ;;
        verify)
            need 1 "$@"
            [ -f "$1" ] || die "no such file: $1"
            q "SELECT * FROM qm_dkim_verify_detail($(sql_str "$(cat "$1")"))" ;;
        *) die "unknown dkim command '$action' (keygen|list|remove|verify)" ;;
    esac
}

cmd_ratelimit() {
    action=${1:-list}; shift 2>/dev/null || true
    case "$action" in
        set)
            need 4 "$@"
            burst_max=$(sql_int "$2"); burst_secs=$(sql_int "$3"); daily_max=$(sql_int "$4")
            q "SELECT ok, note FROM qm_ratelimit_set($(sql_str "$1"), $burst_max, $burst_secs, $daily_max)" ;;
        list)   q "SELECT * FROM qm_ratelimits()" ;;
        status) need 1 "$@"; q "SELECT * FROM qm_rate_status($(sql_str "$1"))" ;;
        *) die "unknown ratelimit command '$action' (set|list|status)" ;;
    esac
}

cmd_config() {
    action=${1:-list}; shift 2>/dev/null || true
    case "$action" in
        get)  need 1 "$@"; q "SELECT * FROM qm_config_get($(sql_str "$1"))" ;;
        set)  need 2 "$@"
              q "SELECT ok, note FROM qm_config_set($(sql_str "$1"), $(sql_str "$2"))" ;;
        list) q "SELECT * FROM qm_config()" ;;
        *) die "unknown config command '$action' (get|set|list)" ;;
    esac
}

cmd_room() {
    action=${1:-list}; shift 2>/dev/null || true
    case "$action" in
        add)  need 1 "$@"; q "SELECT ok, note FROM cit_room_add($(sql_str "$1"))" ;;
        list) q "SELECT room_num, display_name, floor_num, qr_flags, highest_msg
                 FROM citadel_rooms ORDER BY floor_num, listorder, room_num" ;;
        acl)  need 1 "$@"
              if [ $# -ge 3 ]; then
                  q "SELECT ok, note FROM cit_room_acl_set($(sql_str "$1"), $(sql_str "$2"), $(sql_str "$3"))"
              else
                  q "SELECT * FROM cit_room_acl($(sql_str "$1"))"
              fi ;;
        *) die "unknown room command '$action' (add|list|acl)" ;;
    esac
}

cmd_floor() {
    action=${1:-list}; shift 2>/dev/null || true
    case "$action" in
        add)  need 1 "$@"; q "SELECT ok, note FROM cit_floor_add($(sql_str "$1"))" ;;
        list) q "SELECT floor_num, name FROM citadel_floors ORDER BY floor_num" ;;
        *) die "unknown floor command '$action' (add|list)" ;;
    esac
}

cmd_list() {
    action=${1:-list}; shift 2>/dev/null || true
    case "$action" in
        list)   q "SELECT * FROM qm_lists()" ;;
        create) need 1 "$@"
                q "SELECT ok, note FROM qm_list_create($(sql_str "$1"), $(sql_str "${2:-}"))" ;;
        set)    need 3 "$@"
                q "SELECT ok, note FROM qm_list_set($(sql_str "$1"), $(sql_str "$2"), $(sql_str "$3"))" ;;
        remove) need 1 "$@"; q "SELECT ok, note FROM qm_list_remove($(sql_str "$1"))" ;;
        subs)   need 1 "$@"; q "SELECT * FROM qm_list_subs($(sql_str "$1"))" ;;
        subscribe)
                need 2 "$@"
                q "SELECT ok, note FROM qm_list_sub_add($(sql_str "$1"), $(sql_str "$2"), $(sql_str "${3:-post}"))" ;;
        unsubscribe)
                need 2 "$@"
                q "SELECT ok, note FROM qm_list_sub_remove($(sql_str "$1"), $(sql_str "$2"))" ;;
        held)   q "SELECT * FROM qm_list_held($(sql_str "${1:-}"))" ;;
        approve)
                need 1 "$@"
                id=$(sql_int "$1")
                q "SELECT ok, note FROM qm_list_approve($id)" ;;
        reject) need 1 "$@"
                id=$(sql_int "$1")
                q "SELECT ok, note FROM qm_list_reject($id)" ;;
        # Distribution normally happens on the spooler's timer; this forces a
        # pass now, which is what you want after changing something.
        run)    if [ $# -ge 1 ]; then
                    n=$(sql_int "$1")
                    q "SELECT * FROM qm_listserv_run(room_num => $n)"
                else
                    q "SELECT * FROM qm_listserv_run()"
                fi ;;
        *) die "unknown list command '$action' (list|create|set|remove|subs|subscribe|unsubscribe|held|approve|reject|run)" ;;
    esac
}

cmd_feed() {
    action=${1:-list}; shift 2>/dev/null || true
    case "$action" in
        list)   q "SELECT * FROM qm_feeds()" ;;
        add)    need 4 "$@"
                q "SELECT ok, note FROM qm_feed_add($(sql_str "$1"), $(sql_str "$2"), $(sql_str "$3"), $(sql_str "$4"))" ;;
        set)    need 3 "$@"
                q "SELECT ok, note FROM qm_feed_set($(sql_str "$1"), $(sql_str "$2"), $(sql_str "$3"))" ;;
        remove) need 1 "$@"; q "SELECT ok, note FROM qm_feed_remove($(sql_str "$1"))" ;;
        test)   need 1 "$@"; q "SELECT ok, note FROM qm_feed_test($(sql_str "$1"))" ;;
        # Poll now rather than waiting for the interval. No argument polls every
        # enabled feed.
        run)    if [ $# -ge 1 ]; then
                    q "SELECT * FROM qm_fetch_run(feed => $(sql_str "$1"))"
                else
                    q "SELECT * FROM qm_fetch_run()"
                fi ;;
        *) die "unknown feed command '$action' (list|add|set|remove|test|run)" ;;
    esac
}

cmd_queue() {
    action=${1:-list}; shift 2>/dev/null || true
    case "$action" in
        list) q "SELECT id, from_addr, rcpt, status, attempts, next_attempt_at, last_error
                 FROM quackmail_outbound ORDER BY id" ;;
        retry)
            need 1 "$@"
            id=$(sql_int "$1")
            q "UPDATE quackmail_outbound SET status = 'queued', next_attempt_at = now()
               WHERE id = $id RETURNING id, rcpt, status" ;;
        flush)
            # Only the terminal rows; anything still queued is left alone.
            q "DELETE FROM quackmail_outbound WHERE status IN ('sent', 'failed')
               RETURNING id, rcpt, status" ;;
        *) die "unknown queue command '$action' (list|retry|flush)" ;;
    esac
}

cmd_sieve() {
    action=${1:-list}; shift 2>/dev/null || true
    case "$action" in
        list) need 1 "$@"
              q "SELECT name, active, length(script) AS bytes
                 FROM quackmail_sieve_scripts WHERE username = $(sql_str "$1") ORDER BY name" ;;
        get)  need 2 "$@"
              as_tsv q "SELECT script FROM quackmail_sieve_scripts
                        WHERE username = $(sql_str "$1") AND name = $(sql_str "$2")" ;;
        set)
            need 3 "$@"
            [ -f "$3" ] || die "no such file: $3"
            script=$(cat "$3")
            # Refuse to install a script the engine cannot parse, so a typo in a
            # filter is caught here rather than silently at delivery time.
            check=$(as_tsv q "SELECT ok FROM qm_sieve_check($(sql_str "$script"))")
            case "$check" in
                true|True|1) : ;;
                *) as_tsv q "SELECT note FROM qm_sieve_check($(sql_str "$script"))" >&2
                   die "script rejected; nothing was stored" ;;
            esac
            # One statement per request, so this is a delete followed by an insert.
            q "DELETE FROM quackmail_sieve_scripts
               WHERE username = $(sql_str "$1") AND name = $(sql_str "$2")" >/dev/null
            q "INSERT INTO quackmail_sieve_scripts (username, name, active, script)
               VALUES ($(sql_str "$1"), $(sql_str "$2"), false, $(sql_str "$script"))" >/dev/null
            echo "stored script '$2' for $1 (activate it with: quackcitadm.sh sieve activate $1 $2)"
            ;;
        activate)
            need 2 "$@"
            q "UPDATE quackmail_sieve_scripts SET active = false
               WHERE username = $(sql_str "$1")" >/dev/null
            q "UPDATE quackmail_sieve_scripts SET active = true
               WHERE username = $(sql_str "$1") AND name = $(sql_str "$2")
               RETURNING name, active" ;;
        check)
            need 1 "$@"
            [ -f "$1" ] || die "no such file: $1"
            q "SELECT ok, note FROM qm_sieve_check($(sql_str "$(cat "$1")"))" ;;
        *) die "unknown sieve command '$action' (list|get|set|activate|check)" ;;
    esac
}

cmd_spf() {
    need 3 "$@"
    q "SELECT * FROM qm_spf_check($(sql_str "$1"), $(sql_str "$2"), $(sql_str "$3"))"
}

cmd_dmarc() {
    need 1 "$@"
    q "SELECT * FROM qm_dmarc_check($(sql_str "$1"))"
}

cmd_status() {
    q "SELECT * FROM qm_status()"
}

# Browser sessions for the web front-end. The raw token is never stored — only
# its SHA-256 — so there is nothing here that could be replayed as a login.
cmd_websession() {
    action=${1:-list}; shift 2>/dev/null || true
    case "$action" in
        list) now=$(sql_now)
              q "SELECT token_hash, username, peer_ip, tls, user_agent,
                        to_timestamp(created_at) AS created,
                        to_timestamp(last_seen)  AS last_seen,
                        to_timestamp(expires_at) AS expires
                 FROM quackmail_web_sessions
                 WHERE NOT revoked AND expires_at > $now
                 ORDER BY last_seen DESC" ;;
        revoke)
            need 1 "$@"
            q "DELETE FROM quackmail_web_sessions WHERE token_hash = $(sql_str "$1")
               RETURNING token_hash, username" ;;
        revoke-user)
            need 1 "$@"
            q "DELETE FROM quackmail_web_sessions WHERE username = $(sql_str "$1")
               RETURNING token_hash, username" ;;
        prune)
            now=$(sql_now)
            q "DELETE FROM quackmail_web_sessions
               WHERE revoked OR expires_at <= $now
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
  room      add <name> | list | acl <room> [<identifier> <rights>]
              RFC 4314 rights: lrswipkxtea. Granting "anyone" the p right opens
              the room to mail at room_<name>@<fqdn>; no rights removes the entry
  floor     add <name> | list
  list      list | create <room> [address] | set <room> <key> <value> | remove <room>
            subs <room> | subscribe <room> <address> [post|digest]
            unsubscribe <room> <address> | held [room] | approve <id> | reject <id>
            run [room_num]
              a mailing list is a room: posts to it are archived there and fanned
              out to its subscribers. set keys: address, enabled, mode
              (post|digest|both), post_policy (anyone|subscribers|moderated),
              reply_to (sender|list), subject_tag, footer, digest_interval,
              digest_max. subscribe from here is confirmed outright; anyone
              mailing <list>-subscribe@ must answer a confirmation first
  feed      list | add <name> <pop3|imap|rss> <source> <target> | set <name> <key> <value>
            remove <name> | test <name> | run [name]
              pull messages from somewhere else into a room. <source> is a URL
              for rss, or user:password@host[:port] for a mailbox; <target> is a
              room name, or user:<name> to route through that user's filters.
              set keys: kind, enabled, url, host, port, tls (none|starttls|
              implicit), username, password, mailbox, room, user, author,
              subject_prefix, interval, leave_on_server, max_per_run
  queue     list | retry <id> | flush
  websession list | revoke <token_hash> | revoke-user <name> | prune
              signed-in browsers; only the hash of each token is stored
  spf       <client-ip> <helo> <mail-from>
  dmarc     <domain>
  status
  sql       <statement>

Output format: QUACKCIT_FORMAT=table (default) | tsv | json
Database: $QUACKCIT_DB
Control:  $QUACKCIT_ADMIN_FIFO (only while the server is running)
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
    list)      cmd_list "$@" ;;
    feed)      cmd_feed "$@" ;;
    queue)     cmd_queue "$@" ;;
    websession) cmd_websession "$@" ;;
    spf)       cmd_spf "$@" ;;
    dmarc)     cmd_dmarc "$@" ;;
    status)    cmd_status ;;
    sql)       need 1 "$@"; q "$1" ;;
    help|-h|--help) usage ;;
    *)         usage; exit 2 ;;
esac
