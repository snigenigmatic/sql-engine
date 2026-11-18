#!/usr/bin/env python3
"""People using a cluster through the real sqlengine CLI (in pseudo-terminals).

Start a cluster first (scripts/local-cluster.sh up), then:

    scripts/cluster_users.py setup      # accounts table, 1000 rows of 1000
    scripts/cluster_users.py clerks     # two people update the same row
    scripts/cluster_users.py deadlock   # two transfers in opposite directions
    scripts/cluster_users.py chaos      # 6 people transferring money while a
                                        # storage node and the leading
                                        # coordinator are killed and restarted

Every scenario checks that the total balance does not change.
Environment: SQLENGINE (default build/src/sqlengine).
"""
import collections
import os
import pty
import random
import re
import select
import signal
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLI = os.environ.get("SQLENGINE", os.path.join(ROOT, "build/src/sqlengine"))
CLUSTER = os.path.join(ROOT, "scripts/local-cluster.sh")
PORTS = [7481, 7482, 7483]
PROMPT = re.compile(rb"sql\*?> $")


def addresses(first=0):
    order = PORTS[first:] + PORTS[:first]
    return ",".join("127.0.0.1:%d" % p for p in order)


class User:
    """One interactive sqlengine session, as a person at a terminal."""

    def __init__(self, name, first=0):
        self.name = name
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execv(CLI, [CLI, "--connect", addresses(first)])
        self.buf = b""
        if self._read(15) is None:
            raise RuntimeError("no prompt from " + name)

    def _read(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            ready, _, _ = select.select([self.fd], [], [], 0.05)
            if ready:
                try:
                    self.buf += os.read(self.fd, 65536)
                except OSError:
                    return None
                if PROMPT.search(self.buf):
                    out, self.buf = self.buf, b""
                    return out.decode(errors="replace").replace("\r", "")
        return None

    def send(self, line):
        self.started = time.time()
        os.write(self.fd, (line + "\n").encode())

    def answer(self, timeout=30):
        out = self._read(timeout)
        if out is None:
            return None
        self.in_transaction = out.rstrip().endswith("sql*>")  # the prompt says so
        return "\n".join(out.split("\n")[1:-1])  # no echo, no prompt

    def type(self, line, timeout=30, show=True):
        self.send(line)
        out = self.answer(timeout)
        if show:
            print("[%s +%.2fs] %s\n    %s" % (self.name, time.time() - self.started, line,
                                             (out or "<no answer>").replace("\n", "\n    ")))
        return out

    def kill(self):
        try:
            os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)
        except (ProcessLookupError, ChildProcessError):
            pass

    def quit(self):
        os.write(self.fd, b".quit\n")
        try:
            os.waitpid(self.pid, 0)
        except ChildProcessError:
            pass


def run(sql):
    out = subprocess.run([CLI, "--connect", addresses()], input=sql, capture_output=True, text=True,
                         timeout=60).stdout
    return out.strip()


def total():
    row = run("SELECT COUNT(*), SUM(balance) FROM accounts;\n").splitlines()[-2]
    return [cell.strip() for cell in row.split("|")]


def setup():
    run("DROP TABLE accounts;\n")
    rows = ", ".join("(%d, 1000)" % i for i in range(1, 1001))
    print(run("CREATE TABLE accounts (id INTEGER PRIMARY KEY, balance INTEGER NOT NULL);\n"
              "INSERT INTO accounts VALUES %s;\n" % rows))


def clerks():
    ben, chen = User("ben", 0), User("chen", 1)
    ben.type("BEGIN;")
    ben.type("UPDATE accounts SET balance = balance - 300 WHERE id = 7;")
    chen.send("UPDATE accounts SET balance = balance + 300 WHERE id = 7;")
    time.sleep(1)
    print("    (chen is waiting for ben's transaction)")
    ben.type("COMMIT;")
    print("[chen +%.2fs] %s" % (time.time() - chen.started, chen.answer()))
    chen.type("SELECT balance FROM accounts WHERE id = 7;")
    ben.quit()
    chen.quit()


