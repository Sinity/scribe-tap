# Rust Rewrite Blueprint

This document captures the conclusions from the 2025-10-17 design discussion
about porting `scribe-tap` from C to Rust. It is intended to give future
implementers a head start on both the technical plan and the surrounding
operational work.

## Why Consider Rust?

- **Shared infrastructure with intercept-bounce**  
  The interception pipeline already ships a Rust binary (`intercept-bounce`).
  Extracting the common building blocks (EVDEV frame parsing, queueing, idle
  timers, timestamp maths) into a shared Rust crate would reduce duplication and
  make it easier to evolve the pipeline as a cohesive whole.

- **Memory and resource safety**  
  Rust's ownership model and `Result` propagation eliminate the manual lifetime
  management that currently relies on `exit(1)` fail-fast paths. This directly
  targets the crash-only failure handling we saw while stabilising snapshot
  persistence.

- **Structured concurrency**  
  Channels and scoped threads (or a lightweight async runtime) simplify the
  current `pthread` queue, offer stronger back-pressure semantics, and make it
  easier to cancel or drain the worker on shutdown.

- **Extensibility**  
  Features on the horizon--filtered capture, richer telemetry, selective
  replay--are easier to slot into a modular Rust code base than into the single
  translation unit we maintain today.

## Guiding Principles

1. **Parity first.**  
   CLI flags, environment overrides, logging formats, snapshot layout, and exit
   codes must remain identical until the new binary has proven parity in the
   production pipeline.

2. **Incremental adoption.**  
   The rewrite should deliver components that can be exercised in isolation
   (crate-level unit tests, Python integration harness, interception pipeline
   rehearsal) before we attempt a cut-over.

3. **Keep the data plane local.**  
   JSONL logs and snapshot text files stay the canonical artefacts. Tracing and
   metrics may sit alongside them, but the rewrite is *not* a licence to reroute
   keystroke content through generic logging backends.

4. **Avoid Friday-night rewrites.**  
   Plan for a multi-week effort that touches build tooling, packaging, testing,
   and deployment. The C implementation remains the safety net until the
   Rust-based flow has been proven end to end.

## Target Architecture

```
crates/
├── scribe-core         # shared low-level primitives (input parsing, timestamps)
├── scribe-pipeline     # queueing, clipboard helpers, idle flush logic
├── scribe-cli          # CLI surface + binary entry point
└── intercept-bounce    # existing crate optionally reusing scribe-core
```

- `scribe-core` exposes safe wrappers for EVDEV events, libxkbcommon keymap
  translation, and monotonic/real time conversion. It should also host the time
  override hooks currently injected via `SCRIBE_TAP_TEST_*`.
- `scribe-pipeline` packages the runtime components--fan-out queue, window
  context fetch, clipboard integration, snapshot writer--behind traits so the CLI
  can be configured and tested with fakes.
- `scribe-cli` holds argument parsing (e.g. `clap`), configuration validation,
  and the main loop that wires the pieces together.
- `intercept-bounce` consumes the shared crate(s) opportunistically once they
  stabilise; this happens after `scribe-tap` has shipped in Rust.

## Implementation Stages

### Stage 0 - Preparation

- Introduce Cargo metadata (`Cargo.toml`, workspace layout) without altering the
  current C build. Update the flake, Makefile, and CI scripts to build both
  toolchains in parallel.
- Capture golden fixtures for the Python integration harness (`tests/test_basic.py`)
  that we can replay against the Rust binary later.

### Stage 1 - Core crates

- Implement EVDEV frame parsing, UTF-8 translation, idle timers, and timestamp
  utilities in `scribe-core`.
- Provide FFI shims for `libxkbcommon`, Hyprland's `hyprctl` signatures, and
  clipboard helpers. Where Rust crates exist (`xkbcommon`, `zbus`/`wayland`),
  evaluate whether adopting them is simpler than hand-written bindings.
