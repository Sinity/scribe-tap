#!/usr/bin/env python3
import datetime
import json
import os
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

KEY_A = 30
KEY_B = 48
KEY_V = 47
KEY_ENTER = 28
KEY_LEFTCTRL = 29
KEY_LEFTSHIFT = 42
KEY_INSERT = 110
KEY_CAPSLOCK = 58
EV_KEY = 0x01
EV_SYN = 0x00


def pack_event(sec: int, usec: int, ev_type: int, code: int, value: int) -> bytes:
    return struct.pack("llHHI", sec, usec, ev_type, code, value & 0xFFFFFFFF)


def send_key(stream, code: int, value: int) -> None:
    stream.write(pack_event(0, 0, EV_KEY, code, value))
    stream.write(pack_event(0, 0, EV_SYN, 0, 0))


def write_fake_time(path: Path, dt: datetime.datetime, monotonic: float = None) -> None:
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=datetime.timezone.utc)
    dt = dt.astimezone(datetime.timezone.utc)
    real_sec = int(dt.timestamp())
    real_nsec = int(round((dt.timestamp() - real_sec) * 1_000_000_000))
    if real_nsec >= 1_000_000_000:
        real_sec += 1
        real_nsec -= 1_000_000_000
    if monotonic is None:
        mono_sec = real_sec
        mono_nsec = real_nsec
    else:
        mono_sec = int(monotonic)
        mono_nsec = int(round((monotonic - mono_sec) * 1_000_000_000))
        if mono_nsec >= 1_000_000_000:
            mono_sec += 1
            mono_nsec -= 1_000_000_000
    with path.open("w", encoding="utf-8") as handle:
        handle.write(f"{real_sec} {real_nsec}\n{mono_sec} {mono_nsec}\n")


