#!/usr/bin/env python3
"""End-to-end test for the SSH listener (qm_ssh, in quackmail_telnet).

Driven by the system OpenSSH `ssh` and `sftp` binaries rather than a Python
client: the transport is written from the RFCs on top of OpenSSL, so the thing
worth proving is that a real, current client agrees with it — the key exchange,
every cipher and MAC it offers, every key type a user can register, and the
strict-kex handshake. paramiko is exercised as a second implementation when it
is installed (it has no AEAD cipher, so it is what covers aes-ctr + HMAC).

What this pins:

  * `ssh user@host` reaches the BBS shell with no name/password prompt, and
    `joe_user` reaches "Joe User".
  * publickey login with ed25519, ecdsa-p256/p384/p521 and RSA keys, each
    registered through qm_sshkey_add; an unregistered key is refused.
  * password login, and its refusal once qm_ssh_allow_password = 0.
  * SFTP over the file areas: ls, put, get, rename, rm, mkdir/rmdir, with the
    bytes landing in the same room FTP and WebDAV serve.
  * the host key is stable across a listener restart (it lives in the
    database), so a client's known_hosts entry stays valid.

Requires: pip install duckdb==1.5.4; OpenSSH client binaries on PATH.
Run after `make` so the loadable extensions exist under build/release/extension.
"""
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import time

try:
    import duckdb
except ImportError:
    print("SKIP test_ssh.py: duckdb module not installed")
    sys.exit(0)

if not shutil.which("ssh") or not shutil.which("sftp") or not shutil.which("ssh-keygen"):
    print("SKIP test_ssh.py: OpenSSH client binaries not installed")
    sys.exit(0)

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
PORT = 12222
USER = "sshuser"
PASSWORD = "sekrit-pass"
SPACED = "Joe User"


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


class Env:
    def __init__(self, tmp):
        self.tmp = tmp
        self.known_hosts = os.path.join(tmp, "known_hosts")
        self.askpass = os.path.join(tmp, "askpass.sh")

    def base(self, *extra):
        return [
            "-o", "BatchMode=no",
            "-o", f"UserKnownHostsFile={self.known_hosts}",
            "-o", "StrictHostKeyChecking=yes",
            "-o", "GlobalKnownHostsFile=/dev/null",
            "-o", "IdentitiesOnly=yes",
            "-o", "ConnectTimeout=10",
            "-o", "LogLevel=ERROR",
            "-F", "/dev/null",
            *extra,
        ]

    def env(self, password=None):
        e = dict(os.environ)
        e.pop("SSH_AUTH_SOCK", None)
        if password is not None:
            with open(self.askpass, "w") as f:
                f.write(f"#!/bin/sh\necho '{password}'\n")
            os.chmod(self.askpass, stat.S_IRWXU)
            e["SSH_ASKPASS"] = self.askpass
            e["SSH_ASKPASS_REQUIRE"] = "force"
            e["DISPLAY"] = ":0"
        else:
            e["SSH_ASKPASS_REQUIRE"] = "never"
        return e

    def sftp(self, user, batch, key=None, password=None, extra=()):
        opts = self.base(*extra)
        if key:
            opts += ["-i", key, "-o", "PasswordAuthentication=no", "-o", "KbdInteractiveAuthentication=no"]
        else:
            opts += ["-o", "PubkeyAuthentication=no"]
        path = os.path.join(self.tmp, "batch")
        with open(path, "w") as f:
            f.write(batch)
        cmd = ["sftp", "-P", str(PORT), *opts, "-b", path, f"{user}@{HOST}"]
        return subprocess.run(cmd, capture_output=True, text=True, timeout=60, env=self.env(password),
                              cwd=self.tmp, stdin=subprocess.DEVNULL)

    def shell(self, user, keystrokes, key=None, password=None):
        opts = self.base()
        if key:
            opts += ["-i", key, "-o", "PasswordAuthentication=no"]
        else:
            opts += ["-o", "PubkeyAuthentication=no"]
        cmd = ["ssh", "-tt", "-p", str(PORT), *opts, f"{user}@{HOST}"]
        p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             env=self.env(password))
        out = b""
        try:
            # Give the shell time to draw its prompt before each keystroke, the
            # way a person would; the BBS reads one key at a time with a pty.
            time.sleep(1.5)
            for k in keystrokes:
                p.stdin.write(k.encode())
                p.stdin.flush()
                time.sleep(0.4)
            out, err = p.communicate(timeout=20)
        except subprocess.TimeoutExpired:
            p.kill()
            out, err = p.communicate()
        return p.returncode, out.decode("utf-8", "replace"), err.decode("utf-8", "replace")


