# scribe-tap

A Wayland-friendly keystroke mirror designed for interception-tools pipelines on Hyprland.

`scribe-tap` consumes `struct input_event` frames from `stdin`, forwards them unchanged to
`stdout`, and mirrors the reconstructed text to JSONL logs plus per-window snapshot files.
It tags every entry with the Hyprland active window, and optionally appends clipboard
contents when a paste shortcut is detected.

## Features

- Works inside an existing `udevmon` chain (`intercept | scribe-tap | … | uinput`).
- Tags each keystroke with the active Hyprland window (title, class, address) and can read the signature from the owning user.
- Appends to daily JSONL logs and maintains one snapshot file per window. Log mode `both` (default) keeps a concise key trail alongside snapshots.
- Flushes snapshot files after periods of idle typing so that the most recent buffer survives compositor or browser crashes.
- Detects clipboard pastes (Ctrl+V or Shift+Insert) via `wl-paste` or `xclip`.
- Learns the Hyprland instance signature automatically when running out of session, so `--hypr-user` is rarely required.
- Zero external dependencies at runtime beyond the compositor tooling you already have.

### NixOS module

The flake exports `nixosModules.default`, a high-level module that creates state directories, wires up command-line flags, and exposes the fully rendered invocation for downstream pipeline modules. After adding the flake as an input:

```nix
{
  imports = [ inputs.scribe-tap.nixosModules.default ];

  services.scribeTap = {
    enable = true;
    dataDir = "/var/lib/scribe-tap";
    logMode = "both";
    translateMode = "xkb";
    hyprUser = "sinity";
    xkbLayout = "pl";
    directoryUser = "sinity";
    directoryGroup = "users";
  };
}
```

The module publishes `services.scribeTap.command` (list form), `commandString` (shell form), and the resolved directories in `services.scribeTap.resolvedPaths`, keeping pipeline configuration declarative.

## Building

```sh
make
```

Run the basic integration test harness:

```sh
make check
```

Run quick throughput benchmarks (writes to a temporary directory):

```sh
make bench
```

### Test Harness Helpers

The integration tests spoof wall-clock time and Hyprland tooling via dedicated
environment hooks:

- `SCRIBE_TAP_TEST_TIME_FILE` – path to a file containing two lines, the first
  with `<real_sec> <real_nsec>` and the optional second with
  `<monotonic_sec> <monotonic_nsec>`. When set, the binary uses those values for
  `CLOCK_REALTIME`/`CLOCK_MONOTONIC`, enabling deterministic day transitions in
  `tests/test_basic.py`.
- `SCRIBE_TAP_TEST_HYPRCTL` – absolute path to a stub `hyprctl` binary used when
  resolving the compositor context during tests.

The `Makefile` honours `CC`, `CFLAGS`, and `prefix`. Install via:

```sh
make install prefix=$HOME/.local
```

## Runtime options

```
scribe-tap [--data-dir DIR] [--log-dir DIR] [--snapshot-dir DIR] [--snapshot-interval SEC]
           [--clipboard (auto|off)] [--context hyprland|none]
           [--log-mode events|snapshots|both] [--translate xkb|raw]
           [--xkb-layout LAYOUT] [--xkb-variant VARIANT]
           [--context-refresh SEC] [--hyprctl CMD]
           [--hypr-signature PATH] [--hypr-user USER]
```

- `--data-dir` – root directory for artefacts (defaults to `/realm/data/keylog`, creating `logs/` and `snapshots/` automatically).
- `--log-dir` – directory for JSONL log files (`$data_dir/logs` by default).
- `--snapshot-dir` – directory for live snapshots (`$data_dir/snapshots`).
- `--snapshot-interval` – write snapshot at most once per window per interval (seconds).
- `--clipboard` – control paste capture; `auto` invokes clipboard helpers, `off` disables.
- `--context` – `hyprland` (default) polls Hyprland for active window; `none` disables polling.
- `--log-mode` – choose whether to record `events`, `snapshots`, or `both` (default).
- `--context-refresh` – minimum seconds between Hyprland window polls.
- `--hyprctl` – override the hyprctl executable path.
- `--hypr-signature` – read the Hyprland instance signature from a given file (useful when running out of session scope).
- `--hypr-user` – look up the Hyprland signature for the named user (tries cache and runtime directories).
- `--translate` – `xkb` (default) uses libxkbcommon to emit UTF-8 text; `raw` falls back to direct keycode mapping.
- `--xkb-layout` / `--xkb-variant` – pass explicit XKB names when running outside the user session (e.g. in interception-tools).

Snapshots contain the current buffer for their window, making it easy to yank the most
recent draft if a browser tab eats it. JSON logs hold the full per-key history.

Use the included replay helper to inspect logs (`scribe-tap-replay` when installed via Nix). It can list snapshots, tail events, or run interactively. Filter output by window or session id and optionally surface clipboard payloads:

```sh
# latest snapshots and tail events
python3 tools/replay.py --log-dir /realm/data/keylog/logs --snapshot-dir /realm/data/keylog/snapshots --mode both --window messenger --events-tail 10 --show-clipboard

# interactive picker
python3 tools/replay.py --snapshot-dir /realm/data/keylog/snapshots --interactive --session 20251003T001711
```

## License

MIT.
