#!/usr/bin/env python3
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

KEY_A = 30
KEY_ENTER = 28
EV_KEY = 0x01
EV_SYN = 0x00


def pack_event(sec: int, usec: int, ev_type: int, code: int, value: int) -> bytes:
    return struct.pack("llHHI", sec, usec, ev_type, code, value & 0xFFFFFFFF)


def send_key(stream, code: int, value: int) -> None:
    stream.write(pack_event(0, 0, EV_KEY, code, value))
    stream.write(pack_event(0, 0, EV_SYN, 0, 0))


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]
    binary = repo_root / "scribe-tap"
    if not binary.exists():
        print("scribe-tap binary not built", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        log_dir = Path(tmp) / "logs"
        snap_dir = Path(tmp) / "snapshots"
        log_dir.mkdir()
        snap_dir.mkdir()

        proc = subprocess.Popen(
            [
                str(binary),
                "--log-dir",
                str(log_dir),
                "--snapshot-dir",
                str(snap_dir),
                "--context",
                "none",
                "--clipboard",
                "off",
                "--snapshot-interval",
                "0",
                "--log-mode",
                "both",
                "--translate",
                "raw",
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        assert proc.stdin is not None

        send_key(proc.stdin, KEY_A, 1)
        send_key(proc.stdin, KEY_A, 0)
        send_key(proc.stdin, KEY_ENTER, 1)
        send_key(proc.stdin, KEY_ENTER, 0)
        proc.stdin.close()
        proc.wait(timeout=5)

        assert proc.returncode == 0, proc.stderr.read().decode()

        files = list(log_dir.glob("*.jsonl"))
        assert files, "no log files created"
        events = [json.loads(line) for line in files[0].read_text().splitlines()]
        press = [e for e in events if e["event"] == "press"]
        assert any(e.get("keycode") == "KEY_A" for e in press)
        assert any(e.get("keycode") == "KEY_ENTER" for e in press)
        assert all("buffer" not in e for e in press), "press events should omit buffer payload"
        snapshots = [e for e in events if e.get("event") == "snapshot" and e.get("buffer")]
        assert snapshots and snapshots[-1]["buffer"].startswith("a"), "snapshot should capture buffer"

        snapshot_files = list(snap_dir.glob("*.txt"))
        assert snapshot_files, "no snapshot files created"
        content = snapshot_files[0].read_text()
        assert "a" in content
        assert content.endswith("\n")

    return 0


if __name__ == "__main__":
    sys.exit(main())
