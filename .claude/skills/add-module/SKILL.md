---
name: add-module
description: Add a new loadable DuckDB extension module to QuackCit — the four files besides <mod>/src/* that must be touched, including the one that's easy to miss (release.yml packaging). Use when creating a brand-new extension directory.
---

Besides `<mod>/src/*` you must touch **four** files — the last one is easy to miss:

1. `<mod>/CMakeLists.txt` — copy `pop3/CMakeLists.txt` verbatim, change the name.
2. `extension_config.cmake` — add a `duckdb_extension_load(...)` entry.
3. `core/quackmail_core.cmake` — add any new `core/src/*.cpp` to the single
   compile list (core is compiled exactly once to avoid duplicate symbols).
4. `.github/workflows/release.yml` — add the extension to the hardcoded
   `for ext in ...` packaging loop, or it won't ship in releases.

Then add it to the `quackcit_services` table in `deploy/quackcit_common.sh`,
`test/sql/quackmail.test`, and the table in `docs/protocols.md`.
