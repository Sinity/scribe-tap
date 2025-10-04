#!/usr/bin/env python3
"""Micro benchmarks for scribe-tap throughput."""

import argparse
import statistics as stats
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

KEY_A = 30
KEY_SPACE = 57
KEY_ENTER = 28
KEY_BACKSPACE = 14
EV_KEY = 0x01
EV_SYN = 0x00


def pack_event(code: int, value: int) -> bytes:
    return struct.pack("llHHI", 0, 0, EV_KEY, code, value & 0xFFFFFFFF)


def syn() -> bytes:
    return struct.pack("llHHI", 0, 0, EV_SYN, 0, 0)


def build_payload(strokes: int, wrap: int) -> bytes:
    chunks = bytearray()
    for i in range(strokes):
        code = KEY_A if i % 2 == 0 else KEY_SPACE
        chunks += pack_event(code, 1)
        chunks += syn()
        chunks += pack_event(code, 0)
        chunks += syn()
        if wrap and (i + 1) % wrap == 0:
            chunks += pack_event(KEY_ENTER, 1) + syn()
            chunks += pack_event(KEY_ENTER, 0) + syn()
            chunks += pack_event(KEY_BACKSPACE, 1) + syn()
            chunks += pack_event(KEY_BACKSPACE, 0) + syn()
    return bytes(chunks)


CASES = {
    "raw-events": ["--log-mode", "events", "--translate", "raw"],
    "raw-both": ["--log-mode", "both", "--translate", "raw", "--snapshot-interval", "0.2"],
    "xkb-events": ["--log-mode", "events", "--translate", "xkb", "--xkb-layout", "us"],
    "xkb-both": ["--log-mode", "both", "--translate", "xkb", "--xkb-layout", "us", "--snapshot-interval", "0.2"],
}


def run_case(binary: Path, name: str, payload: bytes, key_count: int, context_flags: list[str]) -> dict:
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = Path(tmpdir)
        log_dir = tmp / "logs"
        snap_dir = tmp / "snapshots"
        data_dir = tmp / "state"
        log_dir.mkdir()
        snap_dir.mkdir()
        data_dir.mkdir()

        cmd = [
            str(binary),
            "--data-dir",
            str(data_dir),
            "--log-dir",
            str(log_dir),
            "--snapshot-dir",
            str(snap_dir),
            "--context",
            "none",
            "--clipboard",
            "off",
        ] + context_flags

        start = time.perf_counter()
        proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        assert proc.stdin is not None
        try:
            proc.stdin.write(payload)
        finally:
            proc.stdin.close()
        proc.wait()
        elapsed = time.perf_counter() - start
        stderr = proc.stderr.read().decode().strip()
        if proc.returncode != 0:
            raise RuntimeError(f"{name} failed ({proc.returncode}): {stderr}")
        return {
            "name": name,
            "seconds": elapsed,
            "keys_per_second": key_count / elapsed,
            "stderr": stderr,
        }


def format_results(results: list[dict]) -> str:
    lines = ["case\tkeystrokes/s\tseconds"]
    for entry in results:
        lines.append(f"{entry['name']}\t{entry['keys_per_second']:,.0f}\t{entry['seconds']:.3f}")
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path(__file__).resolve().parents[1] / "scribe-tap")
    parser.add_argument("--count", type=int, default=100_000, help="Number of keystrokes to replay")
    parser.add_argument("--wrap", type=int, default=120, help="Insert newline/backspace every N keystrokes for snapshot churn")
    parser.add_argument(
        "--cases",
        nargs="*",
        choices=sorted(CASES.keys()),
        help="Subset of benchmark cases to execute",
    )
    args = parser.parse_args()

    if not args.binary.exists():
        sys.exit(f"Binary not found: {args.binary}")

    payload = build_payload(args.count, args.wrap)

    selected = args.cases or sorted(CASES.keys())

    samples = []
    for name in selected:
        context_flags = CASES[name]
        results = [run_case(args.binary, name, payload, args.count, context_flags) for _ in range(3)]
        seconds = [r["seconds"] for r in results]
        keys = [r["keys_per_second"] for r in results]
        samples.append(
            {
                "name": name,
                "seconds": stats.mean(seconds),
                "keys_per_second": stats.mean(keys),
            }
        )

    print(format_results(samples))


if __name__ == "__main__":
    main()
