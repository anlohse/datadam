"""Crash check: what a server acknowledged must be there after it crashes and starts again.

  python crash_check.py write <out-dir> <seconds> [workers] [pipeline] [churn]
      Each worker (a process of its own) sends a numbered sequence of operations
      on keys of its own, `pipeline` at a time, and records the highest one
      acknowledged, with its time: op n is, by n % 6, SET <run>:s:<w>:<n>, HSET
      <run>:h:<w>:<g> f<n>, RPUSH <run>:l:<w>:<g>, SADD <run>:t:<w>:<g>, ZADD
      <run>:z:<w>:<g> n m<n> or INCR <run>:c:<w> (g: one of 64 keys per kind).
      `churn` more processes overwrite random keys with 1 KB values, untracked,
      to keep the storage flushing and compacting. Everything stops when the
      server goes away (the crash) or after `seconds`.
  python crash_check.py verify <out-dir> [allow-loss] [final]
      Waits for the server, reads back every worker's keys and checks that
      - what survived is a prefix of each worker's sequence (no holes: the WAL
        is replayed in order),
      - nothing is there that was never sent, and every value is the one written,
      - every collection agrees with its own count (HLEN, LLEN, SCARD, ZCARD),
      - no acknowledged operation is lost (unless allow-loss: then it reports how
        many were, and the time they span).
      final: writes verify-final.json instead of verify.json (a later check).
      Exits 1 when any of that fails.

env: REDIS_PASSWORD; HOST, PORT (default 6379); RUN (a key prefix).
out-dir gets: acks-<worker>.txt ("<unix ms> <highest acknowledged op>", one line
per pipeline), summary.json (write) and verify.json (verify).
"""
import json
import multiprocessing
import os
import random
import sys
import time

from ha_check import Conn, RespError, now_ms

HOST = os.environ.get("HOST", "127.0.0.1")
PORT = int(os.environ.get("PORT", "6379"))
RUN = os.environ.get("RUN", "crash")
GROUPS = 64  # keys per collection kind and worker
CHURN_KEYS = 20_000_000


