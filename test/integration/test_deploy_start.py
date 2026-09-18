#!/usr/bin/env python3
"""Start and stop the whole server the way a real install does.

Every other suite loads extensions into a Python DuckDB and starts one or two
listeners. None of them runs deploy/quackcit.sh: the startup script with every
listener and every worker at once, on a database that already has data in it.
That is where a crash-at-start lives — concurrent schema migration, a worker's
first tick racing the listeners — and it is the one configuration a user
actually runs.

What this pins:

  * `quackcit.sh start` comes up with every service enabled, twice in a row on
    the same database (the second start is an upgrade-in-place with nothing to
    create), and stays up;
  * every listener and worker reports "started" in the log;
  * nothing reaches the error log but the CLI's own banner;
  * `quackcit.sh stop` exits cleanly and leaves no process behind.

Requires: a `make release` build (the repo layout of deploy/quackcit.sh).
"""
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCRIPT = os.path.join(REPO, "deploy", "quackcit.sh")

if not os.path.exists(os.path.join(REPO, "build", "release", "duckdb")):
    print("SKIP test_deploy_start.py: no build/release/duckdb")
    sys.exit(0)

# Off the dev defaults, so this can run beside a developer's own server.
PORTS = {
    "CITADEL": 25040, "SMTP": 22525, "LMTP": 22033, "SUBMISSION": 22587, "SUBMISSIONS": 22465,
    "POP3": 21110, "POP3S": 21995, "IMAP": 21143, "IMAPS": 21993, "MANAGESIEVE": 24190,
    "TELNET": 22300, "TELNETS": 22992, "SSH": 22222, "NNTP": 21119, "NNTPS": 21563,
    "FTP": 22121, "FTPS": 21990, "XMPP": 25222, "XMPPS": 25223, "HTTP": 28080, "HTTPS": 28443,
}


def run(env, *args, timeout=120):
    return subprocess.run([SCRIPT, *args], env=env, capture_output=True, text=True, timeout=timeout)


def main():
    tmp = tempfile.mkdtemp(prefix="qc-deploy-")
    env = dict(os.environ)
    for k in ("HOME", "STATE_DIR", "LOG_DIR", "RUN_DIR"):
        env[f"QUACKCIT_{k}"] = tmp
    env["QUACKCIT_DB"] = os.path.join(tmp, "quackcit.duckdb")
    env["QUACKCIT_CONF"] = os.path.join(REPO, "deploy", "quackcit.conf")
    for key, port in PORTS.items():
        env[f"QUACKCIT_PORT_{key}"] = str(port)
    pidfile = os.path.join(tmp, "quackcit.pid")
    try:
        for attempt in (1, 2):
            r = run(env, "start")
            assert r.returncode == 0, f"start #{attempt} failed:\n{r.stdout}\n{r.stderr}"
            with open(pidfile) as f:
                pid = int(f.read().strip())
            # Past the workers' first tick (capped at 10 s), which is when a
            # startup race would have fired.
            time.sleep(12)
            os.kill(pid, 0)  # raises if the server died

            with open(os.path.join(tmp, "quackcit.log")) as f:
                log = f.read()
            for prefix in ("cit", "qm_smtp_in", "qm_imap", "qm_telnet", "qm_ssh", "qm_ftp", "qm_nntp",
                           "qm_http", "qm_https"):
                assert f"{prefix}_start\ttrue" in log, f"start #{attempt}: {prefix} did not start:\n{log[-3000:]}"
            for worker in ("listserv", "fetch", "acme", "expire"):
                assert f"{worker}\ttrue" in log, f"start #{attempt}: worker {worker} did not start"

            r = run(env, "stop")
            assert r.returncode == 0, f"stop #{attempt} failed:\n{r.stdout}\n{r.stderr}"
            for _ in range(50):
                try:
                    os.kill(pid, 0)
                except ProcessLookupError:
                    break
                time.sleep(0.2)
            else:
                raise AssertionError(f"pid {pid} still running after stop")

        with open(os.path.join(tmp, "quackcit.err")) as f:
            errors = [ln for ln in f.read().splitlines()
                      if ln.strip() and "Loading resources from" not in ln and ln.strip() != "\x1b[00m"]
        assert not errors, "the error log is not empty:\n" + "\n".join(errors)
    finally:
        try:
            with open(pidfile) as f:
                os.kill(int(f.read().strip()), signal.SIGKILL)
        except (OSError, ValueError):
            pass
        shutil.rmtree(tmp, ignore_errors=True)

    print("OK test_deploy_start.py")


if __name__ == "__main__":
    main()
