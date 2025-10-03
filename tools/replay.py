#!/usr/bin/env python3
"""Replay captured scribe-tap logs for quick inspection."""

import argparse
import datetime as dt
import json
import sys
from pathlib import Path
from typing import Iterable, List, Tuple


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
    parser.add_argument("--snapshot-dir", type=Path, default=Path("/realm/data/keylog/snapshots"))
    parser.add_argument(
        "--date",
        type=str,
        help="Date (YYYY-MM-DD)",
        default=dt.datetime.now(dt.timezone.utc).date().isoformat(),
    )
    parser.add_argument("--window", type=str, help="Filter window substring")
    parser.add_argument("--mode", choices=["snapshots", "events", "both"], default="snapshots")
    parser.add_argument("--interactive", action="store_true", help="Interactive selection")
    parser.add_argument("--events-tail", type=int, default=20, help="Number of key events to show")
    args = parser.parse_args()

    log_path = args.log_dir / f"{args.date}.jsonl"
    try:
        events = list(load_events(log_path))
    except SystemExit as exc:
        if args.mode in {"snapshots", "both"}:
            events = []
        else:
            raise

    def slugify(text: str) -> str:
        cleaned: List[str] = []
        prev_underscore = False
        for ch in text:
            if ch.isalnum():
                cleaned.append(ch.lower())
                prev_underscore = False
            else:
                if not prev_underscore:
                    cleaned.append("_")
                    prev_underscore = True
        slug = "".join(cleaned).strip("_")
        return slug or "window"

    snapshot_events = {}
    for ev in events:
        if ev.get("event") == "snapshot":
            window = ev.get("window") or "unknown"
            slug = slugify(window)
            snapshot_events[slug] = (window, ev.get("buffer") or "")

    snapshots: List[Tuple[str, str, Path]] = []
    if args.mode in {"snapshots", "both"}:
        if args.snapshot_dir.exists():
            for path in sorted(args.snapshot_dir.glob("*.txt")):
                slug = path.stem
                window, _ = snapshot_events.get(slug, (slug, ""))
                content = path.read_text(encoding="utf-8", errors="replace")
                snapshots.append((window, content, path))
        if not snapshots:
            for slug, (window, buffer) in snapshot_events.items():
                snapshots.append((window, buffer, Path()))

    def filter_window(name: str) -> bool:
        if not args.window:
            return True
        target = args.window.lower()
        return target in (name or "").lower()

    if args.interactive:
        if args.mode in {"snapshots", "both"} and snapshots:
            filtered = [(idx + 1, win, buf) for idx, (win, buf, _) in enumerate(snapshots) if filter_window(win)]
            if not filtered:
                print("No snapshots match the requested criteria.")
            else:
                print("Snapshots:")
                for idx, win, buf in filtered:
                    preview = (buf.strip().splitlines() or ["<empty>"])[0]
                    print(f"  [{idx}] {win}: {preview[:60]}")
                choice = input("Select snapshot (q to quit): ").strip().lower()
                if choice not in {"q", "quit", "exit", ""}:
                    try:
                        sel = int(choice)
                    except ValueError:
                        print("Invalid selection")
                        sys.exit(1)
                    match = next(((win, buf) for i, win, buf in filtered if i == sel), None)
                    if not match:
                        print("Selection out of range")
                        sys.exit(1)
                    window, buffer = match
                    print(f"Window: {window}")
                    print(buffer.rstrip("\n") or "<empty>")
                    print("---")
                    if args.mode in {"events", "both"}:
                        tail = [
                            (ev.get("ts"), ev.get("window"), ev.get("keycode"))
                            for ev in events
                            if ev.get("event") == "press" and filter_window(ev.get("window") or "")
                        ][-args.events_tail:]
                        if tail:
                            print("Key events (newest last):")
                            for ts, window_name, key in tail:
                                print(f"[{ts}] {window_name}: {key}")
                    sys.exit(0)
                sys.exit(0)
        else:
            print("Interactive mode requires snapshots to be available.")
            sys.exit(0)

    if args.mode in {"snapshots", "both"}:
        matched = False
        for window, buffer, _ in snapshots:
            if not filter_window(window):
                continue
            matched = True
            print(f"Window: {window}")
            print(buffer.rstrip("\n") or "<empty>")
            print("---")
        if not matched:
            print("No snapshots match the requested criteria.")

    if args.mode in {"events", "both"}:
        trail = [
            (ev.get("ts"), ev.get("window"), ev.get("keycode"))
            for ev in events
            if ev.get("event") == "press" and filter_window(ev.get("window") or "")
        ][-args.events_tail:]
        if trail:
            print("Key events (newest last):")
            for ts, window, key in trail:
                print(f"[{ts}] {window}: {key}")


if __name__ == "__main__":
    main()
