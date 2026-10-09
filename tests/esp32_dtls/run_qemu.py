#!/usr/bin/env python3
"""Run ESP32 DTLS regression tests without host networking or physical hardware."""
import argparse
import json
from pathlib import Path
import selectors
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True, help="Official Espressif qemu-system-xtensa binary")
    parser.add_argument("--timeout", type=int, default=180)
    arguments = parser.parse_args()
    build = ROOT / "build"
    flash = json.loads((build / "flasher_args.json").read_text())
    image = build / "qemu-flash.bin"
    command = [sys.executable, "-m", "esptool", "--chip", "esp32", "merge-bin",
               "--output", str(image), "--fill-flash-size", "4MB"]
    for address, filename in flash["flash_files"].items():
        command.extend([address, str(build / filename)])
    subprocess.run(command, check=True, timeout=60)
    command = [arguments.qemu, "-nographic", "-machine", "esp32", "-nic", "none",
               "-drive", f"file={image},if=mtd,format=raw", "-no-reboot",
               "-global", "driver=timer.esp32.timg,property=wdt_disable,value=true"]
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    output = bytearray()
    deadline = time.monotonic() + arguments.timeout
    passed = False
    try:
        while time.monotonic() < deadline:
            if not selector.select(timeout=1):
                if process.poll() is not None:
                    break
                continue
            chunk = process.stdout.read1(4096)
            if not chunk:
                break
            output.extend(chunk)
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
            if b"PROBE: ALL PASS" in output:
                passed = True
                break
            if any(marker in output for marker in
                   (b"Guru Meditation", b"assert failed", b"PROBE: failure", b"abort() was called")):
                break
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        selector.close()
        process.stdout.close()
    if not passed:
        raise SystemExit("ESP32 QEMU probe did not pass all cases")


if __name__ == "__main__":
    main()
