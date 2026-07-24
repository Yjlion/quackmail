#!/usr/bin/env python3
"""Execute one SQL statement against the QuackCit database and print a table.

DuckDB allows a single read-write process per database file, so this cannot
simply open the file while the server is running. Two paths, chosen
automatically:

  * server up   -> send the SQL over its admin socket (QUACKCIT_ADMIN_SOCK)
  * server down -> open the database file directly and load the umbrella
                   extension so the qm_* functions exist

quackcitadm.sh builds the SQL; this only runs it. Usage:

    quackcit_admin.py "SELECT * FROM qm_domains()"
    quackcit_admin.py --format=tsv "CALL qm_user_add('bob', 'pw')"
"""
import json
import os
import socket
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DB = os.environ.get("QUACKCIT_DB", os.path.join(REPO, "quackcit.duckdb"))
EXT_DIR = os.environ.get("QUACKCIT_EXT_DIR", os.path.join(REPO, "build", "release", "extension"))
SOCK = os.environ.get("QUACKCIT_ADMIN_SOCK", DB + ".admin.sock")


def via_socket(sql, params):
    """Ask the running server to run the statement. Returns None if it is down."""
    if not SOCK or not os.path.exists(SOCK):
        return None
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(30)
        s.connect(SOCK)
    except OSError:
        # A stale socket file left by a crashed server looks exactly like this.
        return None
    with s:
        s.sendall((json.dumps({"sql": sql, "params": params}) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    if not buf:
        return None
    return json.loads(buf.split(b"\n", 1)[0].decode())


def via_file(sql, params):
    """Open the database directly. Only works while the server is stopped."""
    try:
        import duckdb
    except ImportError:
        return {"ok": False, "error": "the duckdb Python package is not installed"}

    try:
        con = duckdb.connect(DB, config={"allow_unsigned_extensions": "true"})
    except Exception as e:
        # The most common cause by far, worth naming explicitly.
        return {
            "ok": False,
            "error": f"{e}\n(is the server running? it holds a write lock on {DB}; "
                     f"the admin socket at {SOCK} was not reachable)",
        }

    try:
        umbrella = os.path.join(EXT_DIR, "quackmail", "quackmail.duckdb_extension")
        if os.path.exists(umbrella):
            con.execute(f"LOAD '{umbrella}'")
        cur = con.execute(sql, params) if params else con.execute(sql)
        columns = [d[0] for d in cur.description] if cur.description else []
        rows = cur.fetchall() if columns else []
        return {
            "ok": True,
            "columns": columns,
            "rows": [[None if v is None else str(v) for v in row] for row in rows],
        }
    except Exception as e:
        return {"ok": False, "error": str(e)}
    finally:
        con.close()


def render(result, fmt):
    if not result.get("ok"):
        print(f"error: {result.get('error', 'unknown error')}", file=sys.stderr)
        return 1

    columns = result.get("columns") or []
    rows = result.get("rows") or []
    if not columns:
        return 0

    if fmt == "tsv":
        for row in rows:
            print("\t".join("" if v is None else v for v in row))
        return 0
    if fmt == "json":
        print(json.dumps([dict(zip(columns, row)) for row in rows], indent=2))
        return 0

    # Aligned columns, which is what a human reading a terminal wants.
    widths = [len(c) for c in columns]
    for row in rows:
        for i, v in enumerate(row):
            widths[i] = max(widths[i], len("" if v is None else v))
    line = "  ".join(c.ljust(widths[i]) for i, c in enumerate(columns))
    print(line)
    print("  ".join("-" * w for w in widths))
    for row in rows:
        print("  ".join(("" if v is None else v).ljust(widths[i]) for i, v in enumerate(row)))
    if not rows:
        print("(no rows)")
    return 0


def main(argv):
    fmt = "table"
    args = []
    params = []
    it = iter(argv[1:])
    for a in it:
        if a.startswith("--format="):
            fmt = a.split("=", 1)[1]
        elif a == "--param":
            params.append(next(it, ""))
        else:
            args.append(a)
    if not args:
        print("usage: quackcit_admin.py [--format=table|tsv|json] [--param V ...] <sql>",
              file=sys.stderr)
        return 2

    sql = args[0]
    result = via_socket(sql, params)
    if result is None:
        result = via_file(sql, params)
    return render(result, fmt)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
