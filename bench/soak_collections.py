"""Collections that grow and shrink without end, at a steady rate, in a bounded
space: what a soak test needs to show memory or disk that is never given back.

  python soak_collections.py <seconds> [ops per second] [keys per kind]

Hashes, sets and sorted sets take members from 500 names, added or removed at
random (each stays at most 500 members); lists are pushed and trimmed to their
last 300 items; 1 key in 100 is dropped whole now and then. Values are 10 to
500 random bytes. Prints a line a minute.

env: REDIS_PASSWORD; HOST, PORT (default 6379); RUN (a key prefix, default soakc).
"""
import json
import os
import random
import sys
import time

from ha_check import Conn, RespError, now_ms

HOST = os.environ.get("HOST", "127.0.0.1")
PORT = int(os.environ.get("PORT", "6379"))
RUN = os.environ.get("RUN", "soakc")
BATCH = 100
NAMES = 500


def ops(rnd, keys):
    kind = rnd.randrange(4)
    key = f"{RUN}:{'hlsz'[kind]}:{rnd.randrange(keys)}"
    if rnd.random() < 0.0001:
        return [("DEL", key)]
    add = rnd.random() < 0.5
    member = f"m{rnd.randrange(NAMES)}"
    if kind == 0:
        return [("HSET", key, member, os.urandom(rnd.randint(10, 500)))] if add else [("HDEL", key, member)]
    if kind == 1:
        return [("RPUSH", key, os.urandom(rnd.randint(10, 500))), ("LTRIM", key, -300, -1)] if add else \
            [("LPOP", key)]
    if kind == 2:
        return [("SADD", key, member)] if add else [("SREM", key, member)]
    return [("ZADD", key, rnd.random(), member)] if add else [("ZREM", key, member)]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    seconds = float(sys.argv[1])
    rate = int(sys.argv[2]) if len(sys.argv) > 2 else 5000
    keys = int(sys.argv[3]) if len(sys.argv) > 3 else 10000
    rnd = random.Random()
    deadline = time.time() + seconds
    done = errors = 0
    started = next_report = time.time()
    conn = None
    while time.time() < deadline:
        try:
            if conn is None:
                conn = Conn(HOST, PORT, timeout=10)
            batch = [op for _ in range(BATCH) for op in ops(rnd, keys)]
            conn.send(*batch)
            for _ in batch:
                if isinstance(conn.read(), RespError):
                    errors += 1
            done += len(batch)
        except OSError:
            conn = None
            errors += 1
            time.sleep(1)
        # Keep to the rate: sleep until this many operations are due.
        ahead = done / rate - (time.time() - started)
        if ahead > 0:
            time.sleep(ahead)
        if time.time() >= next_report:
            print(json.dumps({"t": now_ms(), "ops": done, "errors": errors}), flush=True)
            next_report += 60
    print(json.dumps({"t": now_ms(), "ops": done, "errors": errors, "done": True}), flush=True)


if __name__ == "__main__":
    main()
