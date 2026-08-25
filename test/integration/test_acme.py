#!/usr/bin/env python3
"""End-to-end test for the ACME client, against a fake CA on localhost.

No network. The test mints its own certificate authority, runs an HTTPS server
that speaks enough of RFC 8555 to issue a certificate, and points the client at
it with `qm_acme_ca_bundle`.

That last part is the design decision worth naming: **there is no "insecure"
switch**. An escape hatch that skips certificate verification is a production
footgun, and it would also mean the security-critical path — a verifying client
context, with SNI and a host name check — was the one path never exercised. A CA
bundle knob is not a test affordance either; it is what makes a private ACME
server (step-ca, a lab Boulder) usable.

What is asserted, in rough order of how quietly it would otherwise regress:

  * The http-01 challenge is answered **over plain HTTP with
    `qm_web_force_https` left on**. Every other request is redirected to
    https://c_fqdn, and at first issuance there is nothing there but the
    self-signed certificate we are trying to replace, so a challenge routed
    through the redirect cannot be answered.
  * The fake CA really fetches the token from the running listener and compares
    the key authorization, so the thumbprint has to be right.
  * A renewed certificate is picked up by a **running** listener without a
    restart: the test opens a TLS connection before and after and compares the
    serial the server presents.
  * The key on disk is mode 0600.
  * A second pass is a no-op, and a failing order is deferred with a future
    next_attempt rather than retried on the spot.
  * No private key appears in any qm_acme_* result.

Requires: pip install duckdb==1.5.4, and openssl on PATH.
Run after `make release`.
"""
import base64
import json
import os
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
HTTP_PORT = 18098      # the QuackCit listener the CA validates against
HTTPS_PORT = 18448     # the listener whose certificate must hot-reload
CA_PORT = 14431        # the fake ACME server


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


def openssl(*args, **kw):
    return subprocess.run(["openssl", *args], check=True, capture_output=True, **kw)


def make_ca(tmp):
    """A CA that is also the fake server's own TLS certificate, valid for localhost."""
    key = os.path.join(tmp, "ca.key")
    cert = os.path.join(tmp, "ca.pem")
    openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256", "-days", "2",
            "-keyout", key, "-out", cert, "-subj", "/CN=Test ACME CA",
            "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
            "-addext", "basicConstraints=critical,CA:TRUE")
    return key, cert


def self_signed(tmp, name):
    """The pair a real install starts with, so there is something to replace."""
    key = os.path.join(tmp, name + ".key")
    cert = os.path.join(tmp, name + ".pem")
    openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256", "-days", "2",
            "-keyout", key, "-out", cert, "-subj", "/CN=before",
            "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1")
    os.chmod(key, 0o600)
    return key, cert


def issue(tmp, ca_key, ca_cert, csr_b64url, names):
    """Sign a CSR the client sent, as a CA would."""
    der = base64.urlsafe_b64decode(csr_b64url + "=" * (-len(csr_b64url) % 4))
    csr_path = os.path.join(tmp, "req.der")
    with open(csr_path, "wb") as f:
        f.write(der)
    ext_path = os.path.join(tmp, "leaf.ext")
    with open(ext_path, "w") as f:
        f.write("subjectAltName=" + ",".join("DNS:" + n for n in names) + "\n")
    out = os.path.join(tmp, "leaf.pem")
    openssl("x509", "-req", "-in", csr_path, "-inform", "DER", "-CA", ca_cert,
            "-CAkey", ca_key, "-set_serial", str(int(time.time())), "-days", "30",
            "-sha256", "-extfile", ext_path, "-out", out)
    with open(out) as f:
        leaf = f.read()
    with open(ca_cert) as f:
        return leaf + f.read()      # a chain, as a CA returns


