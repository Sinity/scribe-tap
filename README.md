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
- Detects clipboard pastes (Ctrl+V or Shift+Insert) via `wl-paste` or `xclip`.
- Zero external dependencies at runtime beyond the compositor tooling you already have.

## Building

```sh
make
```

Run the basic integration test harness:

```sh
make check
```

The `Makefile` honours `CC`, `CFLAGS`, and `prefix`. Install via:

```sh
make install prefix=$HOME/.local
```

## Runtime options

```
scribe-tap [--log-dir DIR] [--snapshot-dir DIR] [--snapshot-interval SEC]
           [--clipboard (auto|off)] [--context hyprland|none]
           [--log-mode events|snapshots|both] [--translate xkb|raw]
           [--xkb-layout LAYOUT] [--xkb-variant VARIANT]
           [--context-refresh SEC] [--hyprctl CMD]
           [--hypr-signature PATH] [--hypr-user USER]
```

- `--log-dir` – directory for JSONL log files (`/realm/data/keylog/logs` by default).
- `--snapshot-dir` – directory for live snapshots (`/realm/data/keylog/snapshots`).
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

Use the included replay helper to inspect logs (`scribe-tap-replay` when installed via Nix). It can list snapshots, tail events, or run interactively:

```sh
# latest snapshots and tail events
python3 tools/replay.py --log-dir /realm/data/keylog/logs --snapshot-dir /realm/data/keylog/snapshots --mode both --window messenger --events-tail 10

# interactive picker
python3 tools/replay.py --snapshot-dir /realm/data/keylog/snapshots --interactive
```

## License

MIT.
