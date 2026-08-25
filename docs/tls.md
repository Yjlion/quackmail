# TLS, authentication and certificates

## TLS & AUTH

TLS uses the **system OpenSSL** (`libssl-dev`); DuckDB's bundled mbedTLS is
crypto-only. SASL `PLAIN`/`LOGIN` (SMTP) is offered only after TLS; credentials
are verified against `quackmail_users` with a constant-time compare. The native
Citadel `USER`/`PASS` verify against the same table.

Passwords are stored with **scrypt** (RFC 7914, N=16384 r=8 p=1 — 16 MiB and
~80 ms per check), through OpenSSL's `EVP_PBE_scrypt`. Memory-hardness is the
point: PBKDF2 and bcrypt both fit in a GPU's registers, scrypt does not. The
`algo` column is self-describing (`scrypt$16384$8$1`), so the work factor can
be raised later without invalidating rows already stored at the old one.

This replaced a single round of salted SHA-256, which a leaked
`quackmail_users` table gave up at GPU speed. Rows written by the old scheme
**still verify**, and are rewritten with scrypt on their owner's next
successful sign-in — otherwise an existing install would keep the weak hash for
every account until somebody happened to change a password, which for most
accounts is never.

## Certificates

Three sources, in the order the server prefers them:

1. **Your own** — `QUACKCIT_TLS_CERT` and `QUACKCIT_TLS_KEY`. A Let's Encrypt
   `fullchain.pem`/`privkey.pem` pair obtained by some other tool works as-is.
   A path that does not resolve is a fatal error rather than a silent fallback:
   papering over a typo with a self-signed certificate would hand out the wrong
   identity for as long as nobody noticed.
2. **ACME** — obtained and renewed by the server itself, below.
3. **A self-signed pair**, minted on first start under `QUACKCIT_TLS_DIR` and
   reused by every restart afterwards, so a client that accepted the
   fingerprint keeps working. Nothing will trust it. `QUACKCIT_TLS_AUTOGEN=0`
   opts out, at the cost of every implicit-TLS listener being skipped.

## Automatic certificates (ACME)

`quackmail_spool` carries an RFC 8555 client. Turn it on in `quackcit.conf`:

```sh
: "${QUACKCIT_ACME_ENABLE:=1}"
: "${QUACKCIT_ACME_DOMAINS:=mail.example.com,www.example.com}"
: "${QUACKCIT_ACME_CONTACT:=admin@example.com}"
```

then `deploy/quackcit.sh restart` and watch it:

```bash
deploy/quackcitadm.sh acme status
```

**What the CA actually checks is reachability.** Every name must resolve to
this machine and reach it on **port 80**, where the server answers
`/.well-known/acme-challenge/`. Nothing else is verified and nothing else needs
to be open. That route is dispatched before the HTTPS redirect, so
`qm_web_force_https` does not have to be turned off for it — which matters,
because at first issuance the HTTPS listener is holding the self-signed
certificate you are trying to replace.

### Staging first, deliberately

`QUACKCIT_ACME_DIRECTORY` defaults to Let's Encrypt's **staging** endpoint,
whose certificates nothing trusts. That is not caution for its own sake: a
failed validation counts against a small per-hostname hourly limit on the
production endpoint, and the first run of a new install is the one most likely
to have DNS or a firewall wrong. Once `acme status` shows an issued
certificate, switch over and renew:

```sh
: "${QUACKCIT_ACME_DIRECTORY:=https://acme-v02.api.letsencrypt.org/directory}"
```
```bash
deploy/quackcitadm.sh acme renew web
```

Setting a contact address is what records agreement to the CA's terms of
service. That is the operator's act, not the server's, so without one no
account is created.

### Renewal, and how it reaches a running server

The `qm_acme` worker ticks hourly and renews anything within
`QUACKCIT_ACME_RENEW_DAYS` (30) of expiry. On success it asks **every listener
in the process** to re-read its certificate — IMAP, POP3, submission, the web,
all of them — by finding their `<prefix>_tls_reload` functions in the DuckDB
catalog. The catalog is already an authoritative register of what is loaded, so
there is no second list to keep in step.

A reload does not disturb the listening socket and does not drop a connection:
each accept re-reads the certificate, so the next connection gets the new one.
You can trigger it by hand:

```bash
deploy/quackcitadm.sh acme reload
```

A failure is deferred with exponential backoff to a one-day cap, and a
`rateLimited` response jumps straight to a day. A worker that retried a broken
order every tick would get the whole account throttled — including for the
names that were fine.

### Everything the CLI does

```
quackcitadm.sh acme status              certificates, expiry, last error
quackcitadm.sh acme account             directory, contact, key thumbprint
quackcitadm.sh acme order web <domains> queue an order
quackcitadm.sh acme renew web           renew now, ignoring the window
quackcitadm.sh acme run                 one pass of the worker's work
quackcitadm.sh acme reload              put a renewed certificate into service
quackcitadm.sh acme revoke web          revoke, and stop managing it
quackcitadm.sh acme forget web          stop managing it, leave it valid
```

The same is on `/admin/acme` in the web console.

![The certificates page](../screenshots/web-admin-acme.png)

### What it does not do

- **dns-01 and wildcards.** `dns.hpp` resolves; it does not update. Offering
  dns-01 would mean "publish this TXT record by hand in the next few minutes",
  which is not automation.
- **tls-alpn-01.** It needs a challenge certificate and an ALPN callback on
  every TLS listener — more OpenSSL surface than http-01 for the same result,
  given that the HTTP listener is already there.
- **External Account Binding.** One extra JWS nested inside the account
  request; it slots in without changing any of the shapes, but no public CA
  this is aimed at requires it.
- **ES256.** The account key is RSA and signs RS256. There is no ECDSA anywhere
  in this tree, and every ACME server accepts RS256.

### Private and lab CAs

`QUACKCIT_ACME_CA_BUNDLE` points the client at a CA bundle instead of the
system trust store, which is what makes step-ca or a lab Boulder work. There is
deliberately **no way to skip verification**: an ACME client that does not
authenticate the CA is not an ACME client, and an escape hatch would also mean
the verifying path — the one with SNI and a host name check — was the one never
exercised. The offline test in `test/integration/test_acme.py` mints its own CA
and uses this knob, so that path is what CI runs.