class FakeAcme(BaseHTTPRequestHandler):
    """Enough of RFC 8555 to issue one certificate."""

    base = ""
    tmp = ""
    ca_key = ""
    ca_cert = ""
    state = {}

    def log_message(self, *a):
        pass

    # ---- helpers ---------------------------------------------------------

    def _send(self, code, body=b"", ctype="application/json", extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Replay-Nonce", base64.urlsafe_b64encode(
            os.urandom(16)).decode().rstrip("="))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _json(self, code, obj, extra=None):
        self._send(code, json.dumps(obj).encode(), extra=extra)

    def _payload(self):
        n = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(n)
        jws = json.loads(raw)
        prot = json.loads(base64.urlsafe_b64decode(
            jws["protected"] + "=" * (-len(jws["protected"]) % 4)))
        FakeAcme.state.setdefault("protected", []).append(prot)
        if not jws["payload"]:
            return prot, None                      # POST-as-GET
        return prot, json.loads(base64.urlsafe_b64decode(
            jws["payload"] + "=" * (-len(jws["payload"]) % 4)))

    # ---- routes ----------------------------------------------------------

    def do_HEAD(self):
        self._send(200)

    def do_GET(self):
        if self.path == "/directory":
            self._json(200, {
                "newNonce": self.base + "/newNonce",
                "newAccount": self.base + "/newAccount",
                "newOrder": self.base + "/newOrder",
                "revokeCert": self.base + "/revokeCert",
                "meta": {"termsOfService": self.base + "/terms"},
            })
            return
        self._send(404, b"{}")

    def do_POST(self):
        st = FakeAcme.state
        try:
            prot, payload = self._payload()
        except Exception as e:                       # noqa: BLE001
            self._json(400, {"type": "urn:ietf:params:acme:error:malformed",
                             "detail": str(e)})
            return

        if self.path == "/newAccount":
            # The account is bound to the JWK, and the thumbprint the client
            # will answer challenges with is derived from it here — so a
            # mis-ordered JWK fails the challenge below rather than silently.
            jwk = prot.get("jwk")
            assert jwk, "newAccount must be signed with a jwk, not a kid"
            canon = json.dumps({k: jwk[k] for k in sorted(jwk)}, separators=(",", ":"))
            import hashlib
            st["thumbprint"] = base64.urlsafe_b64encode(
                hashlib.sha256(canon.encode()).digest()).decode().rstrip("=")
            st["tos"] = payload.get("termsOfServiceAgreed")
            st["contact"] = payload.get("contact")
            self._json(201, {"status": "valid"},
                       extra={"Location": self.base + "/acct/1"})
            return

        if self.path == "/newOrder":
            assert prot.get("kid"), "an order must be signed with the account URL"
            st["names"] = [i["value"] for i in payload["identifiers"]]
            st["authz_done"] = False
            st["finalized"] = False
            self._json(201, {
                "status": "pending",
                "identifiers": payload["identifiers"],
                "authorizations": [self.base + "/authz/1"],
                "finalize": self.base + "/finalize",
            }, extra={"Location": self.base + "/order/1"})
            return

        if self.path == "/authz/1":
            self._json(200, {
                "status": "valid" if st.get("authz_done") else "pending",
                "identifier": {"type": "dns", "value": st["names"][0]},
                "challenges": [{"type": "http-01",
                                "url": self.base + "/chal/1",
                                "token": st.setdefault("token", "tok-" + base64.urlsafe_b64encode(
                                    os.urandom(24)).decode().rstrip("=")),
                                "status": "valid" if st.get("authz_done") else "pending"}],
            })
            return

        if self.path == "/chal/1":
            # Actually go and look. This is the assertion that matters: the
            # challenge has to be reachable over **plain HTTP**, through the
            # listener, with the redirect left on.
            url = f"http://{HOST}:{HTTP_PORT}/.well-known/acme-challenge/{st['token']}"
            want = st["token"] + "." + st["thumbprint"]
            try:
                with urllib.request.urlopen(url, timeout=10) as r:
                    got = r.read().decode().strip()
                    st["validated_status"] = r.status
            except urllib.error.HTTPError as e:
                got = f"<HTTP {e.code}>"
                st["validated_status"] = e.code
            except Exception as e:                   # noqa: BLE001
                got = f"<{e}>"
                st["validated_status"] = 0
            st["served"] = got
            st["authz_done"] = (got == want)
            if not st["authz_done"]:
                self._json(200, {"status": "invalid", "type": "http-01"})
                return
            self._json(200, {"status": "valid", "type": "http-01"})
            return

        if self.path == "/finalize":
            if st.get("fail_finalize"):
                self._json(403, {"type": "urn:ietf:params:acme:error:serverInternal",
                                 "detail": "deliberate failure"})
                return
            st["chain"] = issue(self.tmp, self.ca_key, self.ca_cert,
                                payload["csr"], st["names"])
            st["finalized"] = True
            self._json(200, {"status": "valid", "certificate": self.base + "/cert/1"},
                       extra={"Location": self.base + "/order/1"})
            return

        if self.path == "/order/1":
            self._json(200, {
                "status": "valid" if st.get("finalized") else "pending",
                "certificate": self.base + "/cert/1" if st.get("finalized") else "",
                "finalize": self.base + "/finalize",
                "authorizations": [self.base + "/authz/1"],
            })
            return

        if self.path == "/cert/1":
            self._send(200, st["chain"].encode(), ctype="application/pem-certificate-chain")
            return

        self._json(404, {"type": "urn:ietf:params:acme:error:malformed"})


