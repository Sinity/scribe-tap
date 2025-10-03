# scribe-tap

A Wayland-friendly keystroke mirror designed for interception-tools pipelines on Hyprland.

`scribe-tap` consumes `struct input_event` frames from `stdin`, forwards them unchanged to
`stdout`, and mirrors the reconstructed text to JSONL logs plus per-window snapshot files.
It tags every entry with the Hyprland active window, and optionally appends clipboard
contents when a paste shortcut is detected.

## Features

- Works inside an existing `udevmon` chain (`intercept | scribe-tap | … | uinput`).
- Tags each keystroke with the active Hyprland window (title, class, address).
- Appends to daily JSONL logs and maintains one snapshot file per window.
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
           [--context-refresh SEC] [--hyprctl CMD]
```

- `--log-dir` – directory for JSONL log files (`/realm/data/keylog/logs` by default).
- `--snapshot-dir` – directory for live snapshots (`/realm/data/keylog/snapshots`).
- `--snapshot-interval` – write snapshot at most once per window per interval (seconds).
- `--clipboard` – control paste capture; `auto` invokes clipboard helpers, `off` disables.
- `--context` – `hyprland` (default) polls Hyprland for active window; `none` disables polling.
- `--context-refresh` – minimum seconds between Hyprland window polls.
- `--hyprctl` – override the hyprctl executable path.

Snapshots contain the current buffer for their window, making it easy to yank the most
recent draft if a browser tab eats it. JSON logs hold the full per-key history.

Use the included replay helper to inspect logs:

```sh
python3 tools/replay.py --log-dir /realm/data/keylog/logs --window messenger
```

## License

MIT.
