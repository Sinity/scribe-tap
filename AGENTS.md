# Repository Guidelines

## Project Structure & Module Organization
Core logic sits in `src/main.c`, which compiles into the `scribe-tap` binary. Place shared declarations or future split modules in `include/` so the translation unit stays lean. End-to-end tests live in `tests/` and drive the binary via recorded `struct input_event` frames. Support scripts (benchmarks, log replay) belong in `tools/`. Build outputs are rooted in the repo top-level; wipe them with `make clean` before committing.

## Build, Test, and Development Commands
- `make` – compile with pkg-config lookups for libxkbcommon; respects `CFLAGS` and `PKG_CONFIG_PATH`.
- `make check` / `python3 tests/test_basic.py` – run the integration harness against temporary log directories.
- `make bench` – execute the throughput probe (writes under `/tmp`).
- `nix develop` – enter the preconfigured shell with gcc, libxkbcommon, wl-clipboard, xclip, and pkg-config.
- `nix build` – produce the packaged derivation, installing replay and bench helpers alongside the binary.

## Coding Style & Naming Conventions
Target `-std=c11` with `-Wall -Wextra`; new warnings should fail the build. Keep four-space indentation, braces on the control line, and prefer `static` helpers for internal linkage. Use lower_snake_case for functions, locals, and file-scope statics, while structs stay UpperCamelCase (`struct Buffer`). Follow the existing CLI conventions (`--long-flag`) and JSON payload names when extending the protocol. Clean up heap allocations before returning.

## Testing Guidelines
Extend `tests/test_basic.py` when adding behaviours; each scenario should isolate itself with `tempfile.TemporaryDirectory()`. Name new assertions inside the `main()` workflow or helper functions prefixed with `test_` to preserve readability. Cover both event logging and snapshot persistence whenever you change buffer handling or clipboard logic. Run `make check`; add `make bench` when touching hot paths or I/O loops.

## Commit & Pull Request Guidelines
Recent history mixes plain imperative subjects (`Add data-dir flag`) with optional Conventional Commit prefixes (`feat:`). Whichever you choose, keep the subject under 72 characters and describe the user-visible impact in the body when needed. Pull requests should link issues, outline configuration changes, and include `make`/`make check` results (terminal snippets beat screenshots). Mention any flags or environment requirements reviewers must set.

## Environment & Tooling Notes
Hyprland context detection depends on libxkbcommon plus clipboard helpers; verify they are on PATH or rely on `nix develop`. Default log destinations point at `/realm/data/keylog`, so override with `--data-dir` or `--log-dir` during local testing. When embedding inside interception pipelines, keep stdin/stdout unbuffered and use `--context none` for headless runs to avoid compositor calls.
