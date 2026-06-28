#!/usr/bin/env python3
import errno
import os
import stat
import sys
import threading
import time

RPC_CHUNK_SIZE = 2048

try:
    from fuse import FUSE, FuseOSError, Operations
except Exception as exc:
    sys.stderr.write("fuse import failed: %s\n" % exc)
    sys.exit(70)


def enc(text):
    return text.encode("utf-8", "surrogateescape").hex()


def dec(text):
    return bytes.fromhex(text).decode("utf-8", "surrogateescape")


class Rpc:
    def __init__(self, default_volume, input_path=None, output_path=None):
        self.default_volume = default_volume
        self.lock = threading.Lock()
        self.next_id = 1
        self.input = open(input_path, "r", encoding="utf-8", errors="surrogateescape") if input_path else sys.stdin
        self.output = open(output_path, "w", encoding="utf-8", errors="surrogateescape", buffering=1) if output_path else sys.stdout

    def call(self, op, volume, path, *args):
        with self.lock:
            req_id = self.next_id
            self.next_id += 1
            fields = ["REQ", str(req_id), op, volume, enc(path)] + [str(arg) for arg in args]
            self.output.write("\t".join(fields) + "\n")
            self.output.flush()
            while True:
                line = self.input.readline()
                if not line:
                    raise FuseOSError(errno.EIO)
                parts = line.rstrip("\n").split("\t")
                if len(parts) >= 4 and parts[0] == "RSP" and parts[1] == str(req_id):
                    if parts[2] == "OK":
                        return parts[3:]
                    if len(parts) >= 4:
                        raise FuseOSError(int(parts[3]))
                    raise FuseOSError(errno.EIO)


