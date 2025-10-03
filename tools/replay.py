#!/usr/bin/env python3
"""Replay captured scribe-tap logs for quick inspection."""

import argparse
import datetime as dt
import json
from pathlib import Path
from typing import Iterable


def load_events(log_path: Path) -> Iterable[dict]:
    if not log_path.exists():
        raise SystemExit(f"Log file not found: {log_path}")
    with log_path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                yield json.loads(line)
            except json.JSONDecodeError:
                continue


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log-dir", type=Path, default=Path("/realm/data/keylog/logs"))
    parser.add_argument("--date", type=str, help="Date (YYYY-MM-DD)",
                        default=dt.datetime.utcnow().date().isoformat())
    parser.add_argument("--window", type=str, help="Filter window substring")
    parser.add_argument("--show-keys", action="store_true", help="Print key trail")
    parser.add_argument("--tail", type=int, default=20, help="Tail length for key trail")
    args = parser.parse_args()

    log_path = args.log_dir / f"{args.date}.jsonl"
    events = list(load_events(log_path))

    if args.window:
        windows = {
            ev.get("window"): ev.get("buffer")
            for ev in events
            if ev.get("buffer") is not None and args.window.lower() in (ev.get("window") or "").lower()
        }
    else:
        windows = {
            ev.get("window"): ev.get("buffer")
            for ev in events
            if ev.get("buffer") is not None
        }

    if not windows:
        print("No buffers match the requested criteria.")
    for window, buffer in sorted(windows.items()):
        print(f"Window: {window}")
        print(buffer.rstrip("\n") or "<empty>")
        print("---")

    if args.show-keys:
        trail = [
            (ev.get("ts"), ev.get("window"), ev.get("keycode"))
            for ev in events
            if ev.get("event") == "press"
        ][-args.tail:]
        if trail:
            print("Key events (newest last):")
            for ts, window, key in trail:
                print(f"[{ts}] {window}: {key}")


if __name__ == "__main__":
    main()
