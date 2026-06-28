#!/usr/bin/env python3
import argparse
import errno
import os
import stat
import subprocess
import sys
import threading
import time

import serial

try:
    from fuse import FUSE, FuseOSError, Operations
except ImportError:  # Allow --check-serial without fusepy installed.
    FUSE = None
    FuseOSError = OSError
    Operations = object


class Tab5Serial:
    def __init__(self, port_name: str, baud: int, timeout: float = 5.0):
        self.lock = threading.Lock()
        self.timeout = timeout
        self.port = serial.Serial(port_name, baudrate=baud, timeout=0.1, write_timeout=1)
        self.port.dtr = False
        self.port.rts = False
        time.sleep(0.2)
        self.port.reset_input_buffer()

    def command(self, line: str):
        with self.lock:
            self.port.write((line + "\r\n").encode("utf-8"))
            self.port.flush()
            deadline = time.time() + self.timeout
            lines = []
            while time.time() < deadline:
                raw = self.port.readline()
                if not raw:
                    continue
                text = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                if text.startswith("> "):
                    continue
                lines.append(text)
                if text == "OK" or text.startswith("OK ") or text.startswith("ERR "):
                    return lines
            raise TimeoutError(line)

    def close(self):
        self.port.close()


def parse_kv(line: str):
    result = {}
    for item in line.split():
        if "=" in item:
            key, value = item.split("=", 1)
            result[key] = value
    return result


class Tab5Fuse(Operations):
    def __init__(self, transport: Tab5Serial, volume: str):
        self.transport = transport
        self.volume = volume

    def _spec(self, path: str) -> str:
        return f"{self.volume}:{path or '/'}"

    def _stat(self, path: str):
        lines = self.transport.command(f"fs stat {self._spec(path)}")
        last = lines[-1]
        if last.startswith("ERR "):
            raise FuseOSError(errno.ENOENT)
        data = parse_kv(last)
        mode = int(data.get("mode", "755" if data.get("type") == "dir" else "644"), 8)
        if data.get("type") == "dir":
            mode |= stat.S_IFDIR
            size = 0
        else:
            mode |= stat.S_IFREG
            size = int(data.get("size", "0"))
        mtime = int(data.get("mtime", str(int(time.time()))))
        return {
            "st_mode": mode,
            "st_nlink": 2 if data.get("type") == "dir" else 1,
            "st_size": size,
            "st_ctime": mtime,
            "st_mtime": mtime,
            "st_atime": mtime,
            "st_uid": os.getuid(),
            "st_gid": os.getgid(),
        }

    def getattr(self, path, fh=None):
        if path == "/":
            return self._stat("/")
        return self._stat(path)

    def readdir(self, path, fh):
        yield "."
        yield ".."
        lines = self.transport.command(f"fs list {self._spec(path)}")
        if lines[-1].startswith("ERR "):
            raise FuseOSError(errno.ENOENT)
        for line in lines:
            if line.startswith("ITEM "):
                parts = line.split(" ", 5)
                if len(parts) == 6:
                    yield parts[5]

    def read(self, path, size, offset, fh):
        lines = self.transport.command(f"fs read {self._spec(path)} {offset} {min(size, 512)}")
        if lines[-1].startswith("ERR "):
            raise FuseOSError(errno.EIO)
        payload = ""
        for line in lines:
            if line.startswith("DATA "):
                payload = line[5:].strip()
                break
        return bytes.fromhex(payload)

    def write(self, path, data, offset, fh):
        total = 0
        while total < len(data):
            chunk = data[total:total + 512]
            lines = self.transport.command(f"fs write {self._spec(path)} {offset + total} {chunk.hex()}")
            if lines[-1].startswith("ERR "):
                raise FuseOSError(errno.EIO)
            total += len(chunk)
        return total

    def create(self, path, mode, fi=None):
        self.transport.command(f"fs write {self._spec(path)} 0 ")
        return 0

    def truncate(self, path, length, fh=None):
        if length == 0:
            self.transport.command(f"fs write {self._spec(path)} 0 ")
            return 0
        raise FuseOSError(errno.ENOTSUP)

    def mkdir(self, path, mode):
        lines = self.transport.command(f"fs mkdir {self._spec(path)}")
        if lines[-1].startswith("ERR "):
            raise FuseOSError(errno.EIO)

    def unlink(self, path):
        lines = self.transport.command(f"fs rm {self._spec(path)}")
        if lines[-1].startswith("ERR "):
            raise FuseOSError(errno.EIO)

    def rmdir(self, path):
        lines = self.transport.command(f"fs rmdir {self._spec(path)}")
        if lines[-1].startswith("ERR "):
            raise FuseOSError(errno.EIO)


def ensure_fuse_available():
    if FUSE is None:
        raise SystemExit("fusepy is required: python3 -m pip install --user fusepy")


def main() -> int:
    parser = argparse.ArgumentParser(description="Mount Tab5 storage through the firmware Serial API.")
    parser.add_argument("mountpoint", nargs="?")
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--volume", default="sd", choices=["sd", "usb"])
    parser.add_argument("--check-serial", action="store_true")
    args = parser.parse_args()

    transport = Tab5Serial(args.port, args.baud)
    try:
        if args.check_serial:
            for line in transport.command("fs volumes"):
                print(line)
            return 0
        if not args.mountpoint:
            parser.error("mountpoint is required unless --check-serial is used")
        ensure_fuse_available()
        os.makedirs(args.mountpoint, exist_ok=True)
        FUSE(Tab5Fuse(transport, args.volume), args.mountpoint, foreground=True, nothreads=True)
    finally:
        transport.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