def op(w, n):
    kind, g = n % 6, (n // 6) % GROUPS
    if kind == 0:
        return ("SET", f"{RUN}:s:{w}:{n}", n)
    if kind == 1:
        return ("HSET", f"{RUN}:h:{w}:{g}", f"f{n}", n)
    if kind == 2:
        return ("RPUSH", f"{RUN}:l:{w}:{g}", n)
    if kind == 3:
        return ("SADD", f"{RUN}:t:{w}:{g}", n)
    if kind == 4:
        return ("ZADD", f"{RUN}:z:{w}:{g}", n, f"m{n}")
    return ("INCR", f"{RUN}:c:{w}")


def reply_ok(n, r):
    kind = n % 6
    if kind == 0:
        return r == "OK"
    if kind in (1, 3, 4):
        return r == 1
    if kind == 2:
        return isinstance(r, int) and r >= 1
    return r == n // 6 + 1


def worker(w, out, deadline, pipeline, results):
    acked = sent = -1
    error = None
    wrong = False
    with open(os.path.join(out, f"acks-{w}.txt"), "w") as acks:
        try:
            c = Conn(HOST, PORT, timeout=5)
            n = 0
            flushed = time.time()
            while time.time() < deadline:
                c.send(*[op(w, i) for i in range(n, n + pipeline)])
                sent = n + pipeline - 1
                for i in range(n, n + pipeline):
                    r = c.read()
                    if isinstance(r, RespError) or not reply_ok(i, r):
                        wrong = True
                        raise OSError(f"op {i}: unexpected reply {r!r}")
                    acked = i
                acks.write(f"{now_ms()} {acked}\n")
                n += pipeline
                if time.time() - flushed > 0.2:
                    acks.flush()
                    flushed = time.time()
        except OSError as e:  # the server went away (the crash), or answered wrong
            error = str(e)
    results.put({"worker": w, "acked": acked, "sent": sent, "error": error, "wrong_reply": wrong,
                 "error_ms": now_ms() if error else None})


def churn(deadline):
    rnd = random.Random()
    try:
        c = Conn(HOST, PORT, timeout=5)
        while time.time() < deadline:
            c.send(*[("SET", f"{RUN}:churn:{rnd.randrange(CHURN_KEYS)}", os.urandom(1024)) for _ in range(100)])
            for _ in range(100):
                c.read()
    except OSError:
        pass


def write(out, seconds, workers, pipeline, churners):
    os.makedirs(out, exist_ok=True)
    started = now_ms()
    deadline = time.time() + seconds
    results = multiprocessing.Queue()
    procs = [multiprocessing.Process(target=worker, args=(w, out, deadline, pipeline, results)) for w in range(workers)]
    procs += [multiprocessing.Process(target=churn, args=(deadline,)) for _ in range(churners)]
    for p in procs:
        p.start()
    done = [results.get() for _ in range(workers)]
    for p in procs:
        p.join()
    done.sort(key=lambda d: d["worker"])
    acked = sum(d["acked"] + 1 for d in done)
    summary = {"run": RUN, "started_ms": started, "seconds": seconds, "pipeline": pipeline, "churn": churners,
               "ended_ms": now_ms(), "acked": acked, "workers": done,
               "wrong_replies": sum(d["wrong_reply"] for d in done)}
    summary["acked_per_s"] = round(acked / max(0.001, (summary["ended_ms"] - started) / 1000))
    with open(os.path.join(out, "summary.json"), "w") as f:
        json.dump(summary, f, indent=1)
    print(json.dumps({k: v for k, v in summary.items() if k != "workers"}))


def wait_for_server(limit_s=1800):
    deadline = time.time() + limit_s
    while True:
        try:
            c = Conn(HOST, PORT, timeout=5)
            r = c.call("GET", f"{RUN}:probe")
            c.close()
            if not isinstance(r, RespError):
                return
        except OSError:
            pass
        if time.time() >= deadline:
            raise SystemExit("the server did not come back")
        time.sleep(0.5)


def ack_times(out, w):
    """(time, highest acknowledged op) lines of a worker, in order."""
    with open(os.path.join(out, f"acks-{w}.txt")) as f:
        return [tuple(map(int, line.split())) for line in f if line.strip()]


def check_worker(c, w, acked, sent, pipeline):
    """What of worker w's sequence is there: a list of flags 0..sent, and problems found."""
    n_max = sent + 6 * pipeline  # a little past what was sent: none of it may be there
    present = bytearray(n_max + 1)
    problems = {"wrong": 0, "phantom": 0, "inconsistent": 0}
    first_problem = []

    def problem(kind, what):
        problems[kind] += 1
        if len(first_problem) < 5:
            first_problem.append(f"{kind}: {what}")

    def mark(n, kind, key):
        if n % 6 != kind or (kind != 0 and (n // 6) % GROUPS != key):
            problem("wrong", f"op {n} in the wrong key")
        elif n > sent:
            problem("phantom", f"op {n} was never sent")
        else:
            present[n] = 1

    strings = list(range(0, n_max + 1, 6))
    for i in range(0, len(strings), 1000):
        chunk = strings[i:i + 1000]
        c.send(*[("GET", f"{RUN}:s:{w}:{n}") for n in chunk])
        for n in chunk:
            v = c.read()
            if v is None:
                continue
            if v != str(n):
                problem("wrong", f"s:{w}:{n} = {v!r}")
            else:
                mark(n, 0, 0)
    for g in range(GROUPS):
        h, l, t, z = (f"{RUN}:{k}:{w}:{g}" for k in "hltz")
        c.send(("HGETALL", h), ("HLEN", h), ("LRANGE", l, 0, -1), ("LLEN", l), ("SMEMBERS", t), ("SCARD", t),
               ("ZRANGE", z, 0, -1, "WITHSCORES"), ("ZCARD", z))
        fields, hlen, items, llen, members, scard, scored, zcard = (c.read() for _ in range(8))
        if len(fields) // 2 != hlen:
            problem("inconsistent", f"{h}: HLEN {hlen}, {len(fields) // 2} fields")
        for f, v in zip(fields[::2], fields[1::2]):
            if f != f"f{v}":
                problem("wrong", f"{h} {f} = {v!r}")
            else:
                mark(int(v), 1, g)
        if len(items) != llen:
            problem("inconsistent", f"{l}: LLEN {llen}, {len(items)} items")
        if [int(x) for x in items] != sorted(int(x) for x in items):
            problem("wrong", f"{l} out of order")
        for x in items:
            mark(int(x), 2, g)
        if len(members) != scard:
            problem("inconsistent", f"{t}: SCARD {scard}, {len(members)} members")
        for x in members:
            mark(int(x), 3, g)
        if len(scored) // 2 != zcard:
            problem("inconsistent", f"{z}: ZCARD {zcard}, {len(scored) // 2} members")
        for m, s in zip(scored[::2], scored[1::2]):
            if m != f"m{int(float(s))}":
                problem("wrong", f"{z} {m} scored {s}")
            else:
                mark(int(float(s)), 4, g)
    count = c.call("GET", f"{RUN}:c:{w}")
    count = int(count) if count is not None else 0
    for n in range(5, n_max + 1, 6):
        if n // 6 < count:
            mark(n, 5, (n // 6) % GROUPS)
    return present[:sent + 1] if sent >= 0 else bytearray(), problems, first_problem


def verify(out, allow_loss, final):
    with open(os.path.join(out, "summary.json")) as f:
        summary = json.load(f)
    started = now_ms()
    wait_for_server()
    result = {"run": RUN, "waited_ms": now_ms() - started, "acked": summary["acked"], "workers": []}
    c = Conn(HOST, PORT, timeout=60)
    totals = {"lost": 0, "holes": 0, "wrong": 0, "phantom": 0, "inconsistent": 0, "lost_span_ms": 0}
    for wk in summary["workers"]:
        w, acked, sent = wk["worker"], wk["acked"], wk["sent"]
        present, problems, first = check_worker(c, w, acked, sent, summary["pipeline"])
        missing = present.find(0)
        prefix = len(present) - 1 if missing < 0 else missing - 1  # every op up to here is there
        holes = present.count(1, prefix + 2) if missing >= 0 else 0  # there, after one that is not
        lost = present[:acked + 1].count(0)  # acknowledged, and not there
        span = 0
        if lost:
            times = ack_times(out, w)
            first_lost = next(t for t, n in times if n > prefix)
            span = times[-1][0] - first_lost  # acknowledged history lost, from the first lost to the last ack
        entry = {"worker": w, "acked": acked, "sent": sent, "prefix": prefix, "holes": holes, "lost": lost,
                 "lost_span_ms": span, **problems, "first_problems": first}
        result["workers"].append(entry)
        for k in ("lost", "holes", "wrong", "phantom", "inconsistent"):
            totals[k] += entry[k]
        totals["lost_span_ms"] = max(totals["lost_span_ms"], span)
    c.close()
    result.update(totals)
    result["ok"] = (totals["holes"] == 0 and totals["wrong"] == 0 and totals["phantom"] == 0 and
                    totals["inconsistent"] == 0 and (allow_loss or totals["lost"] == 0))
    with open(os.path.join(out, "verify-final.json" if final else "verify.json"), "w") as f:
        json.dump(result, f, indent=1)
    print(json.dumps({k: v for k, v in result.items() if k != "workers"}))
    return result["ok"]


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("write", "verify"):
        sys.exit(__doc__)
    out = sys.argv[2]
    if sys.argv[1] == "write":
        args = [int(a) for a in sys.argv[3:7]]
        seconds, workers, pipeline, churners = (args + [60, 8, 16, 0][len(args):])[:4]
        write(out, seconds, workers, pipeline, churners)
    else:
        if not verify(out, "allow-loss" in sys.argv[3:], "final" in sys.argv[3:]):
            sys.exit(1)


if __name__ == "__main__":
    main()
