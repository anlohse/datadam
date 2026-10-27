"""High availability check: writes acknowledged by a replica must survive a failover.

  python ha_check.py write <out-dir> <seconds> [workers]
      Each worker sends SET ha:<run>:<worker>:<n> followed by WAIT 1 <ms> to the
      primary Sentinel names, and keeps every write a replica acknowledged (WAIT
      >= 1), with its time. On an error or an unacknowledged write it asks the
      sentinels again (it follows a failover). Meanwhile INFO replication of
      every data node is sampled every 250 ms.
  python ha_check.py verify <out-dir>
      Reads back every acknowledged write, on every data node, and counts the
      ones missing or different.

env: REDIS_PASSWORD; SENTINELS="ip:port,..." (master name: MASTER, default dd);
     NODES="name=ip,..." (the data nodes, port 6379); RUN (a key prefix);
     WAIT_MS (WAIT's timeout, default 500).
out-dir gets: acks-<worker>.txt ("<unix ms> <key>"), events.jsonl (errors,
unacknowledged writes, primary changes), replication.jsonl (the samples),
summary.json (write) and verify.json (verify).
"""
import json
import os
import socket
import sys
import threading
import time

PASSWORD = os.environ.get("REDIS_PASSWORD", "")
MASTER = os.environ.get("MASTER", "dd")
WAIT_MS = int(os.environ.get("WAIT_MS", "500"))
RUN = os.environ.get("RUN", "ha")


def now_ms():
    return int(time.time() * 1000)


class RespError(str):
    pass


class Conn:
    """A blocking RESP2 connection; error replies come back as RespError."""

    def __init__(self, host, port, timeout=2.0, password=PASSWORD):
        self.addr = (host, port)
        self.s = socket.create_connection((host, port), timeout=timeout)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.f = self.s.makefile("rb")
        if password:
            r = self.call("AUTH", password)
            if isinstance(r, RespError):
                raise OSError(f"AUTH failed: {r}")

    def send(self, *commands):
        out = []
        for args in commands:
            out.append(b"*%d\r\n" % len(args))
            for a in args:
                a = a if isinstance(a, bytes) else str(a).encode()
                out.append(b"$%d\r\n%s\r\n" % (len(a), a))
        self.s.sendall(b"".join(out))

    def read(self):
        line = self.f.readline()
        if not line:
            raise OSError("connection closed")
        kind, rest = line[:1], line[1:-2]
        if kind == b"+":
            return rest.decode()
        if kind == b"-":
            return RespError(rest.decode(errors="replace"))
        if kind == b":":
            return int(rest)
        if kind == b"$":
            n = int(rest)
            if n < 0:
                return None
            data = self.f.read(n + 2)
            return data[:-2].decode(errors="replace")
        if kind == b"*":
            n = int(rest)
            return None if n < 0 else [self.read() for _ in range(n)]
        raise OSError(f"bad reply {line!r}")

    def call(self, *args):
        self.send(args)
        return self.read()

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def addr_list(env):
    out = []
    for item in filter(None, os.environ.get(env, "").split(",")):
        name, _, rest = item.rpartition("=") if "=" in item else ("", "", item)
        host, _, port = rest.partition(":")
        out.append((name or rest, host, int(port or 6379)))
    return out


class Discovery:
    """The primary, as the sentinels say; asked again at most every 200 ms."""

    def __init__(self, sentinels, events):
        self.sentinels = sentinels
        self.events = events
        self.lock = threading.Lock()
        self.current = None
        self.asked = 0.0

    def primary(self, stale=None):
        with self.lock:
            if self.current and (stale is None or stale != self.current or time.time() - self.asked < 0.2):
                return self.current
            for _, host, port in self.sentinels:
                try:
                    c = Conn(host, port, timeout=0.5, password="")
                    r = c.call("SENTINEL", "get-master-addr-by-name", MASTER)
                    c.close()
                except OSError:
                    continue
                if isinstance(r, list) and len(r) == 2:
                    found = (r[0], int(r[1]))
                    if found != self.current:
                        self.events.add(kind="primary", old=self.current and self.current[0], new=found[0])
                    self.current = found
                    self.asked = time.time()
                    return found
            self.asked = time.time()
            return self.current


class Events:
    def __init__(self, path):
        self.f = open(path, "w")
        self.lock = threading.Lock()

    def add(self, **e):
        e["t"] = now_ms()
        with self.lock:
            self.f.write(json.dumps(e) + "\n")
            self.f.flush()


FIELDS = ("role", "master_host", "master_link_status", "master_sync_in_progress", "slave_repl_offset",
          "master_repl_offset", "repl_offset", "connected_slaves", "full_syncs", "partial_syncs",
          "full_syncs_served", "partial_syncs_served")


def monitor(nodes, path, stop):
    conns = {}
    with open(path, "w") as f:
        while not stop.is_set():
            t = now_ms()
            for name, host, port in nodes:
                sample = {"t": t, "node": name}
                try:
                    if name not in conns:
                        conns[name] = Conn(host, port, timeout=0.3)
                    text = conns[name].call("INFO", "replication")
                    for line in (text or "").split("\r\n"):
                        k, _, v = line.partition(":")
                        if k in FIELDS:
                            sample[k] = int(v) if v.lstrip("-").isdigit() else v
                except OSError:
                    conns.pop(name, None)
                    sample["down"] = True
                f.write(json.dumps(sample) + "\n")
            f.flush()
            stop.wait(0.25)