def keygen(tmp, name, *args):
    path = os.path.join(tmp, name)
    subprocess.run(["ssh-keygen", "-q", "-N", "", "-C", f"{name}@test", "-f", path, *args], check=True)
    with open(path + ".pub") as f:
        return path, f.read().strip()


def main():
    tmp = tempfile.mkdtemp(prefix="qc-ssh-")
    env = Env(tmp)
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    for name in ("quackmail", "quackmail_citadel", "quackmail_telnet", "quackmail_ftp"):
        con.execute(f"LOAD '{ext(name)}'")
    con.execute(f"CALL qm_user_add('{USER}', '{PASSWORD}')")
    con.execute(f"CALL qm_user_add('{SPACED}', '{PASSWORD}')")
    con.execute("CALL cit_room_add('Uploads')")
    con.execute("UPDATE citadel_rooms SET qr_flags = qr_flags | 32 | 64 | 128 | 256 "
                "WHERE display_name = 'Uploads'")

    # ---- keys ---------------------------------------------------------------
    keys = {
        "ed25519": keygen(tmp, "id_ed25519", "-t", "ed25519"),
        "ecdsa256": keygen(tmp, "id_ecdsa256", "-t", "ecdsa", "-b", "256"),
        "ecdsa384": keygen(tmp, "id_ecdsa384", "-t", "ecdsa", "-b", "384"),
        "ecdsa521": keygen(tmp, "id_ecdsa521", "-t", "ecdsa", "-b", "521"),
        "rsa": keygen(tmp, "id_rsa", "-t", "rsa", "-b", "3072"),
    }
    stranger, _ = keygen(tmp, "id_stranger", "-t", "ed25519")
    small_rsa, small_rsa_pub = keygen(tmp, "id_small", "-t", "rsa", "-b", "1024")
    for kind, (_, pub) in keys.items():
        ok, note, fp = con.execute("SELECT * FROM qm_sshkey_add(?, ?)", [USER, pub]).fetchone()
        assert ok, f"{kind} key refused: {note}"
        assert fp.startswith("SHA256:"), fp
    ok, note, _ = con.execute("SELECT * FROM qm_sshkey_add(?, ?)", [USER, keys["ed25519"][1]]).fetchone()
    assert not ok and "already" in note, f"a duplicate key was accepted: {note}"
    ok, note, _ = con.execute("SELECT * FROM qm_sshkey_add(?, ?)", [SPACED, keys["ed25519"][1]]).fetchone()
    assert not ok and "another user" in note, f"one key was registered to two users: {note}"
    ok, note, _ = con.execute("SELECT * FROM qm_sshkey_add(?, ?)", [USER, small_rsa_pub]).fetchone()
    assert not ok and "2048" in note, f"a 1024-bit RSA key was accepted: {note}"
    ok, note, _ = con.execute("SELECT * FROM qm_sshkey_add(?, ?)",
                              [USER, 'command="/bin/sh" ' + keys["ed25519"][1]]).fetchone()
    assert not ok and "options" in note, f"an authorized_keys option was silently dropped: {note}"
    listed = con.execute("SELECT type FROM qm_sshkeys(?) ORDER BY type", [USER]).fetchall()
    assert len(listed) == 5, listed

    # ---- the listener and its host key ---------------------------------------
    note = con.execute(f"SELECT note FROM qm_ssh_start('{HOST}', {PORT})").fetchone()[0]
    assert note == "started", f"listener did not start: {note}"
    pub, fp, _ = con.execute("SELECT * FROM qm_ssh_hostkey()").fetchone()
    assert pub.startswith("ssh-ed25519 "), pub
    with open(env.known_hosts, "w") as f:
        f.write(f"[{HOST}]:{PORT} {pub}\n")
    time.sleep(0.3)

    try:
        # ---- SFTP with each key type ------------------------------------------
        for kind, (path, _) in keys.items():
            r = env.sftp(USER, "ls /\n", key=path)
            assert r.returncode == 0, f"{kind}: sftp failed: {r.stderr}"
            assert "Uploads" in r.stdout, f"{kind}: the file area is not in /: {r.stdout}"
        # RSA must work with both SHA-2 signature algorithms, and SHA-1 is refused.
        for alg in ("rsa-sha2-256", "rsa-sha2-512"):
            r = env.sftp(USER, "ls /\n", key=keys["rsa"][0], extra=("-o", f"PubkeyAcceptedAlgorithms={alg}"))
            assert r.returncode == 0, f"RSA with {alg} failed: {r.stderr}"
        r = env.sftp(USER, "ls /\n", key=keys["rsa"][0], extra=("-o", "PubkeyAcceptedAlgorithms=ssh-rsa"))
        assert r.returncode != 0, "a SHA-1 ssh-rsa signature was accepted"

        r = env.sftp(USER, "ls /\n", key=stranger)
        assert r.returncode != 0, "an unregistered key logged in"
        used = con.execute("SELECT count(*) FROM qm_sshkeys(?) WHERE last_used > 0", [USER]).fetchone()[0]
        assert used == 5, f"last_used was not stamped on every key that logged in: {used}"

        # ---- every cipher and MAC ---------------------------------------------
        for cipher in ("chacha20-poly1305@openssh.com", "aes256-gcm@openssh.com", "aes128-gcm@openssh.com"):
            r = env.sftp(USER, "ls /\n", key=keys["ed25519"][0], extra=("-c", cipher))
            assert r.returncode == 0, f"{cipher}: {r.stderr}"
        for cipher in ("aes256-ctr", "aes128-ctr"):
            for mac in ("hmac-sha2-256-etm@openssh.com", "hmac-sha2-512-etm@openssh.com", "hmac-sha2-256",
                        "hmac-sha2-512"):
                r = env.sftp(USER, "ls /\n", key=keys["ed25519"][0], extra=("-c", cipher, "-o", f"MACs={mac}"))
                assert r.returncode == 0, f"{cipher}/{mac}: {r.stderr}"
        # A cipher we do not offer fails the handshake rather than falling back.
        r = env.sftp(USER, "ls /\n", key=keys["ed25519"][0], extra=("-c", "aes128-cbc"))
        assert r.returncode != 0, "negotiated a cipher that is not on the server's list"

        # ---- password login, and switching it off ------------------------------
        r = env.sftp(USER, "ls /\n", password=PASSWORD)
        assert r.returncode == 0, f"password login failed: {r.stderr}"
        r = env.sftp(USER, "ls /\n", password="wrong")
        assert r.returncode != 0, "a wrong password was accepted"
        con.execute("CALL qm_config_set('qm_ssh_allow_password', '0')")
        r = env.sftp(USER, "ls /\n", password=PASSWORD)
        assert r.returncode != 0, "password login still worked with qm_ssh_allow_password = 0"
        r = env.sftp(USER, "ls /\n", key=keys["ed25519"][0])
        assert r.returncode == 0, f"key login broke when passwords were turned off: {r.stderr}"
        con.execute("CALL qm_config_set('qm_ssh_allow_password', '1')")

        # ---- the user-name mapping --------------------------------------------
        for typed in ("joe_user", "JOE.USER", "Joe_User"):
            r = env.sftp(typed, "ls /\n", password=PASSWORD)
            assert r.returncode == 0, f"{typed!r} did not reach {SPACED!r}: {r.stderr}"
        r = env.sftp("nobody_here", "ls /\n", password=PASSWORD)
        assert r.returncode != 0, "an unknown user logged in"

        # ---- SFTP file operations ---------------------------------------------
        blob = bytes(range(256)) * 300
        local = os.path.join(tmp, "blob.bin")
        with open(local, "wb") as f:
            f.write(blob)
        batch = (
            "cd /Uploads\n"
            "put blob.bin\n"
            "ls -l\n"
            "get blob.bin back.bin\n"
            "rename blob.bin moved.bin\n"
            "ls\n"
        )
        r = env.sftp(USER, batch, key=keys["ed25519"][0])
        assert r.returncode == 0, f"sftp batch failed: {r.stdout}\n{r.stderr}"
        with open(os.path.join(tmp, "back.bin"), "rb") as f:
            assert f.read() == blob, "the downloaded file differs from the upload"
        assert "moved.bin" in r.stdout, r.stdout

        # The file is a message in the room — the same one FTP and WebDAV see.
        row = con.execute(
            "SELECT count(*) FROM citadel_messages m JOIN citadel_room_msgs rm ON rm.msgnum = m.msgnum "
            "JOIN citadel_rooms r ON r.room_num = rm.room_num "
            "WHERE r.display_name = 'Uploads' AND m.euid = 'file/moved.bin'").fetchone()[0]
        assert row == 1, "the SFTP upload is not a file in the Uploads room"

        r = env.sftp(USER, "cd /Uploads\nrm moved.bin\nls\n", key=keys["ed25519"][0])
        # sftp echoes each batch command, so look only at the listing lines.
        listing = [ln for ln in r.stdout.splitlines() if not ln.startswith("sftp>")]
        assert r.returncode == 0 and not any("moved.bin" in ln for ln in listing), r.stdout + r.stderr
        r = env.sftp(USER, "get /Uploads/nothing-here.txt\n", key=keys["ed25519"][0])
        assert r.returncode != 0, "fetching a missing file succeeded"
        r = env.sftp(USER, "mkdir /Uploads/sub\n", key=keys["ed25519"][0])
        assert r.returncode != 0, "a subdirectory was created inside a file area"

        # mkdir/rmdir of an area is room creation, so it needs the aide level.
        con.execute(f"CALL qm_config_set('qm_room_create_axlevel', '0')")
        r = env.sftp(USER, "mkdir /Fresh Area\nls /\nrmdir /Fresh Area\nls /\n", key=keys["ed25519"][0])
        assert r.returncode == 0, f"mkdir/rmdir of an area failed: {r.stdout}\n{r.stderr}"
        assert r.stdout.count("Fresh Area") >= 1, r.stdout

        # ---- the shell --------------------------------------------------------
        code, out, err = env.shell(USER, ["T"], key=keys["ed25519"][0])
        assert "Welcome, sshuser." in out, f"no welcome from the shell: {out!r} {err!r}"
        assert "Password:" not in out and "Enter your name" not in out, "the shell asked for credentials"
        assert "Goodbye." in out, f"the shell did not end on T: {out!r}"
        code, out, err = env.shell("joe_user", ["T"], password=PASSWORD)
        assert f"Welcome, {SPACED}." in out, f"joe_user did not become {SPACED}: {out!r} {err!r}"

        # ---- the host key survives a restart ----------------------------------
        con.execute("CALL qm_ssh_stop()")
        note = con.execute(f"SELECT note FROM qm_ssh_start('{HOST}', {PORT})").fetchone()[0]
        assert note == "started", note
        time.sleep(0.3)
        r = env.sftp(USER, "ls /\n", key=keys["ed25519"][0])
        assert r.returncode == 0, f"host key changed across a restart: {r.stderr}"

        # ---- removing a key revokes it -----------------------------------------
        fp_ed = con.execute("SELECT fingerprint FROM qm_sshkeys(?) WHERE type = 'ssh-ed25519'", [USER]).fetchone()[0]
        ok, _ = con.execute("SELECT * FROM qm_sshkey_remove(?, ?)", [USER, fp_ed]).fetchone()
        assert ok
        r = env.sftp(USER, "ls /\n", key=keys["ed25519"][0])
        assert r.returncode != 0, "a removed key still logs in"

        # ---- paramiko: a second client implementation --------------------------
        try:
            import paramiko
        except ImportError:
            print("  (paramiko not installed; skipping the second-client check)")
        else:
            t = paramiko.Transport((HOST, PORT))
            t.connect(username=USER, password=PASSWORD)
            sf = paramiko.SFTPClient.from_transport(t)
            assert "Uploads" in sf.listdir("/"), sf.listdir("/")
            with sf.open("/Uploads/para.txt", "wb") as f:
                f.write(b"hello from paramiko\n")
            with sf.open("/Uploads/para.txt", "rb") as f:
                assert f.read() == b"hello from paramiko\n"
            sf.remove("/Uploads/para.txt")
            t.close()
    finally:
        con.execute("CALL qm_ssh_stop()")
        shutil.rmtree(tmp, ignore_errors=True)

    print("OK test_ssh.py")


if __name__ == "__main__":
    main()