def peer_serial(port):
    """The serial of the certificate the listener is presenting right now."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    with socket.create_connection((HOST, port), timeout=10) as raw:
        with ctx.wrap_socket(raw, server_hostname="localhost") as s:
            der = s.getpeercert(binary_form=True)
    out = subprocess.run(["openssl", "x509", "-inform", "DER", "-noout", "-serial"],
                         input=der, capture_output=True, check=True)
    return out.stdout.decode().strip()


def main():
    if not shutil.which("openssl"):
        print("test_acme: openssl is not on PATH; skipping")
        return
    tmp = tempfile.mkdtemp(prefix="qm-acme-")
    try:
        ca_key, ca_cert = make_ca(tmp)
        cert_dir = os.path.join(tmp, "certs")
        os.makedirs(cert_dir, exist_ok=True)
        before_key, before_cert = self_signed(cert_dir, "web")

        FakeAcme.base = f"https://localhost:{CA_PORT}"
        FakeAcme.tmp = tmp
        FakeAcme.ca_key = ca_key
        FakeAcme.ca_cert = ca_cert
        FakeAcme.state = {}

        httpd = ThreadingHTTPServer((HOST, CA_PORT), FakeAcme)
        sctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        sctx.load_cert_chain(ca_cert, ca_key)
        httpd.socket = sctx.wrap_socket(httpd.socket, server_side=True)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()

        con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
        for name in ("quackmail", "quackmail_http", "quackmail_spool"):
            con.execute(f"LOAD '{ext(name)}'")
        con.execute("SELECT count(*) FROM qm_status()").fetchall()

        con.execute("CALL qm_config_set('c_fqdn', 'localhost')")
        # Deliberately ON. The challenge must answer anyway.
        con.execute("CALL qm_config_set('qm_web_force_https', '1')")
        con.execute("CALL qm_config_set('qm_acme_enabled', '1')")
        con.execute(f"CALL qm_config_set('qm_acme_directory', 'https://localhost:{CA_PORT}/directory')")
        con.execute(f"CALL qm_config_set('qm_acme_ca_bundle', '{ca_cert}')")
        con.execute(f"CALL qm_config_set('qm_acme_cert_dir', '{cert_dir}')")
        con.execute("CALL qm_config_set('qm_acme_contact', 'admin@example.com')")
        con.execute("CALL qm_config_set('qm_acme_tos_agreed', '1')")
        # A one-day renewal window against a thirty-day certificate, so the
        # second pass below is genuinely "nothing is due" rather than "not yet
        # implemented".
        con.execute("CALL qm_config_set('qm_acme_renew_days', '1')")

        note = con.execute(f"SELECT note FROM qm_http_start('{HOST}', {HTTP_PORT})").fetchone()[0]
        assert note == "started", f"qm_http: {note}"
        note = con.execute(
            "SELECT note FROM qm_https_start(?, ?, implicit_tls => true, tls_cert => ?, tls_key => ?)",
            [HOST, HTTPS_PORT, before_cert, before_key]).fetchone()[0]
        assert note == "started", f"qm_https: {note}"
        time.sleep(0.4)

        serial_before = peer_serial(HTTPS_PORT)

        # ---- the order -----------------------------------------------------
        ok, msg = con.execute("SELECT ok, note FROM qm_acme_order('web', 'localhost')").fetchone()
        assert ok, f"queueing the order failed: {msg}"

        rows = con.execute("SELECT name, status, note, not_after FROM qm_acme_run()").fetchall()
        assert rows, "the pass produced no rows"
        name, status, note, not_after = rows[0]
        assert status == "issued", f"the order ended as {status!r}: {note}"
        assert not_after > time.time(), f"the certificate is already expired: {not_after}"

        # The CA actually fetched the token, over plain HTTP, with the redirect on.
        st = FakeAcme.state
        assert st.get("validated_status") == 200, \
            (f"the challenge was served with status {st.get('validated_status')} "
             f"(body {st.get('served')!r}) — qm_web_force_https must not apply to it")
        assert st["served"] == st["token"] + "." + st["thumbprint"], \
            f"the key authorization did not match: served {st['served']!r}"
        assert st.get("tos") is True, "termsOfServiceAgreed was not sent"
        assert st.get("contact") == ["mailto:admin@example.com"], \
            f"the contact was not sent: {st.get('contact')}"

        # Exactly one request is signed with a bare JWK; the rest use the kid.
        jwk_posts = [p for p in st["protected"] if "jwk" in p]
        assert len(jwk_posts) == 1, \
            f"{len(jwk_posts)} requests carried a jwk; only newAccount should"
        assert all("nonce" in p and "url" in p for p in st["protected"]), \
            "every protected header must carry a nonce and a url"
        assert all(p.get("alg") == "RS256" for p in st["protected"]), "alg must be RS256"

        # ---- what landed on disk -------------------------------------------
        cert_path = os.path.join(cert_dir, "web.pem")
        key_path = os.path.join(cert_dir, "web.key")
        assert os.path.exists(cert_path) and os.path.exists(key_path), "no files were written"
        mode = os.stat(key_path).st_mode & 0o777
        assert mode == 0o600, f"the private key is mode {oct(mode)}, not 0600"
        with open(cert_path) as f:
            assert f.read().count("BEGIN CERTIFICATE") >= 2, "the chain was not stored whole"

        # ---- hot reload ------------------------------------------------------
        reloads = con.execute("SELECT function_name, note FROM qm_acme_reload()").fetchall()
        names = {r[0] for r in reloads}
        assert "qm_https_tls_reload" in names, f"no https listener was found: {names}"
        by_name = dict(reloads)
        assert by_name["qm_https_tls_reload"] == "reloaded", \
            f"the https listener reported {by_name['qm_https_tls_reload']!r}"
        # A listener that was started without certificate paths says so rather
        # than pretending, and one that is not running is not an error.
        assert by_name.get("qm_http_tls_reload", "").startswith(("not running", "error")), \
            f"the plaintext listener reported {by_name.get('qm_http_tls_reload')!r}"

        serial_after = peer_serial(HTTPS_PORT)
        assert serial_after != serial_before, \
            "the running listener is still presenting the old certificate"

        # ---- idempotence and backoff ----------------------------------------
        rows = con.execute("SELECT status, note FROM qm_acme_run()").fetchall()
        assert rows[0][0] == "idle", f"a second pass was not a no-op: {rows}"

        FakeAcme.state["fail_finalize"] = True
        con.execute("SELECT ok FROM qm_acme_order('bad', 'localhost')").fetchone()
        rows = con.execute("SELECT name, status, note FROM qm_acme_run(name := 'bad')").fetchall()
        assert rows and rows[0][1] == "error", f"a failing order reported {rows}"
        nxt, attempts = con.execute(
            "SELECT next_attempt, attempts FROM quackmail_acme_orders WHERE name = 'bad'").fetchone()
        assert attempts == 1, f"the failure was not counted: attempts={attempts}"
        assert nxt > time.time() + 60, \
            f"a failed order was not deferred (next_attempt is {nxt - time.time():.0f}s away)"

        # ---- no key ever comes back out --------------------------------------
        for sql in ("SELECT * FROM qm_acme_certs()", "SELECT * FROM qm_acme_account()",
                    "SELECT * FROM qm_acme_run(name := 'nothing')"):
            text = str(con.execute(sql).fetchall())
            assert "PRIVATE KEY" not in text, f"a private key came back from {sql}"
        thumb = con.execute("SELECT thumbprint FROM qm_acme_account()").fetchone()[0]
        assert thumb == st["thumbprint"], \
            f"qm_acme_account reports {thumb!r}, the CA saw {st['thumbprint']!r}"

        # A challenge row does not outlive its order.
        left = con.execute("SELECT count(*) FROM quackmail_acme_challenges").fetchone()[0]
        assert left == 0, f"{left} challenge rows were left behind"

        # ---- the responder refuses what it should ----------------------------
        base = f"http://{HOST}:{HTTP_PORT}/.well-known/acme-challenge/"
        for path, why in ((base + "short", "a token that is too short"),
                          (base + "a" * 200, "a token that is too long"),
                          (base + "has/a/slash", "a token with path segments"),
                          (base + "unknown-but-well-shaped-token", "an unknown token")):
            try:
                with urllib.request.urlopen(path, timeout=5) as r:
                    code = r.status
            except urllib.error.HTTPError as e:
                code = e.code
            assert code == 404, f"{why} returned {code}, not 404"

        print("test_acme: OK")
    finally:
        try:
            con.execute("CALL qm_http_stop()")
            con.execute("CALL qm_https_stop()")
        except Exception:
            pass
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