class Tab5Fs(Operations):
    def __init__(self, volume, input_path=None, output_path=None):
        self.volume = volume
        self.rpc = Rpc(volume, input_path, output_path)
        self.home = os.path.expanduser("~")

    def _local_path(self, path):
        rel = os.path.normpath("/" + path.lstrip("/")).lstrip("/")
        if not rel or rel == ".":
            return self.home
        return os.path.join(self.home, rel)

    def _target(self, path):
        if self.volume != "all":
            return self.volume, path
        if path == "/":
            return "", "/"
        parts = path.strip("/").split("/", 1)
        if parts[0] not in ("sd", "usb"):
            return "__local__", self._local_path(path)
        return parts[0], "/" + (parts[1] if len(parts) > 1 else "")

    def _local_getattr(self, local_path):
        try:
            st = os.lstat(local_path)
        except FileNotFoundError:
            raise FuseOSError(errno.ENOENT)
        return {
            "st_mode": st.st_mode,
            "st_nlink": st.st_nlink,
            "st_size": st.st_size,
            "st_ctime": int(st.st_ctime),
            "st_mtime": int(st.st_mtime),
            "st_atime": int(st.st_atime),
            "st_uid": st.st_uid,
            "st_gid": st.st_gid,
        }

    def getattr(self, path, fh=None):
        volume, subpath = self._target(path)
        if self.volume == "all" and path in ("/", "/sd", "/usb"):
            now = int(time.time())
            return {"st_mode": stat.S_IFDIR | 0o755, "st_nlink": 2, "st_size": 0, "st_ctime": now, "st_mtime": now, "st_atime": now}
        if volume == "__local__":
            return self._local_getattr(subpath)
        res = self.rpc.call("stat", volume, subpath)
        if len(res) < 5 or res[0] != "STAT":
            raise FuseOSError(errno.EIO)
        is_dir = res[1] == "dir"
        mode = int(res[3], 8)
        mtime = int(res[4])
        return {
            "st_mode": (stat.S_IFDIR if is_dir else stat.S_IFREG) | mode,
            "st_nlink": 2 if is_dir else 1,
            "st_size": int(res[2]),
            "st_ctime": mtime,
            "st_mtime": mtime,
            "st_atime": mtime,
        }

    def readdir(self, path, fh):
        yield "."
        yield ".."
        volume, subpath = self._target(path)
        if self.volume == "all" and path == "/":
            yield "sd"
            yield "usb"
            try:
                for name in os.listdir(self.home):
                    if name not in ("sd", "usb"):
                        yield name
            except OSError:
                pass
            return
        if volume == "__local__":
            try:
                for name in os.listdir(subpath):
                    yield name
            except FileNotFoundError:
                raise FuseOSError(errno.ENOENT)
            except NotADirectoryError:
                raise FuseOSError(errno.ENOTDIR)
            return
        if self.volume == "all" and path in ("/sd", "/usb"):
            subpath = "/"
        res = self.rpc.call("list", volume, subpath)
        if not res or res[0] != "LIST":
            raise FuseOSError(errno.EIO)
        if len(res) < 2 or not res[1]:
            return
        for item in res[1].split("|"):
            if item:
                yield dec(item.split(",", 1)[0])

    def open(self, path, flags):
        volume, subpath = self._target(path)
        if volume == "__local__":
            try:
                return os.open(subpath, flags)
            except OSError as exc:
                raise FuseOSError(exc.errno)
        self.getattr(path)
        return 0

    def release(self, path, fh):
        volume, _ = self._target(path)
        if volume == "__local__" and fh is not None:
            os.close(fh)

    def read(self, path, size, offset, fh):
        volume, subpath = self._target(path)
        if volume == "__local__":
            try:
                os.lseek(fh, offset, os.SEEK_SET)
                return os.read(fh, size)
            except OSError as exc:
                raise FuseOSError(exc.errno)
        chunks = []
        remaining = size
        position = offset
        while remaining > 0:
            request_size = min(remaining, RPC_CHUNK_SIZE)
            res = self.rpc.call("read", volume, subpath, position, request_size)
            if len(res) < 2 or res[0] != "DATA":
                raise FuseOSError(errno.EIO)
            chunk = bytes.fromhex(res[1])
            if not chunk:
                break
            chunks.append(chunk)
            position += len(chunk)
            remaining -= len(chunk)
            if len(chunk) < request_size:
                break
        return b"".join(chunks)

    def create(self, path, mode, fi=None):
        volume, subpath = self._target(path)
        if volume == "__local__":
            try:
                return os.open(subpath, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, mode)
            except OSError as exc:
                raise FuseOSError(exc.errno)
        self.rpc.call("write", volume, subpath, 0, "")
        return 0

    def write(self, path, data, offset, fh):
        volume, subpath = self._target(path)
        if volume == "__local__":
            try:
                os.lseek(fh, offset, os.SEEK_SET)
                return os.write(fh, data)
            except OSError as exc:
                raise FuseOSError(exc.errno)
        written = 0
        while written < len(data):
            chunk = data[written:written + RPC_CHUNK_SIZE]
            self.rpc.call("write", volume, subpath, offset + written, chunk.hex())
            written += len(chunk)
        return len(data)

    def truncate(self, path, length, fh=None):
        volume, subpath = self._target(path)
        if volume == "__local__":
            try:
                if fh:
                    os.ftruncate(fh, length)
                else:
                    with open(subpath, "r+b") as fp:
                        fp.truncate(length)
            except OSError as exc:
                raise FuseOSError(exc.errno)
            return
        self.rpc.call("truncate", volume, subpath, length)

    def mkdir(self, path, mode):
        volume, subpath = self._target(path)
        if volume == "__local__":
            try:
                os.mkdir(subpath, mode)
            except OSError as exc:
                raise FuseOSError(exc.errno)
            return
        self.rpc.call("mkdir", volume, subpath)

    def rmdir(self, path):
        volume, subpath = self._target(path)
        if volume == "__local__":
            try:
                os.rmdir(subpath)
            except OSError as exc:
                raise FuseOSError(exc.errno)
            return
        self.rpc.call("rmdir", volume, subpath)

    def unlink(self, path):
        volume, subpath = self._target(path)
        if volume == "__local__":
            try:
                os.unlink(subpath)
            except OSError as exc:
                raise FuseOSError(exc.errno)
            return
        self.rpc.call("unlink", volume, subpath)

    def rename(self, old, new):
        old_volume, old_subpath = self._target(old)
        new_volume, new_subpath = self._target(new)
        if old_volume == "__local__" and new_volume == "__local__":
            try:
                os.rename(old_subpath, new_subpath)
            except OSError as exc:
                raise FuseOSError(exc.errno)
            return
        if old_volume != new_volume:
            raise FuseOSError(errno.EXDEV)
        self.rpc.call("rename", old_volume, old_subpath, enc(new_subpath))

    def chmod(self, path, mode):
        volume, subpath = self._target(path)
        if volume == "__local__":
            try:
                os.chmod(subpath, mode)
            except OSError as exc:
                raise FuseOSError(exc.errno)

    def chown(self, path, uid, gid):
        volume, subpath = self._target(path)
        if volume == "__local__":
            try:
                os.chown(subpath, uid, gid)
            except OSError as exc:
                raise FuseOSError(exc.errno)

    def utimens(self, path, times=None):
        volume, subpath = self._target(path)
        if volume == "__local__":
            try:
                os.utime(subpath, times)
            except OSError as exc:
                raise FuseOSError(exc.errno)


def main():
    volume = "sd"
    mountpoint = os.path.expanduser("~/sd")
    input_path = None
    output_path = None
    if "--volume" in sys.argv:
        volume = sys.argv[sys.argv.index("--volume") + 1]
    if "--mount" in sys.argv:
        mountpoint = os.path.expanduser(sys.argv[sys.argv.index("--mount") + 1])
    if "--rpc-in" in sys.argv:
        input_path = os.path.expanduser(sys.argv[sys.argv.index("--rpc-in") + 1])
    if "--rpc-out" in sys.argv:
        output_path = os.path.expanduser(sys.argv[sys.argv.index("--rpc-out") + 1])
    os.makedirs(mountpoint, exist_ok=True)
    sys.stderr.write("tab5 fuse mounting %s at %s\n" % (volume, mountpoint))
    sys.stderr.flush()
    FUSE(Tab5Fs(volume, input_path, output_path), mountpoint, foreground=True, nothreads=True, allow_other=False, nonempty=True)


if __name__ == "__main__":
    main()
