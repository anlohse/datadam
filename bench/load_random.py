"""Loads keys memtier-1 .. memtier-N with values of truly random bytes.

memtier_benchmark's --random-data fills one buffer and reuses it for every
key, so its values compress almost entirely (89x with gzip). A store that
compresses (RocksDB with LZ4) would then hold a "3 GB" data set in a few
hundred MB, and the data would not really be bigger than memory. This loader
writes os.urandom() values instead: incompressible, a different one per key.

usage: python load_random.py <host> <port> <keys> <value-bytes> [processes]
env: REDIS_PASSWORD, sent with AUTH first when set
prints: loaded=<keys acknowledged> errors=<error replies> seconds=<s> first_error=<text>
"""
import multiprocessing
import os
import socket
import sys
import threading
import time

BATCH = 1000


def load_range(args):
    host, port, first, last, size = args
    s = socket.create_connection((host, port))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    password = os.environ.get("REDIS_PASSWORD", "").encode()
    if password:
        s.sendall(b"*2\r\n$4\r\nAUTH\r\n$%d\r\n%s\r\n" % (len(password), password))
        reply = b""
        while not reply.endswith(b"\r\n"):
            reply += s.recv(1)
        if not reply.startswith(b"+OK"):
            raise SystemExit(f"AUTH failed: {reply!r}")
    expected = last - first + 1
    counts = {"ok": 0, "err": 0, "first_error": ""}

    def read_replies():
        buf = b""
        seen = 0
        while seen < expected:
            chunk = s.recv(1 << 20)
            if not chunk:
                break
            buf += chunk
            lines = buf.split(b"\r\n")
            buf = lines.pop()
            for line in lines:
                seen += 1
                if line.startswith(b"-"):
                    counts["err"] += 1
                    if not counts["first_error"]:
                        counts["first_error"] = line[1:80].decode(errors="replace")
                else:
                    counts["ok"] += 1

    reader = threading.Thread(target=read_replies)
    reader.start()
    head = b"*3\r\n$3\r\nSET\r\n"
    tail = b"$%d\r\n" % size
    for start in range(first, last + 1, BATCH):
        end = min(start + BATCH, last + 1)
        values = os.urandom(size * (end - start))
        parts = []
        for i, k in enumerate(range(start, end)):
            key = b"memtier-%d" % k
            parts += [head, b"$%d\r\n" % len(key), key, b"\r\n", tail, values[i * size:(i + 1) * size], b"\r\n"]
        s.sendall(b"".join(parts))
    reader.join()
    s.close()
    return counts["ok"], counts["err"], counts["first_error"]


def main():
    host, port, keys, size = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
    procs = int(sys.argv[5]) if len(sys.argv) > 5 else 4
    step = (keys + procs - 1) // procs
    ranges = [(host, port, 1 + i * step, min(keys, (i + 1) * step), size) for i in range(procs) if 1 + i * step <= keys]
    started = time.time()
    with multiprocessing.Pool(len(ranges)) as pool:
        results = pool.map(load_range, ranges)
    ok = sum(r[0] for r in results)
    err = sum(r[1] for r in results)
    first_error = next((r[2] for r in results if r[2]), "")
    print(f"loaded={ok} errors={err} seconds={time.time() - started:.1f} first_error={first_error!r}")


if __name__ == "__main__":
    main()
