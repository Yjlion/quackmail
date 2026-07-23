#!/usr/bin/env python3
"""End-to-end test for the outbound relay drainer (smtp_out).

Enqueues a message in quackmail_outbound, points the relay drainer at a local
throwaway SMTP sink (a smarthost), and asserts the drainer delivers it and marks
the row 'sent'. This exercises the queue -> drain -> outbound-SMTP-client path
without touching the internet.

Requires: pip install duckdb==1.5.4. Run after `make`.
"""
import os
import socket
import threading
import time

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
SINK_PORT = 3600

RAW = "From: alice@quackmail.test\r\nTo: bob@example.com\r\nSubject: relayme\r\n\r\nbody\r\n"


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


class Sink:
    """A one-shot minimal SMTP sink running on a background thread."""

    def __init__(self, port):
        self.port = port
        self.received = threading.Event()
        self._stop = False
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind((HOST, port))
        self.srv.listen(4)
        self.srv.settimeout(0.5)
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self.thread.start()

    def stop(self):
        self._stop = True
        self.thread.join(timeout=3)
        self.srv.close()

    def _run(self):
        while not self._stop:
            try:
                conn, _ = self.srv.accept()
            except socket.timeout:
                continue
            try:
                self._serve(conn)
            except Exception:
                pass
            finally:
                conn.close()

    def _serve(self, conn):
        buf = b""

        def readline():
            nonlocal buf
            while b"\r\n" not in buf:
                chunk = conn.recv(4096)
                if not chunk:
                    return None
                buf += chunk
            line, buf = buf.split(b"\r\n", 1)
            return line

        conn.sendall(b"220 sink\r\n")
        while True:
            line = readline()
            if line is None:
                return
            up = line.upper()
            if up.startswith(b"EHLO") or up.startswith(b"HELO"):
                conn.sendall(b"250 sink\r\n")
            elif up.startswith(b"MAIL") or up.startswith(b"RCPT") or up.startswith(b"RSET"):
                conn.sendall(b"250 OK\r\n")
            elif up.startswith(b"DATA"):
                conn.sendall(b"354 go\r\n")
                while True:
                    dl = readline()
                    if dl is None or dl == b".":
                        break
                self.received.set()
                conn.sendall(b"250 OK: sink accepted\r\n")
            elif up.startswith(b"QUIT"):
                conn.sendall(b"221 bye\r\n")
                return
            else:
                conn.sendall(b"250 OK\r\n")


def main():
    sink = Sink(SINK_PORT)
    sink.start()

    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{ext('quackmail')}'")
    con.execute(f"LOAD '{ext('quackmail_smtp_out')}'")

    con.execute(
        "INSERT INTO quackmail_outbound (from_addr, rcpt, raw, status) "
        "VALUES ('alice@quackmail.test', 'bob@example.com', $1::BLOB, 'queued')",
        [RAW],
    )

    note = con.execute(
        "SELECT note FROM qm_smtp_relay_start(poll_secs=>1, smarthost_host=>'127.0.0.1', "
        f"smarthost_port=>{SINK_PORT})"
    ).fetchone()[0]
    assert note == "started", f"relay did not start: {note}"

    try:
        got = sink.received.wait(timeout=15)
        assert got, "sink never received the relayed message"
        # Give the drainer a moment to record the terminal state.
        for _ in range(20):
            status = con.execute(
                "SELECT status FROM quackmail_outbound WHERE rcpt = 'bob@example.com'"
            ).fetchone()[0]
            if status == "sent":
                break
            time.sleep(0.25)
        assert status == "sent", f"expected status 'sent', got '{status}'"
    finally:
        con.execute("CALL qm_smtp_relay_stop()").fetchall()
        sink.stop()

    print("PASS: relay drainer delivered a queued message to the smarthost and marked it sent")


if __name__ == "__main__":
    main()
