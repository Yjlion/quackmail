---
name: parity-oracle
description: Set up and use the real Citadel Groupware server as the behavioral oracle for QuackCit — the docker container to run, the cgit source-reading trick, port conventions, and the debian.lan status. Use when comparing QuackCit's protocol behavior against real Citadel or when you need Citadel's own source for reference.
---

A **real Citadel Groupware server** is the oracle. Source is the spec; live
probes are the acceptance test.

Get one anywhere with docker — this is the default:

```bash
docker run -d --name citadel -p 10504:504 -p 10025:25 -p 10110:110 \
  -p 10143:143 -p 10119:119 -p 10522:5222 -p 10080:80 \
  -v citadel-data:/citadel-data citadeldotorg/citadel
docker exec citadel /usr/local/citadel/sendcommand -h/citadel-data 'AGUP admin'
```

Real port + 10000, so nothing collides with QuackCit's dev ports (citadel 5040,
smtp 2525, submission 2587, submissions 2465, pop3 1110, pop3s 1995, imap 1143,
imaps 1993, xmpp 15222) or with the integration tests. `admin`/`citadel` exists
already; `sendcommand` needs `-h/citadel-data` or it looks in the wrong place.

If the `ai` user is not in the `docker` group, every command above needs
`sudo` (`sudo docker exec citadel ...`).

The container has **no source tree**, and neither does this machine. Read the
source over HTTP from **`https://code.citadel.org/citadel.git`** (cgit), which
serves the whole tree, history and patches:

```bash
curl -sS -b 'cgit_access=verified' \
  https://code.citadel.org/citadel.git/plain/libcitadel/lib/libcitadel.h
```

The `-b` cookie is not optional — the site gates on a JavaScript check that sets
it, and without it every path returns the challenge page instead of the file.
`citadel/server/modules/<proto>/` holds the protocol servers, `textclient/` the
BBS client, `webcit/` and `webcit-ng/` the web clients, and
`libcitadel/lib/libcitadel.h` the shared constants and enums.

`debian.lan`, named throughout [MEMORY.md](../../../MEMORY.md) as a second oracle
carrying a checkout at `/root/citadel`, **no longer resolves** — do not spend a
session trying to reach it. The `LD_PRELOAD` trick for pointing the official
client at QuackCit is still recorded there and still works.

The box is a disposable test machine: restarting Citadel and creating test
rooms/users on it is fine and expected when exercising admin features.
