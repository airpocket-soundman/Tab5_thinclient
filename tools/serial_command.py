#!/usr/bin/env python3
import argparse
import sys
import time

import serial


def run_command(port_name: str, baud: int, command: str, timeout: float) -> int:
    with serial.Serial(port_name, baudrate=baud, timeout=0.1, write_timeout=1) as port:
        port.dtr = False
        port.rts = False
        time.sleep(0.2)
        port.reset_input_buffer()
        port.write((command + "\r\n").encode("utf-8"))
        port.flush()
        deadline = time.time() + timeout
        while time.time() < deadline:
            chunk = port.read(4096)
            if chunk:
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
                text = chunk.decode("utf-8", errors="ignore")
                if "\nOK" in text or "\r\nOK" in text or "\nERR" in text or "\r\nERR" in text:
                    break
            else:
                time.sleep(0.02)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Send one command to the Tab5 serial API.")
    parser.add_argument("--port", default="COM4")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = " ".join(args.command).strip()
    if not command:
        parser.error("command is required")
    return run_command(args.port, args.baud, command, args.timeout)


if __name__ == "__main__":
    raise SystemExit(main())
