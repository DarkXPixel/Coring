#!/usr/bin/env python3
"""Measure local TCP echo throughput or verify one round trip per new connection."""

import argparse
from concurrent.futures import ProcessPoolExecutor
import socket
import struct
import time


def round_trip(conn, payload):
    conn.sendall(payload)
    received = bytearray(len(payload))
    remaining = memoryview(received)
    while remaining:
        size = conn.recv_into(remaining)
        if size == 0:
            raise EOFError("peer closed before the full response")
        remaining = remaining[size:]
    if received != payload:
        raise ValueError("echo response differs from request")


def connect(port):
    return socket.create_connection(("127.0.0.1", port), timeout=5)


def churn(port, count, payload):
    # Fully exchange each payload, then close abortively to avoid hitting
    # client ephemeral-port exhaustion during million-connection loopback runs.
    for _ in range(count):
        with connect(port) as conn:
            conn.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            round_trip(conn, payload)
    return count


def echo(port, deadline, payload):
    count = 0
    with connect(port) as conn:
        while time.monotonic() < deadline:
            round_trip(conn, payload)
            count += 1
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", type=int)
    parser.add_argument("--clients", type=int, default=8)
    parser.add_argument("--bytes", type=int, default=1024, dest="size")
    workload = parser.add_mutually_exclusive_group(required=True)
    workload.add_argument("--connections", type=int)
    workload.add_argument("--seconds", type=float)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or args.clients < 1 or args.size < 1:
        parser.error("port must be in 1..65535 and clients and bytes must be positive")
    if (args.connections is not None and args.connections < args.clients) or (
        args.seconds is not None and args.seconds <= 0
    ):
        parser.error("use at least one connection per client or a positive duration")

    payload = (bytes(range(256)) * ((args.size + 255) // 256))[: args.size]
    start = time.monotonic()
    with ProcessPoolExecutor(args.clients) as pool:
        if args.connections is not None:
            count, extra = divmod(args.connections, args.clients)
            tasks = [
                pool.submit(churn, args.port, count + (index < extra), payload)
                for index in range(args.clients)
            ]
        else:
            deadline = start + args.seconds
            tasks = [
                pool.submit(echo, args.port, deadline, payload)
                for _ in range(args.clients)
            ]
        completed = sum(task.result() for task in tasks)
    elapsed = time.monotonic() - start
    print(f"{completed} complete round trips in {elapsed:.3f} s "
          f"({completed / elapsed:.0f}/s, {completed * args.size * 8 / elapsed / 1e6:.2f} Mb/s echoed)")


if __name__ == "__main__":
    main()
