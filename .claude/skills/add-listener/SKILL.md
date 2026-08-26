---
name: add-listener
description: Add a new network listener (protocol front-end) to QuackCit — declaring a ServerController, writing the Handle function, and registering start/stop/status controls. Use when adding a new protocol server or a new port for an existing protocol.
---

1. Declare a global `ServerController`.
2. Write `void Handle(DatabaseInstance &db, net::ClientStream &stream)`. If the
   handler needs STARTTLS, give it a `ServerController &` parameter and add a
   thin wrapper (see `HandleImap` / `HandlePop3`).
3. `RegisterServerControls(loader, "<prefix>", <default_port>, g_ctrl, Handle)`
   registers `<prefix>_start/_stop/_status` with the `tls_cert`, `tls_key`,
   `implicit_tls` and `starttls` named parameters.

Implicit TLS is handled centrally in `ServerController::AcceptLoop`. STARTTLS is
done in the handler: `ctrl.StartTlsEnabled()` → `stream.StartTls(ctrl.TlsCtx(), err)`.
Two ports for one protocol = two controllers + two thin wrappers over one impl
(`smtp_out`, `pop3`).