def deadlock():
    ben, chen = User("ben", 0), User("chen", 1)
    for u in (ben, chen):
        u.type("BEGIN;")
    ben.type("UPDATE accounts SET balance = balance - 10 WHERE id = 7;")
    chen.type("UPDATE accounts SET balance = balance - 10 WHERE id = 8;")
    ben.send("UPDATE accounts SET balance = balance + 10 WHERE id = 8;")
    time.sleep(0.3)
    chen.type("UPDATE accounts SET balance = balance + 10 WHERE id = 7;")
    print("[ben +%.2fs] %s" % (time.time() - ben.started, ben.answer()))
    ben.type("COMMIT;")
    ben.quit()
    chen.quit()


def leader():
    for line in run(".help\n").splitlines():
        if "(leader)" in line:
            return int(line.split("coordinator ")[1].split(" ")[0])


def chaos():
    stats, errors = collections.Counter(), collections.Counter()
    lock = threading.Lock()
    stop = threading.Event()

    def worker(n):
        rnd, user = random.Random(n), None
        while not stop.is_set():
            try:
                if user is None:
                    user = User("w%d" % n, n % 3)
                a, b, x = rnd.randint(1, 1000), rnd.randint(1, 1000), rnd.randint(1, 50)
                if a == b:
                    continue
                for sql in ("BEGIN;", "UPDATE accounts SET balance = balance - %d WHERE id = %d;" % (x, a),
                            "UPDATE accounts SET balance = balance + %d WHERE id = %d;" % (x, b), "COMMIT;"):
                    out = user.type(sql, show=False)
                    if out is None:
                        raise RuntimeError("no answer in 30 s")
                    if out.startswith("Error"):
                        break
                with lock:
                    if out == "Transaction committed.":
                        stats["committed"] += 1
                    else:
                        stats["failed"] += 1
                        errors[out.split("\n")[0][:90]] += 1
                if user.in_transaction:
                    user.type("ROLLBACK;", show=False)
            except Exception as e:  # pylint: disable=broad-except
                with lock:
                    stats["client restarts"] += 1
                    errors["client: " + str(e)[:80]] += 1
                if user:
                    user.kill()
                user = None
                time.sleep(0.3)

    def phase(name, seconds):
        before = stats["committed"]
        time.sleep(seconds)
        print("%-36s %5d commits in %d s" % (name, stats["committed"] - before, seconds), flush=True)

    start = total()
    print("total before:", " | ".join(start))
    threads = [threading.Thread(target=worker, args=(i,)) for i in range(6)]
    for t in threads:
        t.start()
    phase("steady", 8)
    subprocess.run([CLUSTER, "kill", "node-1"], check=True)
    phase("node-1 killed (kill -9)", 8)
    coordinator = leader()
    subprocess.run([CLUSTER, "kill", "coord-%d" % coordinator], check=True)
    phase("leading coordinator %d killed" % coordinator, 10)
    subprocess.run([CLUSTER, "start", "node-1"], check=True)
    subprocess.run([CLUSTER, "start", "coord-%d" % coordinator], check=True)
    phase("both restarted", 10)
    stop.set()
    for t in threads:
        t.join()
    print("outcomes:", dict(stats))
    for message, count in errors.most_common():
        print("  %6d x %s" % (count, message))
    end = total()
    print("total after: ", " | ".join(end))
    if end != start:
        sys.exit("MONEY WAS CREATED OR LOST")


if __name__ == "__main__":
    scenarios = {"setup": setup, "clerks": clerks, "deadlock": deadlock, "chaos": chaos}
    if len(sys.argv) != 2 or sys.argv[1] not in scenarios:
        sys.exit(__doc__)
    scenarios[sys.argv[1]]()