- Port the deterministic testing hooks (`SCRIBE_TAP_TEST_TIME_FILE`,
  `SCRIBE_TAP_TEST_HYPRCTL`) into injectable traits/functions so the Python
  suite and future Rust unit tests remain deterministic.

### Stage 2 - Runtime pipeline

- Recreate the producer/consumer queue with channels (`std::sync::mpsc` or
  `crossbeam_channel`) and guard it with back-pressure to avoid unbounded RAM
  growth.
- Port snapshot writers and JSONL loggers, preserving the exact payload schema.
- Replicate idle flush logic, clipboard detection, and Hyprland context refresh
  timers. Add instrumentation hooks (e.g. `tracing::info!`) but keep them behind
  compile-time features so production defaults match current behaviour.

### Stage 3 - CLI and parity testing

- Implement the CLI using `clap` or `argh`; emit identical help text and support
  the same environment overrides as today.
- Add integration tests in Rust mirroring the Python harness, then run the
  Python suite against the Rust binary using `cargo test -- --ignored` or an
  explicit `make check-rust` target.
- Verify the binary inside a live interception pipeline (`intercept |
  scribe-tap-rs | ... | uinput`) and capture CPU usage, throughput, and latency
  metrics for regression comparison.

### Stage 4 - Packaging and deployment

- Update the Nix flake to package the Rust binary (`cargo build --release`),
  install the man page/README, and expose a `services.scribeTap.command` that
  points at the new executable.
- Keep both the C and Rust binaries available behind feature flags until the
  Rust version proves stable in the real pipeline. Document the feature switch
  so operators can fall back quickly.

## Tooling & Build Changes

- Add `cargo fmt`, `cargo clippy`, and `cargo test` targets to the CI pipeline.
- Update the dev shell (`nix develop`) to ship Rust tooling (rustc, cargo,
  clippy, rustfmt) alongside the existing C toolchain.
- Ensure `make check` orchestrates both Python and Rust tests, or add a dedicated
  `make check-rust` so contributors do not forget the new suite.
- Preserve existing installers (`make install`, Nix derivation) by wiring them to
  install the Rust binary under the same path (`bin/scribe-tap`), at least while
  parity mode is active.

## Risks and Unknowns

- **FFI sharp edges.**  
  We still require `libxkbcommon`, `wl-paste`/`xclip`, and Hyprland metadata.
  Binding errors or ABI mismatches can introduce subtle bugs; automated tests
  must cover locale variants and multi-layout systems.

- **Performance regressions.**  
  Rust's standard library introduces different buffering and locale behaviour.
  Confirm that startup time, steady-state CPU use, and I/O throughput stay within
  current tolerances.

- **Operational churn.**  
  Switching binaries changes what lands in `/nix/store`, service activation
  scripts, and upgrade paths. Coordinate with the Sinnix flake before flipping
  defaults.

- **Dual maintenance window.**  
  Expect at least one release cycle where both C and Rust implementations live
  in the tree. Establish a freeze window for feature work so parity testing is
  not a moving target.

## Milestones & Exit Criteria

1. **Compiled Rust binary passes local tests** (Rust unit tests + Python harness).
2. **Nix build succeeds** and packages both binaries, with a feature flag to
   select the Rust one.
3. **Live interception rehearsal** demonstrates parity in logs, snapshots,
   clipboard capture, and window tagging.
4. **Operational sign-off** from the Sinnix pipeline maintainers.
5. **Retire C binary** only after at least one week of successful production use
   or comparable soak testing.

## Open Questions

- Should we target `tokio` or stay on the standard library for concurrency? The
  current workload is light enough that `std::thread` is likely sufficient.
- Do we want to extract a more generic "scribe toolkit" crate that intercept
  tooling can depend on, or keep the shared crate private to this repository
  until the API stabilises?
- Is now the time to revise the JSONL schema (e.g. add structured fields for
  modifiers), or should format changes wait until after the Rust port ships?

Document updates belong in this file as the migration takes shape. Once we start
implementing the rewrite, record design deviations, benchmarks, and operational
findings here so the context stays preserved.
