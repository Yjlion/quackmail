---
name: add-worker
description: Add a new timer-driven background worker to QuackCit (as opposed to a network listener) — PeriodicWorker, tick registration, and the one-shot _run table function that makes it testable. Use when adding anything that runs on a schedule rather than accepting connections.
---

Not everything is a listener. For anything on a timer, declare a
`PeriodicWorker` (`core/include/quackmail/worker.hpp`), write a tick taking a
`Connection &`, and register `<prefix>_start/_stop/_status` — `spool/` has the
registration helper. Then add a row to `quackcit_workers` in
`deploy/quackcit_common.sh`, which is what actually starts it on a real install.

Always give a worker a one-shot `_run` table function beside its controls. That
is what makes it testable: an integration test calls the run function and
asserts, instead of sleeping past a poll interval.
