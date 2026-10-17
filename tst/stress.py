#!/usr/bin/env python3
"""Stress tester for webserv.

Start the server first (in another terminal), then:

    python3 tst/stress.py                          # 50 clients, 10 s, GET /
    python3 tst/stress.py -c 200 -t 30             # 200 clients for 30 s
    python3 tst/stress.py --path /cgi/example.py   # hammer the CGI
    python3 tst/stress.py --no-keepalive           # new connection per request
    python3 tst/stress.py --port 8081

The subject asks: "Stress test your server to ensure it remains available at
all times". The important number is the availability: it must stay at 100 %
(every request gets a valid HTTP response) and the server must still answer
after the test.
"""

import argparse
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import httpclient as http  # noqa: E402

GREEN, RED, YELLOW, GRAY, BOLD, NC = (
    "\033[32m", "\033[31m", "\033[33m", "\033[90m", "\033[1m", "\033[0m")


class Stats:
    def __init__(self):
        self.lock = threading.Lock()
        self.latencies = []      # seconds, one per answered request
        self.statuses = {}       # status code -> count
        self.errors = {}         # error description -> count

    def ok(self, status, latency):
        with self.lock:
            self.latencies.append(latency)
            self.statuses[status] = self.statuses.get(status, 0) + 1

    def error(self, description):
        with self.lock:
            self.errors[description] = self.errors.get(description, 0) + 1


def client(args, stats, stop_at):
    """One simulated client: sends requests in a loop until stop_at."""
    data = http.build("GET", args.path,
                      headers={} if args.keepalive else {"Connection": "close"})
    sock, leftover = None, b""
    while time.time() < stop_at:
        try:
            if sock is None:
                sock = http.connect(args.port, timeout=args.timeout, host=args.host)
                leftover = b""
            start = time.time()
            sock.sendall(data)
            response, leftover = http.read_response(sock, leftover)
            stats.ok(response.status, time.time() - start)
            if not args.keepalive or response.header("connection").lower() == "close":
                sock.close()
                sock = None
        except Exception as e:   # noqa: BLE001
            stats.error(type(e).__name__)
            if sock is not None:
                sock.close()
            sock = None
            time.sleep(0.01)
    if sock is not None:
        sock.close()


def percentile(sorted_values, p):
    if not sorted_values:
        return 0.0
    index = min(len(sorted_values) - 1, int(len(sorted_values) * p / 100.0))
    return sorted_values[index]


def main():
    parser = argparse.ArgumentParser(description="webserv stress tester")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--path", default="/", help="path to request (default: /)")
    parser.add_argument("-c", "--clients", type=int, default=50, help="parallel clients (default: 50)")
    parser.add_argument("-t", "--time", type=float, default=10, help="duration in seconds (default: 10)")
    parser.add_argument("--timeout", type=float, default=15, help="per-request timeout (default: 15 s)")
    parser.add_argument("--no-keepalive", dest="keepalive", action="store_false",
                        help="open a new connection for every request")
    args = parser.parse_args()

    try:
        http.get(args.port, args.path).status
    except Exception as e:   # noqa: BLE001
        sys.exit("cannot reach http://%s:%d%s (%s). Is webserv running?" % (args.host, args.port, args.path, e))

    print("%swebserv stress test%s  ·  %d clients  ·  %.0f s  ·  GET %s  ·  keep-alive %s"
          % (BOLD, NC, args.clients, args.time, args.path, "on" if args.keepalive else "off"))

    stats = Stats()
    stop_at = time.time() + args.time
    threads = [threading.Thread(target=client, args=(args, stats, stop_at), daemon=True)
               for _ in range(args.clients)]
    started = time.time()
    for t in threads:
        t.start()
    while any(t.is_alive() for t in threads):
        time.sleep(1)
        with stats.lock:
            done, errors = len(stats.latencies), sum(stats.errors.values())
        sys.stdout.write("\r  %s%5.0fs%s  %d responses  ·  %d errors   "
                         % (GRAY, time.time() - started, NC, done, errors))
        sys.stdout.flush()
    elapsed = time.time() - started
    print()

    answered = len(stats.latencies)
    errors = sum(stats.errors.values())
    total = answered + errors
    availability = 100.0 * answered / total if total else 0.0
    lat = sorted(stats.latencies)

    print("\n  %-14s %d" % ("requests", total))
    print("  %-14s %.0f req/s" % ("throughput", answered / elapsed if elapsed else 0))
    color = GREEN if availability == 100.0 else RED
    print("  %-14s %s%.2f %%%s" % ("availability", color, availability, NC))
    print("  %-14s %s" % ("status codes", "  ".join("%d: %d" % kv for kv in sorted(stats.statuses.items()))))
    if stats.errors:
        print("  %-14s %s%s%s" % ("errors", RED, "  ".join("%s: %d" % kv for kv in stats.errors.items()), NC))
    print("  %-14s p50 %.1f ms  ·  p95 %.1f ms  ·  p99 %.1f ms  ·  max %.1f ms"
          % ("latency", percentile(lat, 50) * 1000, percentile(lat, 95) * 1000,
             percentile(lat, 99) * 1000, (lat[-1] if lat else 0) * 1000))

    try:
        alive = http.get(args.port, "/").status
        print("\n  %sServer still answering after the test (GET / -> %d)%s" % (GREEN, alive, NC))
    except Exception as e:   # noqa: BLE001
        print("\n  %s%sServer NOT answering after the test: %s%s" % (RED, BOLD, e, NC))
        sys.exit(1)
    sys.exit(0 if availability == 100.0 else 1)


if __name__ == "__main__":
    main()
