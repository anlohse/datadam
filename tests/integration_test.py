#!/usr/bin/env python3
"""End-to-end checks against a running datadam server (RESP + memcache).

usage: integration_test.py [--host H] [--port 6379] [--memcache-port 11211] [--phase all|write|verify]

"write" stores a marker key and "verify" checks it, so persistence can be
tested across a server restart. Uses only the standard library.
"""
import argparse
import socket
import sys
import time


class Resp:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port))
        self.buf = b""

    def _line(self):
        while b"\r\n" not in self.buf:
            self._fill()
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def _fill(self):
        chunk = self.sock.recv(65536)
        if not chunk:
            raise ConnectionError("server closed the connection")
        self.buf += chunk

    def _exact(self, n):
        while len(self.buf) < n + 2:
            self._fill()
        data, self.buf = self.buf[:n], self.buf[n + 2:]
        return data

    def read(self):
        line = self._line()
        t, rest = line[:1], line[1:]
        if t == b"+":
            return rest.decode()
        if t == b"-":
            return RuntimeError(rest.decode())
        if t == b":":
            return int(rest)
        if t == b"$":
            n = int(rest)
            return None if n < 0 else self._exact(n)
        if t in (b"*", b"%"):
            n = int(rest)
            if n < 0:
                return None
            items = [self.read() for _ in range(n * (2 if t == b"%" else 1))]
            return dict(zip(items[::2], items[1::2])) if t == b"%" else items
        if t == b"_":
            return None
        if t == b",":
            return float(rest)
        raise ValueError(f"unexpected reply {line!r}")

    @staticmethod
    def encode(*args):
        out = [b"*%d\r\n" % len(args)]
        for a in args:
            a = a if isinstance(a, bytes) else str(a).encode()
            out.append(b"$%d\r\n%s\r\n" % (len(a), a))
        return b"".join(out)

    def cmd(self, *args):
        self.sock.sendall(self.encode(*args))
        return self.read()


class Memcache:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port))
        self.f = self.sock.makefile("rb")

    def send(self, line, data=None):
        payload = line.encode() + b"\r\n"
        if data is not None:
            payload += data + b"\r\n"
        self.sock.sendall(payload)

    def line(self):
        return self.f.readline().rstrip(b"\r\n").decode()

    def get(self, *keys, cas=False):
        self.send(("gets " if cas else "get ") + " ".join(keys))
        found = {}
        while True:
            header = self.line()
            if header == "END":
                return found
            parts = header.split()
            value = self.f.read(int(parts[3]) + 2)[:-2]
            found[parts[1]] = (value, int(parts[2])) + ((int(parts[4]),) if cas else ())

    def store(self, cmd, key, value, flags=0, exptime=0, cas=None):
        extra = f" {cas}" if cas is not None else ""
        self.send(f"{cmd} {key} {flags} {exptime} {len(value)}{extra}", value)
        return self.line()


failures = 0


def check(name, got, expected):
    global failures
    if got == expected:
        print(f"  ok   {name}")
    else:
        failures += 1
        print(f"  FAIL {name}: got {got!r}, expected {expected!r}")