def worker(w, discovery, events, deadline, acks, counts):
    seq = 0
    conn = None
    while time.time() < deadline:
        if conn is None:
            addr = discovery.primary()
            if addr is None:
                time.sleep(0.05)
                continue
            try:
                conn = Conn(*addr)
            except OSError as e:
                events.add(kind="connect", worker=w, primary=addr[0], error=str(e))
                discovery.primary(stale=addr)
                time.sleep(0.05)
                continue
        key = f"ha:{RUN}:{w}:{seq}"
        seq += 1
        try:
            conn.send(("SET", key, key), ("WAIT", 1, WAIT_MS))
            r_set, r_wait = conn.read(), conn.read()
        except (OSError, ValueError) as e:
            counts["io"] += 1
            events.add(kind="io", worker=w, primary=conn.addr[0], error=str(e))
            discovery.primary(stale=conn.addr)
            conn.close()
            conn = None
            continue
        if isinstance(r_set, RespError) or isinstance(r_wait, RespError):
            counts["error"] += 1
            events.add(kind="error", worker=w, primary=conn.addr[0], error=str(r_set if isinstance(r_set, RespError) else r_wait))
            discovery.primary(stale=conn.addr)
            conn.close()
            conn = None
            time.sleep(0.02)
            continue
        if r_wait >= 1:
            acks.append((now_ms(), key))
            counts["acked"] += 1
        else:
            # Written, but no replica has it: not counted, and maybe this is
            # no longer the primary.
            counts["unacked"] += 1
            if counts["unacked"] % 100 == 1:
                events.add(kind="unacked", worker=w, primary=conn.addr[0], total=counts["unacked"])
            addr = discovery.primary(stale=conn.addr)
            if addr != conn.addr:
                conn.close()
                conn = None
    if conn:
        conn.close()


def write(out, seconds, workers):
    os.makedirs(out, exist_ok=True)
    events = Events(os.path.join(out, "events.jsonl"))
    discovery = Discovery(addr_list("SENTINELS"), events)
    stop = threading.Event()
    mon = threading.Thread(target=monitor, args=(addr_list("NODES"), os.path.join(out, "replication.jsonl"), stop))
    mon.start()
    started = now_ms()
    deadline = time.time() + seconds
    acks = [[] for _ in range(workers)]
    counts = [{"acked": 0, "unacked": 0, "io": 0, "error": 0} for _ in range(workers)]
    threads = [threading.Thread(target=worker, args=(w, discovery, events, deadline, acks[w], counts[w]))
               for w in range(workers)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    stop.set()
    mon.join()
    for w in range(workers):
        with open(os.path.join(out, f"acks-{w}.txt"), "w") as f:
            f.writelines(f"{t} {k}\n" for t, k in acks[w])
    times = sorted(t for a in acks for t, _ in a)
    gaps = sorted(((b - a, a, b) for a, b in zip(times, times[1:])), reverse=True)[:5]
    total = {k: sum(c[k] for c in counts) for k in counts[0]}
    summary = {"started_ms": started, "seconds": seconds, "workers": workers, **total,
               "acked_per_s": round(total["acked"] / seconds), "longest_gaps_ms": [
                   {"ms": g, "from": a, "to": b} for g, a, b in gaps]}
    with open(os.path.join(out, "summary.json"), "w") as f:
        json.dump(summary, f, indent=1)
    print(json.dumps(summary))


def verify(out):
    keys = []
    for name in sorted(os.listdir(out)):
        if name.startswith("acks-"):
            with open(os.path.join(out, name)) as f:
                keys += [line.split()[1] for line in f if line.strip()]
    result = {"acked": len(keys), "nodes": {}}
    for name, host, port in addr_list("NODES"):
        try:
            c = Conn(host, port, timeout=10)
            role = c.call("ROLE")[0]
            missing = wrong = 0
            first = []
            for i in range(0, len(keys), 1000):
                batch = keys[i:i + 1000]
                c.send(*[("GET", k) for k in batch])
                for k in batch:
                    v = c.read()
                    if v is None:
                        missing += 1
                        if len(first) < 5:
                            first.append(k)
                    elif v != k:
                        wrong += 1
            c.close()
            result["nodes"][name] = {"role": role, "missing": missing, "wrong": wrong, "first_missing": first}
        except OSError as e:
            result["nodes"][name] = {"error": str(e)}
    with open(os.path.join(out, "verify.json"), "w") as f:
        json.dump(result, f, indent=1)
    print(json.dumps(result))


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("write", "verify"):
        sys.exit(__doc__)
    if sys.argv[1] == "write":
        write(sys.argv[2], float(sys.argv[3]), int(sys.argv[4]) if len(sys.argv) > 4 else 8)
    else:
        verify(sys.argv[2])


if __name__ == "__main__":
    main()
