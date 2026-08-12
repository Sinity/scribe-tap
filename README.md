# scribe-tap

A Wayland-friendly keystroke and pointer mirror designed for interception-tools pipelines
on Hyprland.

`scribe-tap` consumes `struct input_event` frames from `stdin`, forwards them unchanged to
`stdout`, and mirrors keyboard content and raw mouse/pointer activity to JSONL logs (plus a
per-session snapshot file for keystroke content). It does not track the compositor's active
window itself — window/app attribution, if you want it, is a downstream timestamp-join
against ActivityWatch's focus events; scribe-tap used to poll `hyprctl` for this and it never
worked correctly on this host's actual Hyprland runtime layout, so the feature was removed
rather than patched.

## Features

- Works inside an existing `udevmon` chain (`intercept | scribe-tap | … | uinput`).
- Captures keyboard content: appends to daily JSONL logs and maintains a session snapshot
  file. Log mode `both` (default) keeps a concise key trail alongside snapshots.
- Captures raw mouse/pointer activity: button press/release (`BTN_LEFT`..`BTN_TASK`),
  relative motion and scroll (`EV_REL`, any axis), and absolute-position axes (`EV_ABS`,
  touchpads/tablets/touchscreens riding the same pipeline). Every event is logged; nothing
  is silently dropped or downsampled.
- Flushes snapshot files after periods of idle typing so that the most recent buffer survives
  compositor or browser crashes.
- Detects clipboard pastes (Ctrl+V or Shift+Insert) via `wl-paste` or `xclip`.
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

The `Makefile` honours `CC`, `CFLAGS`, and `prefix`. Install via:

```sh
make install prefix=$HOME/.local
```

## Runtime options

```
scribe-tap [--data-dir DIR] [--log-dir DIR] [--snapshot-dir DIR] [--snapshot-interval SEC]
           [--clipboard (auto|off)] [--log-mode events|snapshots|both]
           [--translate xkb|raw] [--xkb-layout LAYOUT] [--xkb-variant VARIANT]
```

- `--data-dir` – root directory for artefacts (defaults to `/realm/data/captures/keylog`, creating `logs/` and `snapshots/` automatically).
- `--log-dir` – directory for JSONL log files (`$data_dir/logs` by default).
- `--snapshot-dir` – directory for live snapshots (`$data_dir/snapshots`).
- `--snapshot-interval` – write the session snapshot at most once per interval (seconds).
- `--clipboard` – control paste capture; `auto` invokes clipboard helpers, `off` disables.
- `--log-mode` – choose whether to record `events`, `snapshots`, or `both` (default).
- `--translate` – `xkb` (default) uses libxkbcommon to emit UTF-8 text; `raw` falls back to direct keycode mapping.
- `--xkb-layout` / `--xkb-variant` – pass explicit XKB names when running outside the user session (e.g. in interception-tools).

Snapshots contain the current keystroke buffer for the session, making it easy to yank the
most recent draft if a browser tab eats it. JSON logs hold the full per-key and per-pointer-
event history: keystroke events use `event`/`keycode`/`changed`/optional `clipboard`; pointer
events use `event` (`pointer_button_press`/`_release`, `pointer_rel`, `pointer_abs`) plus
`code` (the button/axis name) and `value`.

Use the included replay helper to inspect logs (`scribe-tap-replay` when installed via Nix). It can list snapshots, tail events, or run interactively. Filter output by session id and optionally surface clipboard payloads:

```sh
# latest snapshots and tail events
python3 tools/replay.py --log-dir /realm/data/captures/keylog/logs --snapshot-dir /realm/data/captures/keylog/snapshots --mode both --events-tail 10 --show-clipboard

# interactive picker
python3 tools/replay.py --snapshot-dir /realm/data/captures/keylog/snapshots --interactive --session 20251003T001711
```

## License

MIT.