def redis_tests(r):
    print("RESP")
    r.cmd("FLUSHALL")
    check("PING", r.cmd("PING"), "PONG")
    check("SET", r.cmd("SET", "k", "v"), "OK")
    check("GET", r.cmd("GET", "k"), b"v")
    check("GET missing", r.cmd("GET", "nope"), None)
    check("SET NX existing", r.cmd("SET", "k", "x", "NX"), None)
    check("SET XX GET", r.cmd("SET", "k", "v2", "XX", "GET"), b"v")
    check("SET EX", r.cmd("SET", "ttl", "1", "EX", "100"), "OK")
    check("TTL", r.cmd("TTL", "ttl"), 100)
    check("PERSIST", r.cmd("PERSIST", "ttl"), 1)
    check("TTL after persist", r.cmd("TTL", "ttl"), -1)
    check("PEXPIRE", r.cmd("PEXPIRE", "ttl", 50), 1)
    time.sleep(0.1)
    check("expired key", r.cmd("GET", "ttl"), None)
    check("INCR new", r.cmd("INCR", "n"), 1)
    check("INCRBY", r.cmd("INCRBY", "n", 41), 42)
    check("DECR", r.cmd("DECR", "n"), 41)
    check("INCRBYFLOAT", r.cmd("INCRBYFLOAT", "f", "1.5"), b"1.5")
    check("INCR non-int", isinstance(r.cmd("INCR", "k"), RuntimeError), True)
    check("APPEND", r.cmd("APPEND", "s", "hello"), 5)
    check("APPEND more", r.cmd("APPEND", "s", " world"), 11)
    check("GETRANGE", r.cmd("GETRANGE", "s", 0, 4), b"hello")
    check("GETRANGE negative", r.cmd("GETRANGE", "s", -5, -1), b"world")
    check("SETRANGE", r.cmd("SETRANGE", "s", 6, "WORLD"), 11)
    check("STRLEN", r.cmd("STRLEN", "s"), 11)
    check("MSET", r.cmd("MSET", "a", "1", "b", "2"), "OK")
    check("MGET", r.cmd("MGET", "a", "b", "zz"), [b"1", b"2", None])
    check("MSETNX blocked", r.cmd("MSETNX", "b", "x", "c", "3"), 0)
    check("EXISTS", r.cmd("EXISTS", "a", "b", "c"), 2)
    check("RENAME", r.cmd("RENAME", "a", "a2"), "OK")
    check("KEYS", sorted(r.cmd("KEYS", "a*")), [b"a2"])
    check("DEL", r.cmd("DEL", "a2", "b", "missing"), 2)
    check("TYPE", r.cmd("TYPE", "s"), "string")
    check("GETDEL", r.cmd("GETDEL", "s"), b"hello WORLD")
    check("unknown command", isinstance(r.cmd("NOSUCHCMD"), RuntimeError), True)

    big = b"x" * 200_000
    check("SET big value", r.cmd("SET", "big", big), "OK")
    check("GET big value", r.cmd("GET", "big"), big)

    # Pipelining: many commands in one write.
    r.sock.sendall(b"".join(Resp.encode("SET", f"p{i}", i) for i in range(1000)))
    check("pipelined SETs", all(r.read() == "OK" for _ in range(1000)), True)
    r.sock.sendall(b"".join(Resp.encode("GET", f"p{i}") for i in range(1000)))
    check("pipelined GETs", [r.read() for _ in range(1000)] == [str(i).encode() for i in range(1000)], True)

    hello = r.cmd("HELLO", 3)
    check("HELLO 3", hello.get(b"proto"), 3)
    check("RESP3 null", r.cmd("GET", "nope"), None)
    stats = r.cmd("CACHE", "STATS")
    check("CACHE STATS has hits", stats[b"hits"] > 0, True)
    check("big value not cached", stats[b"rejected_too_large"] >= 1, True)
    r.cmd("HELLO", 2)
    check("INFO cache", b"cache_hits:" in r.cmd("INFO", "cache"), True)


def memcache_tests(m, r):
    print("memcache")
    check("set", m.store("set", "mk", b"hello", flags=7), "STORED")
    check("get", m.get("mk"), {"mk": (b"hello", 7)})
    check("add existing", m.store("add", "mk", b"x"), "NOT_STORED")
    check("replace missing", m.store("replace", "nokey", b"x"), "NOT_STORED")
    check("append", m.store("append", "mk", b"!"), "STORED")
    check("prepend", m.store("prepend", "mk", b">"), "STORED")
    check("get after append", m.get("mk")["mk"][0], b">hello!")
    cas = m.get("mk", cas=True)["mk"][2]
    check("cas ok", m.store("cas", "mk", b"v2", cas=cas), "STORED")
    check("cas stale", m.store("cas", "mk", b"v3", cas=cas), "EXISTS")
    m.send("touch mk 100")
    check("touch", m.line(), "TOUCHED")
    check("TTL via redis", 98 <= r.cmd("TTL", "mk") <= 100, True)
    check("set counter", m.store("set", "cnt", b"10"), "STORED")
    m.send("incr cnt 5")
    check("incr", m.line(), "15")
    m.send("decr cnt 100")
    check("decr floors at 0", m.line(), "0")
    m.send("delete mk")
    check("delete", m.line(), "DELETED")
    m.send("delete mk")
    check("delete missing", m.line(), "NOT_FOUND")
    check("multi get", sorted(m.get("cnt", "missing", "p1")), ["cnt", "p1"])
    m.send("set quiet 0 0 1 noreply", b"q")
    m.send("version")
    check("noreply + version", m.line().startswith("VERSION"), True)
    r.cmd("SET", "from-redis", "shared")
    check("cross-protocol redis->memcache", m.get("from-redis"), {"from-redis": (b"shared", 0)})
    m.store("set", "from-mc", b"also shared")
    check("cross-protocol memcache->redis", r.cmd("GET", "from-mc"), b"also shared")
    m.send("bogus")
    check("unknown command", m.line(), "ERROR")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=6379)
    ap.add_argument("--memcache-port", type=int, default=11211)
    ap.add_argument("--phase", default="all", choices=["all", "write", "verify"])
    a = ap.parse_args()

    r = Resp(a.host, a.port)
    if a.phase == "verify":
        print("persistence")
        check("marker survived restart", r.cmd("GET", "persist:marker"), b"still here")
        check("big value survived restart", len(r.cmd("GET", "big") or b""), 200_000)
    else:
        redis_tests(r)
        memcache_tests(Memcache(a.host, a.memcache_port), r)
        r.cmd("SET", "persist:marker", "still here")
    print("FAILED" if failures else "PASSED", f"({failures} failures)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