def wait_for(predicate, timeout: float = 1.0) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError("timed out waiting for condition")


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
                "10",
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
        send_key(proc.stdin, KEY_B, 1)
        send_key(proc.stdin, KEY_B, 0)
        proc.stdin.close()
        proc.wait(timeout=5)

        assert proc.returncode == 0, proc.stderr.read().decode()

        snapshot_files = list(snap_dir.glob("*.txt"))
        assert snapshot_files, "no snapshot files created on idle flush"
        content = snapshot_files[0].read_text()
        assert content == "ab", f"expected idle flush to persist full buffer, got {content!r}"

    with tempfile.TemporaryDirectory() as tmp:
        log_dir = Path(tmp) / "logs"
        snap_dir = Path(tmp) / "snapshots"
        log_dir.mkdir()
        snap_dir.mkdir()

        proc = subprocess.Popen(
            [
                str(binary),
                "--data-dir",
                str(Path(tmp) / "mirror"),
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
                "xkb",
                "--xkb-layout",
                "us",
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
        assert files, "no log files for xkb run"
        events = [json.loads(line) for line in files[0].read_text().splitlines()]
        press = [e for e in events if e["event"] == "press"]
        assert any(e.get("keycode") == "KEY_A" for e in press), "xkb press missing KEY_A"
        assert any(e.get("keycode") == "KEY_ENTER" for e in press), "xkb press missing KEY_ENTER"
        snapshots = [e for e in events if e.get("event") == "snapshot"]
        assert snapshots and snapshots[-1]["buffer"].endswith("\n"), "xkb snapshot should capture newline"

    with tempfile.TemporaryDirectory() as tmp:
        log_dir = Path(tmp) / "logs"
        snap_dir = Path(tmp) / "snapshots"
        stub_bin = Path(tmp) / "bin"
        log_dir.mkdir()
        snap_dir.mkdir()
        stub_bin.mkdir()

        payload = "paste-payload"
        state_file = stub_bin / "clip_state"
        stub_script = f"""#!/bin/sh
state_file='{state_file}'
if [ -f "$state_file" ]; then
  count=$(cat "$state_file")
else
  count=0
fi
count=$((count + 1))
printf "%s" "$count" > "$state_file"
if [ "$count" -eq 1 ]; then
  printf '{payload}'
else
  printf ''
fi
"""
        for name in ("wl-paste", "xclip"):
            script_path = stub_bin / name
            script_path.write_text(stub_script, encoding="utf-8")
            script_path.chmod(0o755)

        env = os.environ.copy()
        env["PATH"] = f"{stub_bin}:{env.get('PATH', '')}"

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
                "auto",
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
            env=env,
        )
        assert proc.stdin is not None

        # Ctrl+V paste
        send_key(proc.stdin, KEY_LEFTCTRL, 1)
        send_key(proc.stdin, KEY_V, 1)
        send_key(proc.stdin, KEY_V, 0)
        send_key(proc.stdin, KEY_LEFTCTRL, 0)

        # Shift+Insert paste
        send_key(proc.stdin, KEY_LEFTSHIFT, 1)
        send_key(proc.stdin, KEY_INSERT, 1)
        send_key(proc.stdin, KEY_INSERT, 0)
        send_key(proc.stdin, KEY_LEFTSHIFT, 0)

        # Ctrl+Insert should not trigger clipboard append
        send_key(proc.stdin, KEY_LEFTCTRL, 1)
        send_key(proc.stdin, KEY_INSERT, 1)
        send_key(proc.stdin, KEY_INSERT, 0)
        send_key(proc.stdin, KEY_LEFTCTRL, 0)

        proc.stdin.close()
        proc.wait(timeout=5)

        assert proc.returncode == 0, proc.stderr.read().decode()

        files = list(log_dir.glob("*.jsonl"))
        assert files, "no log files for clipboard capture"
        events = [json.loads(line) for line in files[0].read_text().splitlines()]
        press = [e for e in events if e["event"] == "press"]

        clipboard_events = [e for e in press if "clipboard" in e]
        assert len(clipboard_events) == 2, clipboard_events
        clipboard_values = [e["clipboard"] for e in clipboard_events]
        assert clipboard_values[0] == payload
        assert clipboard_values[1] == ""

        insert_events = [e for e in press if e.get("keycode") == "KEY_INSERT"]
        assert any("clipboard" in e for e in insert_events), "missing Shift+Insert clipboard event"
        assert any("clipboard" not in e for e in insert_events), "Ctrl+Insert incorrectly captured clipboard"

        snapshot_files = list(snap_dir.glob("*.txt"))
        assert snapshot_files, "no snapshot files for clipboard capture"
        content = snapshot_files[0].read_text()
        assert content == payload, f"unexpected clipboard buffer: {content!r}"

    with tempfile.TemporaryDirectory() as tmp:
        log_dir = Path(tmp) / "logs"
        snap_dir = Path(tmp) / "snapshots"
        sig_file = Path(tmp) / "sig"
        stub_dir = Path(tmp) / "very" / "long" / "nested" / "path"
        stub_dir.mkdir(parents=True)
        log_dir.mkdir()
        snap_dir.mkdir()
        sig_file.write_text("signature", encoding="utf-8")

        script_path = stub_dir / ("hyprctl_" + "x" * 80)
        script_path.write_text(
            """#!/bin/sh
if [ "$1" = "--instance" ]; then
  shift 2
fi
if [ "$1" = "activewindow" ] && [ "$2" = "-j" ]; then
  printf '{"title":"Doc","class":"Editor","address":"0xabc"}'
  exit 0
fi
exit 1
""",
            encoding="utf-8",
        )
        script_path.chmod(0o755)

        proc = subprocess.Popen(
            [
                str(binary),
                "--log-dir",
                str(log_dir),
                "--snapshot-dir",
                str(snap_dir),
                "--hyprctl",
                str(script_path),
                "--hypr-signature",
                str(sig_file),
                "--snapshot-interval",
                "0",
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        assert proc.stdin is not None

        send_key(proc.stdin, KEY_A, 1)
        send_key(proc.stdin, KEY_A, 0)
        proc.stdin.close()
        proc.wait(timeout=5)

        assert proc.returncode == 0, proc.stderr.read().decode()

        files = list(log_dir.glob("*.jsonl"))
        assert files, "no hyprctl log generated"
        events = [json.loads(line) for line in files[0].read_text().splitlines()]
        focus_events = [e for e in events if e.get("event") == "focus"]
        assert focus_events, "missing focus event for custom hyprctl"
        assert "Doc" in focus_events[-1].get("window", "")

    with tempfile.TemporaryDirectory() as tmp:
        log_dir = Path(tmp) / "logs"
        snap_dir = Path(tmp) / "snapshots"
        time_file = Path(tmp) / "time.txt"
        log_dir.mkdir()
        snap_dir.mkdir()

        day_one = datetime.datetime(2021, 1, 1, 23, 59, 50, tzinfo=datetime.timezone.utc)
        day_two = day_one + datetime.timedelta(minutes=2)
        write_fake_time(time_file, day_one, monotonic=1000.0)

        env = os.environ.copy()
        env["SCRIBE_TAP_TEST_TIME_FILE"] = str(time_file)

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
            env=env,
        )
        assert proc.stdin is not None

        wait_for(lambda: (log_dir / "2021-01-01.jsonl").exists())

        send_key(proc.stdin, KEY_A, 1)
        send_key(proc.stdin, KEY_A, 0)
        proc.stdin.flush()

        write_fake_time(time_file, day_two, monotonic=2000.0)

        send_key(proc.stdin, KEY_B, 1)
        send_key(proc.stdin, KEY_B, 0)
        proc.stdin.flush()

        proc.stdin.close()
        proc.wait(timeout=5)

        assert proc.returncode == 0, proc.stderr.read().decode()

        wait_for(lambda: (log_dir / "2021-01-02.jsonl").exists())
        files = sorted(f.name for f in log_dir.glob("*.jsonl"))
        assert "2021-01-02.jsonl" in files, files

        day_one_events = [
            json.loads(line)
            for line in (log_dir / "2021-01-01.jsonl").read_text().splitlines()
        ]
        day_two_events = [
            json.loads(line)
            for line in (log_dir / "2021-01-02.jsonl").read_text().splitlines()
        ]
        assert any(e.get("event") == "start" for e in day_one_events), day_one_events
        assert any(e.get("event") == "stop" for e in day_two_events), day_two_events

    with tempfile.TemporaryDirectory() as tmp:
        log_dir = Path(tmp) / "logs"
        snap_dir = Path(tmp) / "snapshots"
        sig_file = Path(tmp) / "sig"
        time_file = Path(tmp) / "time.txt"
        log_dir.mkdir()
        snap_dir.mkdir()
        sig_file.write_text("signature", encoding="utf-8")

        base_dt = datetime.datetime(2021, 1, 3, 12, 0, tzinfo=datetime.timezone.utc)
        write_fake_time(time_file, base_dt, monotonic=3000.0)

        hyprctl_path = Path(tmp) / "hyprctl"
        hyprctl_path.write_text(
            """#!/bin/sh
if [ "$1" = "--instance" ]; then
  shift 2
fi
if [ "$1" = "activewindow" ] && [ "$2" = "-j" ]; then
  printf '{"title":"Stub","class":"Editor","address":"0xbeef"}'
  exit 0
fi
exit 1
""",
            encoding="utf-8",
        )
        hyprctl_path.chmod(0o755)

        env = os.environ.copy()
        env["PATH"] = ""
        env["SCRIBE_TAP_TEST_HYPRCTL"] = str(hyprctl_path)
        env["SCRIBE_TAP_TEST_TIME_FILE"] = str(time_file)

        proc = subprocess.Popen(
            [
                str(binary),
                "--log-dir",
                str(log_dir),
                "--snapshot-dir",
                str(snap_dir),
                "--hypr-signature",
                str(sig_file),
                "--snapshot-interval",
                "0",
                "--context-refresh",
                "0",
                "--translate",
                "raw",
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
        )
        assert proc.stdin is not None

        send_key(proc.stdin, KEY_A, 1)
        send_key(proc.stdin, KEY_A, 0)
        proc.stdin.flush()
        proc.stdin.close()
        proc.wait(timeout=5)

        assert proc.returncode == 0, proc.stderr.read().decode()

        wait_for(lambda: (log_dir / "2021-01-03.jsonl").exists())
        focus_lines = (log_dir / "2021-01-03.jsonl").read_text().splitlines()
        focus_records = [json.loads(line) for line in focus_lines]
        focus_events = [rec for rec in focus_records if rec.get("event") == "focus"]
        assert focus_events, "expected focus event with fallback hyprctl"
        assert focus_events[-1].get("window") == "Stub (Editor) [0xbeef]"

    with tempfile.TemporaryDirectory() as tmp:
        log_dir = Path(tmp) / "logs"
        snap_dir = Path(tmp) / "snapshots"
        time_file = Path(tmp) / "time.txt"
        log_dir.mkdir()
        snap_dir.mkdir()

        base_dt = datetime.datetime(2021, 1, 4, 9, 0, tzinfo=datetime.timezone.utc)
        write_fake_time(time_file, base_dt, monotonic=4000.0)

        env = os.environ.copy()
        env["SCRIBE_TAP_TEST_TIME_FILE"] = str(time_file)

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
                "xkb",
                "--xkb-layout",
                "us",
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
        )
        assert proc.stdin is not None

        send_key(proc.stdin, KEY_LEFTSHIFT, 1)
        send_key(proc.stdin, KEY_A, 1)
        send_key(proc.stdin, KEY_A, 0)
        send_key(proc.stdin, KEY_LEFTSHIFT, 0)
        send_key(proc.stdin, KEY_A, 1)
        send_key(proc.stdin, KEY_A, 0)
        proc.stdin.flush()

        proc.stdin.close()
        proc.wait(timeout=5)

        assert proc.returncode == 0, proc.stderr.read().decode()

        snapshot_files = list(snap_dir.glob("*.txt"))
        assert snapshot_files, "missing snapshots in modifier regression test"
        content = snapshot_files[0].read_text(encoding="utf-8")
        assert content == "Aa", f"unexpected snapshot content: {content!r}"
        assert all(ord(ch) < 128 for ch in content), content

        wait_for(lambda: (log_dir / "2021-01-04.jsonl").exists())
        events = [json.loads(line) for line in (log_dir / "2021-01-04.jsonl").read_text().splitlines()]
        snapshots = [e for e in events if e.get("event") == "snapshot"]
        assert snapshots, "expected snapshot events"
        assert snapshots[-1]["buffer"] == "Aa", snapshots[-1]

    with tempfile.TemporaryDirectory() as tmp:
        log_dir = Path(tmp) / "logs"
        snap_dir = Path(tmp) / "snapshots"
        sig_file = Path(tmp) / "sig"
        stub_dir = Path(tmp) / "bin"
        log_dir.mkdir()
        snap_dir.mkdir()
        stub_dir.mkdir()
        sig_file.write_text("signature", encoding="utf-8")

        script_path = stub_dir / "hyprctl"
        script_path.write_text("#!/bin/sh\nexit 1\n", encoding="utf-8")
        script_path.chmod(0o755)

        env = os.environ.copy()
        env["PATH"] = f"{stub_dir}:{env.get('PATH', '')}"

        proc = subprocess.Popen(
            [
                str(binary),
                "--log-dir",
                str(log_dir),
                "--snapshot-dir",
                str(snap_dir),
                "--hypr-signature",
                str(sig_file),
                "--snapshot-interval",
                "0",
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
        )
        assert proc.stdin is not None

        send_key(proc.stdin, KEY_A, 1)
        send_key(proc.stdin, KEY_A, 0)
        proc.stdin.close()
        proc.wait(timeout=5)

        assert proc.returncode == 0, proc.stderr.read().decode()

        files = list(log_dir.glob("*.jsonl"))
        assert files, "no logs captured when hyprctl fails"
        events = [json.loads(line) for line in files[0].read_text().splitlines()]
        focus_events = [e for e in events if e.get("event") == "focus"]
        assert any(e.get("window") == "unknown" for e in focus_events), "hyprctl failure should reset context"
        press = [e for e in events if e.get("event") == "press"]
        assert all(e.get("window") == "unknown" for e in press), "press events should log unknown context on failure"

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

        send_key(proc.stdin, KEY_CAPSLOCK, 1)
        send_key(proc.stdin, KEY_CAPSLOCK, 2)
        send_key(proc.stdin, KEY_CAPSLOCK, 0)
        send_key(proc.stdin, KEY_A, 1)
        send_key(proc.stdin, KEY_A, 0)

        proc.stdin.close()
        proc.wait(timeout=5)

        assert proc.returncode == 0, proc.stderr.read().decode()

        snapshot_files = list(snap_dir.glob("*.txt"))
        assert snapshot_files, "no snapshot files for capslock repeat"
        content = snapshot_files[0].read_text()
        assert content == "A", f"capslock repeat should preserve uppercase translation, got {content!r}"

    return 0


if __name__ == "__main__":
    sys.exit(main())
