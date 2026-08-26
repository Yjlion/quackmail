# AGENTS.md

QuackMail (QuackCit) is a Citadel BBS/groupware server implemented as 12
loadable DuckDB extensions in C++17. The shared core compiles once into a
static library linked into every extension. See [CLAUDE.md](CLAUDE.md) for
full architecture, gotchas, and the reuse API catalog ([docs/contributing.md](docs/contributing.md)
is the same guidance written for a person — keep both in step).

## Build

Linux-only. Submodules must be checked out (`git submodule update --init --recursive`).

```bash
# First build: 20-40 min. Incremental is fast unless core/ changed.
GEN=ninja make release

# RAM-limited machine — cap parallelism:
GEN=ninja CMAKE_BUILD_PARALLEL_LEVEL=2 make release

# Artifacts land in build/release/extension/<name>/<name>.duckdb_extension
```

**Core changes are expensive.** Every `core/src/*.cpp` or shared header is
compiled into all 12 extensions. One core edit → 12 extension rebuilds.

## Test

```bash
make test                          # sqllogictest (21 files in test/sql/)

# Integration tests (28 files in test/integration/) — run individually:
~/venv/bin/python3 test/integration/test_citadel.py
# Need: python3 -m venv ~/venv && ~/venv/bin/pip install 'duckdb==1.5.4' jmapc
```

CI also runs `python3 tools/gen_assets.py --check` — if you edit
`http/assets/`, regenerate with `python3 tools/gen_assets.py`.

## Checklists (don't forget any step)

### Adding a module

1. `<mod>/CMakeLists.txt` — copy `pop3/CMakeLists.txt`, change the name
2. `extension_config.cmake` — add `duckdb_extension_load(...)` entry
3. `core/quackmail_core.cmake` — add any new `core/src/*.cpp` files
4. `.github/workflows/release.yml` — add to the hardcoded `for ext in ...` loop
5. `deploy/quackcit_common.sh` — add to `quackcit_services()` or `quackcit_workers()`
6. `test/sql/quackmail.test` — add load + start/stop/status cycle
7. `docs/protocols.md` — add port table row

Step 4 is the easiest to miss; it silently omits the extension from releases.

### Adding a listener

Declare a `ServerController`, write `Handle()`, call `RegisterServerControls()`.
Two ports for one protocol = two controllers + two thin wrappers over one
handler impl (see `smtp_out`, `pop3`). Implicit TLS is in `AcceptLoop`;
STARTTLS is in the handler via `ctrl.StartTlsEnabled()`.

### Adding a background worker

Use `PeriodicWorker` (`core/include/quackmail/worker.hpp`). **Always** add a
one-shot `_run` table function — that is what makes it testable without sleeping.

## Things that will bite you

- **No `clang-format` at the project root.** DuckDB's `.clang-format` lives
  inside `duckdb/`. Match existing code style by reading neighbors.
- **`EnsureSchema` runs from table function `init`, not `LOAD`.** After
  `LOAD quackmail` alone, most tables don't exist yet. Kick one with
  `SELECT count(*) FROM qm_status()` before running raw SQL offline.
- **Umbrella table functions open their own `Connection`.** Rows written by
  sqllogictest's harness are invisible to them. Assert configuration in
  `test/sql/`, but assert enforcement in `test/integration/`.
- **`GetOrAssignUserNum` returns 0 for unknown users** — it does not create
  them. Use it as an existence check.
- **Structs with default member initializers** are non-aggregates in this
  build. `Foo{a, b}` needs an explicit constructor.
- **DuckDB table function args must be constant-foldable.** No nested table
  functions. Composable forms must be scalar functions. Declare numerics as
  `BIGINT`, not `VARCHAR`.
- **Nothing in C++ may write to stderr.** The admin CLI reads errors from the
  server's stderr file via the control channel.
- **Deploy scripts are POSIX `sh`** — test with `dash -n <script>`. Watch for
  `die` inside `$(...)`: it only exits the subshell.
- **Version string lives in 3 places** that must move together:
  `quackmail/src/quackmail_extension.cpp`, `core/src/citadel_store.cpp`,
  `test/sql/quackmail.test`.
